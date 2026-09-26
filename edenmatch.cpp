// edenmatch.cpp — standalone Eden matchmaker (POSIX, single translation unit).
//
// Populates the in-game Server Browser: game servers hold an open TCP connection
// here and REGISTER; game clients connect, send LIST, get the rows, disconnect.
// Protocol and framing: see matchmaker.h (pure logic + wire notes) — this file is
// only sockets, threads, and the optional on-demand HOST spawn.
//
// Build:  part of build_server.sh  (also: c++ -std=c++17 -O2 -pthread edenmatch.cpp -o edenmatch)
// Run:    ./edenmatch [--port 27020] [--advertise-ip IP] [--trust-advertise CIDR,...] [--verbose]
//         SERVER: row width: default is the 7-field capture grammar; --prod-list emits the
//                           6-field form, --short-list the 4-field sketch form (see matchmaker.h)
//         on-demand hosting: --allow-host [--edenserver PATH] [--host-ports LO-HI] [--world DIR]
//                            [--publicip IP]
//         registry persistence: --registry-file PATH  (default eden_registry.txt; empty disables)
//
// Point a server at it with:  ./edenserver --matchmaker <edenmatch-host>[:27020]
#include "hardening.h"
#include "matchmaker.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <unordered_map>

using namespace edenmatch;

static std::atomic<bool>     g_running{true};
static std::atomic<uint64_t> g_nextConn{1};
static Registry               g_registry;
static ConnCaps               g_connCaps;    // stage 2.2: global + per-IP concurrent cap

// Connection deadlines (stage 7.21). Without them a peer that connects and sends
// nothing holds a thread and a ConnCaps slot forever, so ~11 source addresses
// exhaust MAX_CONNS_GLOBAL and every Server Browser LIST is refused.
//   PRE_VERB: how long a fresh connection has to send its first line — every legitimate
//     peer (a browsing client, a HOST request, a REGISTERing server) sends it at once.
//     Mirrors edenserver's --handshake-timeout.
//   REGISTERED: a registered server is the one peer that legitimately goes quiet, so it
//     gets the heartbeat TTL plus a little slack; past that the row is dead anyway.
// The send side gets the PRE_VERB figure too: a LIST reply can exceed the socket buffer,
// and a peer that never reads it would otherwise pin the slot from the other direction.
static const int PRE_VERB_TIMEOUT_SEC   = 15;
static const int REGISTERED_TIMEOUT_SEC = HEARTBEAT_TTL_SEC + 15;

static int         g_port         = DEFAULT_PORT;
static std::string g_advertiseIP;                 // global override for the per-peer fallback
static RowForm     g_rowForm      = RowForm::Capture7;  // SERVER: row width (stage 2.3)
static bool        g_verbose      = false;
static bool        g_allowHost    = false;
static std::string g_edenserver   = "./edenserver";
static std::string g_worldDir     = ".";
static std::string g_publicIp;                    // stage 2.4: address to advertise for a HOST spawn
static int         g_hostPortLo   = 27600;
static int         g_hostPortHi   = 27699;
static std::string g_registryFile = "eden_registry.txt";   // stage 2.2; empty disables persistence
static TrustList   g_trust;                       // stage 7.20: peers allowed to advertise another address

// Registration sockets by connection id, so a row taken over by a restarted
// server can have its stale connection closed instead of left believing it is
// still listed (stage 7.20). Only shutdown() is called from outside the owning
// thread, and only under g_regFdsMtx, which the owner also holds when it
// unregisters just before close() — so a reused fd number is never touched.
static std::mutex                       g_regFdsMtx;
static std::unordered_map<uint64_t, int> g_regFds;

static void dropDisplaced(uint64_t conn) {
    std::lock_guard<std::mutex> lk(g_regFdsMtx);
    auto it = g_regFds.find(conn);
    if (it != g_regFds.end()) shutdown(it->second, SHUT_RDWR);
}

static int64_t monoSeconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(steady_clock::now().time_since_epoch()).count();
}

