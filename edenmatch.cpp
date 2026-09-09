// edenmatch.cpp — standalone Eden matchmaker (POSIX, single translation unit).
//
// Populates the in-game Server Browser: game servers hold an open TCP connection
// here and REGISTER; game clients connect, send LIST, get the rows, disconnect.
// Protocol and framing: see matchmaker.h (pure logic + wire notes) — this file is
// only sockets, threads, and the optional on-demand HOST spawn.
//
// Build:  part of build_server.sh  (also: c++ -std=c++17 -O2 -pthread edenmatch.cpp -o edenmatch)
// Run:    ./edenmatch [--port 27020] [--advertise-ip IP] [--short-list] [--verbose]
//         on-demand hosting: --allow-host [--edenserver PATH] [--host-ports LO-HI] [--world DIR]
//
// Point a server at it with:  ./edenserver --matchmaker <edenmatch-host>[:27020]
#include "matchmaker.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstring>
#include <iostream>
#include <sstream>
#include <thread>

using namespace edenmatch;

static std::atomic<bool>     g_running{true};
static std::atomic<uint64_t> g_nextConn{1};
static Registry              g_registry;

static int         g_port         = DEFAULT_PORT;
static std::string g_advertiseIP;                 // global override for the per-peer fallback
static bool        g_shortList    = false;
static bool        g_verbose      = false;
static bool        g_allowHost    = false;
static std::string g_edenserver   = "./edenserver";
static std::string g_worldDir     = ".";
static int         g_hostPortLo   = 27600;
static int         g_hostPortHi   = 27699;

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
// register back. Returns the reply line to send the client.
static std::string handleHost(const std::string& line) {
    if (!g_allowHost) return "HOSTFAIL:spawn";

    // HOST:name[:hasPassword[:password]]
    std::vector<std::string> f;
    { std::stringstream ss(line); std::string t; while (std::getline(ss, t, ':')) f.push_back(t); }
    std::string name = f.size() >= 2 ? sanitize_name(f[1]) : std::string();
    if (name.empty()) return "HOSTFAIL:spawn";
    bool hasPw = f.size() >= 3 && !f[2].empty() && f[2] != "0";
    std::string pw = f.size() >= 4 ? f[3] : std::string();

    int port = allocHostPort();
    if (port < 0) return "HOSTFAIL:full";

    pid_t pid = fork();
    if (pid < 0) return "HOSTFAIL:spawn";
    if (pid == 0) {
        // child: exec edenserver pointed back at us on loopback
        std::string sport = std::to_string(port);
        std::string mm = "127.0.0.1:" + std::to_string(g_port);
        std::vector<std::string> argv = {g_edenserver, "--port", sport, "--name", name,
                                         "--matchmaker", mm, "--advertise", "127.0.0.1",
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
            if (r.port == port) {
                logline("HOST '" + name + "' -> 127.0.0.1:" + std::to_string(port));
                return "HOSTED:127.0.0.1:" + std::to_string(port);
            }
    }
    return "HOSTFAIL:timeout";
}

// ---- per-connection handling -------------------------------------------------

static void handleConn(int fd, sockaddr_in peer) {
    uint64_t id   = g_nextConn.fetch_add(1);
    std::string ip = peerAddr(peer);
    std::string carry, line;

    if (!readLine(fd, carry, line)) { close(fd); return; }

    // ---- game client: one-shot browse / host ----
    if (line == "LIST" || line == "LISTP") {
        auto regs = g_registry.snapshot();
        sendAll(fd, format_list(regs, g_shortList));
        vlog("LIST from " + ip + " -> " + std::to_string(regs.size()) + " server(s)");
        close(fd);
        return;
    }
    if (line.rfind("HOST:", 0) == 0) {
        std::string reply = handleHost(line);
        sendAll(fd, reply + "\n");
        vlog("HOST from " + ip + " -> " + reply);
        close(fd);
        return;
    }

    // ---- game server: persistent registration ----
    if (line.rfind("REGISTER:", 0) == 0) {
        Registration reg;
        std::string useIp = g_advertiseIP.empty() ? ip : g_advertiseIP;
        if (!parse_register(line, useIp, reg)) {
            vlog("bad REGISTER from " + ip + ": " + line.substr(0, 80));
            close(fd);
            return;
        }
        uint64_t displaced = 0;
        if (!g_registry.add(reg, id, monoSeconds(), &displaced)) {
            sendAll(fd, "REGISTERFAIL:full\n");
            close(fd);
            return;
        }
        sendAll(fd, "REGISTERED\n");
        logline("REGISTER '" + reg.name + "' " + reg.ip + ":" + std::to_string(reg.port) +
                (reg.hasPassword ? " [locked]" : "") + " (conn " + std::to_string(id) + ")");
        if (displaced) vlog("  replaced stale conn " + std::to_string(displaced));

        // Hold the connection open. Any line resets the TTL; PING is the expected
        // keep-alive but a re-REGISTER (e.g. a name change) is honoured too.
        for (;;) {
            if (!readLine(fd, carry, line)) break;
            g_registry.touch(id, monoSeconds());
            if (line == "PING" || line.rfind("PING:", 0) == 0) {
                continue;  // no-op, no reply (dev: "No-op that resets the idle timer")
            }
            if (line.rfind("REGISTER:", 0) == 0) {
                Registration r2;
                if (parse_register(line, useIp, r2)) {
                    g_registry.add(r2, id, monoSeconds());
                    sendAll(fd, "REGISTERED\n");
                }
            }
            // anything else: ignored, connection stays up
        }
        g_registry.remove(id);
        logline("drop  '" + reg.name + "' (conn " + std::to_string(id) + " closed)");
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
        for (uint64_t conn : g_registry.sweep(monoSeconds()))
            logline("drop  conn " + std::to_string(conn) + " (no heartbeat in " +
                    std::to_string(HEARTBEAT_TTL_SEC) + "s)");
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
        else if (a == "--short-list")    g_shortList   = true;
        else if (a == "--verbose")       g_verbose     = true;
        else if (a == "--allow-host")    g_allowHost   = true;
        else if (a == "--edenserver")    g_edenserver  = next("./edenserver");
        else if (a == "--world")         g_worldDir    = next(".");
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

    // Resolve the edenserver path to absolute now — a HOST spawn chdir()s into
    // the world dir before exec, so a relative path would no longer resolve.
    if (g_allowHost) {
        char abs[PATH_MAX];
        if (realpath(g_edenserver.c_str(), abs)) g_edenserver = abs;
        else logline("warning: --edenserver '" + g_edenserver + "' not found; HOST will fail");
    }

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
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
            (g_shortList ? "  [short-list]" : "") +
            (g_allowHost ? "  [HOST enabled]" : ""));
    std::thread(sweeper).detach();

    while (g_running) {
        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        int fd = accept(srv, reinterpret_cast<sockaddr*>(&peer), &plen);
        if (fd < 0) continue;
        std::thread(handleConn, fd, peer).detach();
    }
    close(srv);
    return 0;
}