static void logline(const std::string& s) {
    std::cout << "[edenmatch] " << s << std::endl;
}
static void vlog(const std::string& s) {
    if (g_verbose) logline(s);
}

// SO_RCVTIMEO makes recv() fail with EAGAIN after `secs` of silence, which readLine
// reports as a closed connection — the caller's normal cleanup path handles it.
static void setRecvTimeout(int fd, int secs) {
    struct timeval tv{ secs, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}
static void setSendTimeout(int fd, int secs) {
    struct timeval tv{ secs, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

// Send an entire buffer, looping over partial writes.
static bool sendAll(int fd, const std::string& buf) {
    size_t off = 0;
    while (off < buf.size()) {
        ssize_t n = send(fd, buf.data() + off, buf.size() - off, 0);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

// Read one '\n'-terminated line into `line` (without the newline). Returns false
// on EOF/error. `carry` holds bytes already read past a previous line. A line
// longer than MAX_LINE is treated as a protocol violation (returns false).
static bool readLine(int fd, std::string& carry, std::string& line) {
    for (;;) {
        size_t nl = carry.find('\n');
        if (nl != std::string::npos) {
            line = carry.substr(0, nl);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            carry.erase(0, nl + 1);
            return true;
        }
        if (carry.size() > MAX_LINE) return false;
        char buf[1024];
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        carry.append(buf, static_cast<size_t>(n));
    }
}

static std::string peerAddr(const sockaddr_in& sa) {
    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &sa.sin_addr, ip, sizeof(ip));
    return ip;
}

// RAII release of a ConnCaps slot acquired by the accept loop, covering every
// exit path out of handleConn (LIST reply, HOST reply, REGISTER drop, error).
struct ConnCapGuard {
    std::string ip;
    explicit ConnCapGuard(std::string ip_) : ip(std::move(ip_)) {}
    ~ConnCapGuard() { g_connCaps.release(ip); }
};

// Is a server still actually there? TCP-connect its advertised address with a
// short timeout — the injected "is this address reachable" predicate sweep()
// probes stale entries with before reaping them (stage 2.2, "alive ⇒ listed").
// Non-blocking connect + poll() so one unreachable/firewalled peer can't stall
// the sweeper for the OS connect timeout (which can be minutes).
static bool probeReachable(const std::string& ip, int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return false;
    fcntl(s, F_SETFD, FD_CLOEXEC);
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) { close(s); return false; }

    bool ok = false;
    int rc = connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc == 0) {
        ok = true;
    } else if (errno == EINPROGRESS) {
        pollfd pfd{s, POLLOUT, 0};
        if (poll(&pfd, 1, 2000) > 0 && (pfd.revents & POLLOUT)) {
            int err = 0;
            socklen_t len = sizeof(err);
            ok = getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0;
        }
    }
    close(s);
    return ok;
}

// Write `content` to `path` via temp file + rename, so a crash mid-write can
// never leave a truncated registry file (same pattern as the server's
// world/player saves). Returns false on error.
static bool writeFileAtomic(const std::string& path, const std::string& content) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) return false;
        f << content;
        f.flush();
        if (!f) return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

// Persist the live registry to g_registryFile (stage 2.2) so a matchmaker
// restart reloads it instead of blanking the browser until every server's next
// reconnect. Called from the sweeper's ~10 s tick; a no-op if disabled.
static void persistRegistry() {
    if (g_registryFile.empty()) return;
    if (!writeFileAtomic(g_registryFile, serialize_registry(g_registry.snapshot())))
        logline("warning: failed to persist " + g_registryFile);
}

// ---- on-demand HOST -------------------------------------------------------

// True if some registered server is already using this loopback port — cheap way
// to avoid handing the same port to two concurrent HOST requests before either
// has registered.
static bool hostPortBusy(int port) {
    for (const auto& r : g_registry.snapshot())
        if (r.port == port) return true;
    return false;
}

// Try to bind (and immediately release) a port to check it is free.
static bool portFree(int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return false;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<uint16_t>(port));
    bool ok = bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
    close(s);
    return ok;
}

static int allocHostPort() {
    for (int p = g_hostPortLo; p <= g_hostPortHi; ++p)
        if (!hostPortBusy(p) && portFree(p)) return p;
    return -1;
}

// Fork+exec an edenserver for a HOST request and wait (up to ~30 s) for it to
// register back. Returns the reply line to send the client. `peerIp` is the
// HOST requester's address — the last fallback for the address we advertise
// for the spawned server when neither --publicip nor --advertise-ip is set.
static std::string handleHost(const std::string& line, const std::string& peerIp) {
    if (!g_allowHost) return "HOSTFAIL:spawn";

    // HOST:name[:hasPassword[:password]]
    std::vector<std::string> f;
    { std::stringstream ss(line); std::string t; while (std::getline(ss, t, ':')) f.push_back(t); }
    std::string name = f.size() >= 2 ? sanitize_name(f[1]) : std::string();
    if (name.empty()) return "HOSTFAIL:spawn";
    bool hasPw = f.size() >= 3 && !f[2].empty() && f[2] != "0";
    std::string pw = f.size() >= 4 ? f[3] : std::string();

    // Join-existing: a HOST for a name that's already live returns the
    // running server instead of spawning a duplicate that would fight it for
    // the same world file (stage 2.4).
    // Only rows registered from this host count: those are the servers that could
    // share a world file with a spawn, and a remote peer must not be able to
    // register a name first and have HOST hand requesters its address (7.20).
    std::vector<Registration> local;
    for (const auto& r : g_registry.snapshot())
        if (is_loopback_ipv4(r.peer)) local.push_back(r);
    if (const Registration* live = find_live_by_name(local, name)) {
        logline("HOST '" + name + "' -> joined existing " + live->ip + ":" +
                std::to_string(live->port));
        return "HOSTED:" + live->ip + ":" + std::to_string(live->port);
    }

    int port = allocHostPort();
    if (port < 0) return "HOSTFAIL:full";

    // Address advertised for the spawned server, both in the reply to this
    // requester and in the child's own --advertise (so remote browse clients
    // get a dialable address too, not a loopback that only worked for the
    // matchmaker's own host) — stage 2.4.
    std::string advertiseIp = !g_publicIp.empty()   ? g_publicIp
                             : !g_advertiseIP.empty() ? g_advertiseIP
                                                       : peerIp;
    // One world file per name (stage 2.4 bug fix): before this, every HOST
    // exec'd without --world, so all hosted worlds silently shared (and
    // corrupted) the one eden_world.model in g_worldDir.
    std::string worldFile = "world_" + world_slug(name) + ".model";

    pid_t pid = fork();
    if (pid < 0) return "HOSTFAIL:spawn";
    if (pid == 0) {
        // child: exec edenserver pointed back at us on loopback
        std::string sport = std::to_string(port);
        std::string mm = "127.0.0.1:" + std::to_string(g_port);
        std::vector<std::string> argv = {g_edenserver, "--port", sport, "--name", name,
                                         "--matchmaker", mm, "--advertise", advertiseIp,
                                         "--world", worldFile,
                                         "--idle-timeout", "180"};
        if (hasPw) { argv.push_back("--password"); argv.push_back(pw); }
        std::vector<char*> cargv;
        for (auto& s : argv) cargv.push_back(const_cast<char*>(s.c_str()));
        cargv.push_back(nullptr);
        if (chdir(g_worldDir.c_str()) != 0) _exit(127);
        execv(g_edenserver.c_str(), cargv.data());
        _exit(127);
    }

    // parent: reap asynchronously (the server is expected to outlive this call)
    std::thread([pid] { int st; waitpid(pid, &st, 0); }).detach();

    for (int i = 0; i < 60 && g_running; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        for (const auto& r : g_registry.snapshot())
            if (r.port == port && is_loopback_ipv4(r.peer)) {   // our child, not a squatter (7.20)
                logline("HOST '" + name + "' -> " + r.ip + ":" + std::to_string(port));
                return "HOSTED:" + r.ip + ":" + std::to_string(port);
            }
    }
    return "HOSTFAIL:timeout";
}

// ---- per-connection handling -------------------------------------------------

static void handleConn(int fd, sockaddr_in peer) {
    uint64_t id   = g_nextConn.fetch_add(1);
    std::string ip = peerAddr(peer);
    ConnCapGuard capGuard(ip);   // released on every return path below (stage 2.2)
    std::string carry, line;

    setRecvTimeout(fd, PRE_VERB_TIMEOUT_SEC);
    setSendTimeout(fd, PRE_VERB_TIMEOUT_SEC);
    if (!readLine(fd, carry, line)) {
        vlog("no first line from " + ip + " within " + std::to_string(PRE_VERB_TIMEOUT_SEC) + "s; dropping");
        close(fd);
        return;
    }

    // ---- game client: one-shot browse / host ----
    if (line == "LIST" || line == "LISTP") {
        auto regs = g_registry.snapshot();
        sendAll(fd, format_list(regs, g_rowForm));
        vlog("LIST from " + ip + " -> " + std::to_string(regs.size()) + " server(s)");
        close(fd);
        return;
    }
    if (line.rfind("HOST:", 0) == 0) {
        std::string reply = handleHost(line, ip);
        sendAll(fd, reply + "\n");
        vlog("HOST from " + ip + " -> " + reply);
        close(fd);
        return;
    }

    // ---- game server: persistent registration ----
    if (line.rfind("REGISTER:", 0) == 0) {
        const std::string useIp = g_advertiseIP.empty() ? ip : g_advertiseIP;
        const bool trusted = g_trust.trusted(ip);
        // Parse + ownership policy (stage 7.20). Returns the reply line, or "" for a
        // line that is not a valid REGISTER at all.
        auto doRegister = [&](const std::string& l, Registration& out, uint64_t* displaced) {
            if (!parse_register(l, useIp, out)) return std::string();
            std::string claimed = out.ip;
            if (advertise_policy(out, ip, useIp, trusted))
                logline("REGISTER '" + out.name + "' from " + ip + " advertised " + claimed +
                        "; not a trusted peer, listing " + out.ip + " instead (see --trust-advertise)");
            out.peer = ip;
            switch (g_registry.add(out, id, monoSeconds(), displaced,
                                   trusted ? 0 : MAX_REGISTRATIONS_PER_PEER)) {
                case Registry::AddResult::Ok:        return std::string("REGISTERED");
                case Registry::AddResult::Full:      return std::string("REGISTERFAIL:full");
                case Registry::AddResult::Taken:     return std::string("REGISTERFAIL:taken");
                case Registry::AddResult::PeerLimit: return std::string("REGISTERFAIL:limit");
            }
            return std::string("REGISTERFAIL:full");
        };

        Registration reg;
        uint64_t displaced = 0;
        std::string reply = doRegister(line, reg, &displaced);
        if (reply.empty()) {
            vlog("bad REGISTER from " + ip + ": " + line.substr(0, 80));
            close(fd);
            return;
        }
        if (reply != "REGISTERED") {
            logline(reply + " for '" + reg.name + "' " + reg.ip + ":" + std::to_string(reg.port) +
                    " from " + ip);
            sendAll(fd, reply + "\n");
            close(fd);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(g_regFdsMtx);
            g_regFds[id] = fd;
        }
        sendAll(fd, "REGISTERED\n");
        setRecvTimeout(fd, REGISTERED_TIMEOUT_SEC);   // registered: allowed to go quiet between heartbeats
        logline("REGISTER '" + reg.name + "' " + reg.ip + ":" + std::to_string(reg.port) +
                (reg.hasPassword ? " [locked]" : "") + " (conn " + std::to_string(id) + ")");
        if (displaced) {
            logline("  replaced conn " + std::to_string(displaced) + " from the same peer; closing it");
            dropDisplaced(displaced);
        }

        // Hold the connection open. Any line resets the TTL; PING is the expected
        // keep-alive but a re-REGISTER (e.g. a name change) is honoured too.
        for (;;) {
            if (!readLine(fd, carry, line)) break;
            g_registry.touch(id, monoSeconds());
            if (line == "PING" || line.rfind("PING:", 0) == 0) {
                // No reply either way (dev: "No-op that resets the idle timer").
                // `PING:<n>` additionally updates the row's advertised player count.
                int players;
                if (parse_ping(line, players)) g_registry.set_players(id, players);
                continue;
            }
            if (line.rfind("REGISTER:", 0) == 0) {
                Registration r2;
                uint64_t d2 = 0;
                std::string rep2 = doRegister(line, r2, &d2);
                if (!rep2.empty()) sendAll(fd, rep2 + "\n");
                if (rep2 == "REGISTERED") reg = r2;
                if (d2) dropDisplaced(d2);
            }
            // anything else: ignored, connection stays up
        }
        {
            std::lock_guard<std::mutex> lk(g_regFdsMtx);
            g_regFds.erase(id);
        }
        // Orphan, don't delist (stage 2.2): a brief hiccup on this socket
        // shouldn't drop a server that is plainly still up. The row survives
        // with no owning connection until the sweeper's probe decides its fate.
        if (g_registry.orphan(id))
            logline("orphan '" + reg.name + "' (conn " + std::to_string(id) +
                    " closed; awaiting probe/TTL)");
        else
            logline("closed conn " + std::to_string(id) + " ('" + reg.name + "'; row owned elsewhere)");
        close(fd);
        return;
    }

    vlog("unknown verb from " + ip + ": " + line.substr(0, 80));
    close(fd);
}

// ---- TTL sweeper ------------------------------------------------------------

static void sweeper() {
    while (g_running) {
        for (int i = 0; i < 10 && g_running; ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        for (uint64_t conn : g_registry.sweep(monoSeconds(), probeReachable))
            logline("drop  conn " + std::to_string(conn) + " (no heartbeat in " +
                    std::to_string(HEARTBEAT_TTL_SEC) + "s, probe failed)");
        persistRegistry();   // stage 2.2, piggybacked on the same ~10s tick
    }
}

// ---- main -----------------------------------------------------------------

static void parseArgs(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* def) -> std::string {
            return (i + 1 < argc) ? std::string(argv[++i]) : std::string(def);
        };
        if      (a == "--port")          g_port        = std::atoi(next("27020").c_str());
        else if (a == "--advertise-ip")  g_advertiseIP = next("");
        else if (a == "--trust-advertise") {
            std::string spec = next("");
            if (!g_trust.parse(spec)) {
                std::cerr << "edenmatch: --trust-advertise: bad address or CIDR in '" << spec << "'\n";
                std::exit(2);
            }
        }
        else if (a == "--short-list")    g_rowForm     = RowForm::Sketch4;
        else if (a == "--prod-list")     g_rowForm     = RowForm::Prod6;
        else if (a == "--verbose")       g_verbose     = true;
        else if (a == "--allow-host")    g_allowHost   = true;
        else if (a == "--edenserver")    g_edenserver  = next("./edenserver");
        else if (a == "--world")         g_worldDir    = next(".");
        else if (a == "--publicip")      g_publicIp    = next("");
        else if (a == "--registry-file") g_registryFile = next("eden_registry.txt");
        else if (a == "--host-ports") {
            std::string r = next("27600-27699");
            size_t d = r.find('-');
            if (d != std::string::npos) {
                g_hostPortLo = std::atoi(r.substr(0, d).c_str());
                g_hostPortHi = std::atoi(r.substr(d + 1).c_str());
            }
        } else if (!a.empty() && a[0] != '-') {
            g_port = std::atoi(a.c_str());  // bare port, back-compat with the launcher style
        }
    }
}

int main(int argc, char** argv) {
    std::cout << std::unitbuf;  // live logs under systemd/journald
    signal(SIGPIPE, SIG_IGN);
    parseArgs(argc, argv);

    // Both end up verbatim in SERVER: rows / HOSTED replies; refuse a typo at startup
    // rather than list junk (stage 7.20).
    for (std::string* ipArg : {&g_advertiseIP, &g_publicIp}) {
        if (ipArg->empty()) continue;
        std::string canon;
        if (!canonical_ipv4(*ipArg, canon)) {
            std::cerr << "edenmatch: '" << *ipArg << "' is not an IPv4 address "
                      << "(--advertise-ip / --publicip)\n";
            return 2;
        }
        *ipArg = canon;
    }

    // Resolve the edenserver path to absolute now — a HOST spawn chdir()s into
    // the world dir before exec, so a relative path would no longer resolve.
    if (g_allowHost) {
        char abs[PATH_MAX];
        if (realpath(g_edenserver.c_str(), abs)) g_edenserver = abs;
        else logline("warning: --edenserver '" + g_edenserver + "' not found; HOST will fail");
    }

    // Reload a persisted registry (stage 2.2) before we start listening: entries
    // come back orphaned and stale, so the sweeper's very first tick probes each
    // one before trusting it — a restart re-verifies rather than re-lists blind.
    if (!g_registryFile.empty()) {
        std::ifstream f(g_registryFile);
        if (f) {
            std::vector<Registration> loaded;
            std::string line;
            while (std::getline(f, line)) {
                Registration r;
                if (parse_persisted_line(line, r)) loaded.push_back(r);
            }
            if (!loaded.empty()) {
                g_registry.load_stale(loaded, monoSeconds());
                logline("loaded " + std::to_string(loaded.size()) + " server(s) from " +
                        g_registryFile + " (stale — verifying via probe)");
            }
        }
    }

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    // The listen fd must not survive a HOST spawn's fork+exec: without this,
    // every hosted edenserver inherits it, the port stays bound for as long as
    // any hosted server lives, and a matchmaker restart can't rebind (stage 2.2).
    fcntl(srv, F_SETFD, FD_CLOEXEC);
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(g_port));
    if (bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        perror("bind");
        return 1;
    }
    if (listen(srv, 64) != 0) { perror("listen"); return 1; }

    logline("listening on 0.0.0.0:" + std::to_string(g_port) +
            (g_rowForm == RowForm::Sketch4 ? "  [short-list]" :
             g_rowForm == RowForm::Prod6   ? "  [prod-list]"  : "") +
            (g_allowHost ? "  [HOST enabled]" : ""));
    std::thread(sweeper).detach();

    int64_t lastAcceptFailLog = -1;
    while (g_running) {
        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        int fd = accept(srv, reinterpret_cast<sockaddr*>(&peer), &plen);
        if (fd < 0) {
            // Same policy as edenserver's accept loops (stages 7.10 / 7.25): routine
            // errors retry silently, fd/memory exhaustion sleeps so this doesn't spin a
            // core at the moment the process can least afford it, the rest log <= 1/s.
            const int err = errno;
            const auto act = ewb::classify_accept_error(err);
            if (g_running && act != ewb::AcceptErrorAction::Retry) {
                const int64_t now = monoSeconds();
                if (now - lastAcceptFailLog >= 1) {
                    logline(std::string("accept failed: ") + strerror(err));
                    lastAcceptFailLog = now;
                }
                if (act == ewb::AcceptErrorAction::BackoffSleep)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }
        // Every accepted fd, not just the listener, must not survive a HOST
        // spawn's fork+exec: without this a hosted edenserver also inherits
        // every other live client's socket (stage 2.2).
        fcntl(fd, F_SETFD, FD_CLOEXEC);

        std::string peerIp = peerAddr(peer);
        if (!g_connCaps.tryAcquire(peerIp)) {
            logline("refused " + peerIp + ": connection cap (global " +
                     std::to_string(g_connCaps.total()) + "/" + std::to_string(MAX_CONNS_GLOBAL) +
                     ", this IP " + std::to_string(g_connCaps.forIp(peerIp)) + "/" +
                     std::to_string(MAX_CONNS_PER_IP) + ")");
            close(fd);
            continue;
        }
        // The slot acquired above is released by handleConn's ConnCapGuard
        // (constructed from the same peer address) on every exit path.

        std::thread(handleConn, fd, peer).detach();
    }
    close(srv);
    return 0;
}
