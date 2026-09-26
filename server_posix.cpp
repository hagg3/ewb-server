// Eden multiplayer server — POSIX port of server.cpp (Winsock) for macOS/Linux.
// Builds a standalone executable. Same wire protocol as the original Windows server.
//
// Build:   clang++ -std=c++17 -O2 -pthread server_posix.cpp -o edenserver
//   (or run ./build_server.sh)
// Run:     ./edenserver [port]        (default port 27015)
//
// Protocol (line/segment based, ':' delimited):
//   Client -> Server:
//     JOIN:username:characterType
//     MSG:text
//     ACTION:x:y:z:mode[:typeOrColor]      mode 0=build 1=mine 2=burn 3=paint
//     POS:x:y:z
//     VEL:x:y:z
//     POSVEL:px:py:pz:vx:vy:vz
//     REGION:x:z                           stream the world around a point
//     SIGNQ                                request the sign burst
//     PING                                 liveness / RTT probe
//   Server -> Clients (broadcast, '\n' terminated):
//     POS:username:type:x:y:z
//     VEL:username:type:x:y:z
//     POSVEL:username:type:px:py:pz:vx:vy:vz
//     ACTION:username:type:x:y:z:mode[:typeOrColor]
//     CAPS:region                          (unicast) capability advertisement
//     SPAWN:x:y:z                          (unicast) restore a saved position
//     SIGNP:server:x:y:z:a:b:c:text        (unicast) answer to SIGNQ
//     SNAPZ:count:base64                   (unicast) answer to REGION
//     PONG                                 (unicast) answer to PING
//     [Server] ... / [username (Tn)] chat text
//
// Join sequence (server -> the joining client), matching the native pcap:
//     [Server] Welcome, <name>! (Character Type: N)
//     CAPS:region
//     SPAWN:x:y:z        (only if this name has a saved position)
//     SIGNP:server:...   (burst, in answer to the client's SIGNQ)
//     SNAPZ:<n>:<b64>    (burst, in answer to the client's REGION)
// The client fires JOIN, SIGNQ and REGION back-to-back without waiting for
// replies, so what matters is the order of *our* output and that the SIGNP/SNAPZ
// bursts are answers rather than unsolicited pushes.

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/random.h>   // getentropy — PINs and salts (stage 8.6)
#include <netinet/in.h>
#include <netinet/tcp.h>   // TCP_NODELAY — disables Nagle on client sockets (stage 7.11)
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <memory>
#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <fstream>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cmath>          // lroundf — Tier 2 positions are floats on the wire
#include <climits>        // LLONG_MAX — /zone create takes a selection of any size
#include <ctime>          // gmtime_r/strftime — the audit log's UTC stamp (stage 3.4)

#include "region_query.h"   // REGION reply geometry + Cell -> record table (stage 1.1)
#include "snapz_codec.h"    // raw DEFLATE + base64 + SNAPZ framing      (stage 1.2)
#include "out_queue.h"      // per-client output queue policy            (stage 7.3)
#include "sign_store.h"     // eden_signs.txt + SIGNQ -> SIGNP           (stage 1.5)
#include "spawn_store.h"    // eden_spawn.txt: a world's default spawn     (stage 5.3)
#include "motd_store.h"     // eden_motd.txt: the per-world welcome message
#include "hardening.h"      // names, token buckets, ACTION validation   (stage 1.7)
#include "control.h"        // Tier 1 operator control socket             (stage 3.2)
#include "worldedit.h"      // Tier 2 player command surface              (stage 3.3)
#include "explode.h"        // TNT/paint explosion chain, bounded fan-out (stage 7.7)
#include "world_store.h"    // 16^3 chunk store + EDMB format + text load (stage 7.6)
#include "durable_write.h"  // temp + fsync + rename + dir fsync          (stage 7.29)
#include "zones.h"          // eden_zones.txt: protected boxes             (stage 8.1)
#include "zone_guard.h"     // restore wire, revert queue, audit folding   (stage 8.2)
#include "auth.h"           // eden_auth.txt: per-name PINs, /login        (stage 8.6)
#include "topmap.h"         // topmap: bounded top-down height map          (stage 8.5)
#include "base_profile.h"   // the terrain the client draws for itself

typedef int SOCKET;
constexpr SOCKET INVALID_SOCKET = -1;
constexpr int SOCKET_ERROR = -1;

constexpr int DEFAULT_PORT = 27015;
constexpr int BUFFER_SIZE = 512;

// Player info structure
struct PlayerInfo {
    SOCKET socket;
    std::string username;
    int characterType;
    float posX = 0, posY = 0, posZ = 0;
    float velX = 0, velY = 0, velZ = 0;
    std::string ip;                 // peer address, for `who` and IP bans (stage 3.2)
    // Logged in with this name's PIN (stage 8.6). Written under clientsMutex by
    // `/login` and cleared by `passwd`/`unpasswd`; read by the level and zone
    // checks. Always false for a name with no PIN.
    bool verified = false;
};

std::vector<SOCKET> clients;
std::map<SOCKET, PlayerInfo> playerInfoMap;
std::mutex clientsMutex;
std::atomic<bool> serverRunning{true};

// SIGTERM/SIGINT (systemctl stop, docker stop, Ctrl-C) default to killing the
// process outright, which drops everything since the last 15s autosave tick.
// The handler below only sets serverRunning=false and wakes the accept loop's
// poll() via a self-pipe (write() is async-signal-safe; accept() itself is not
// interruptible in a portable way once poll() has already returned). The actual
// save + drain + exit runs on the main thread, never inside the handler.
static int g_wakePipe[2] = {-1, -1};
static void handleShutdownSignal(int) {
    serverRunning = false;
    if (g_wakePipe[1] >= 0) {
        const char b = 0;
        ssize_t ignored = write(g_wakePipe[1], &b, 1);
        (void)ignored;
    }
}
SOCKET listenSocket = INVALID_SOCKET;

// --- Authoritative world state ---
// The base terrain (flatland) is deterministic on every client; the server only
// needs to store the *edits* players make. The edit log is an ordered list of
// "x:y:z:mode[:extra]" suffixes. Replaying it in order reconstructs the current
// map. New players are sent the whole log when they join; live edits are relayed.
// --- Configuration (set from command-line flags in main) ---
std::string g_worldFile  = "eden_world.model";  // world model (block deltas)
// Which format saveWorld() writes (stage 7.6). Loading always accepts both — the
// magic bytes decide — so this only exists for an operator who wants a grep-able
// world or to hand the file back to a pre-7.6 build. Text costs ~5x the disk and
// holds the world lock for the whole serialise; EDMB is the default for a reason.
bool        g_saveText   = false;               // --world-format text
std::string g_serverName = "Eden Server";       // shown in the matchmaker list
std::string g_password   = "";                  // empty = open server (resolved in main: --password / --password-file / EDEN_PASSWORD)
std::string g_passwordFile;                     // --password-file: first line is the password (stage 7.28)
std::string g_matchHost  = "";                  // matchmaker host; empty = don't register
int         g_matchPort  = 27020;
int         g_port       = 27015;               // our own listen port (set in main)
std::string g_advertiseIP= "";                  // IP clients should use to reach us
bool        g_verbose    = false;               // log every terrain edit (chatty)
int         g_idleTimeout = 0;                   // seconds; >0 = self-exit when empty this long
int         g_regionRadius = ewb::REGION_RADIUS; // blocks a REGION reply covers (--region-radius)
bool        g_regionSort  = true;                // sort records before deflate (--no-region-sort)
bool        g_regionEmptyFrame = true;           // answer an empty region with SNAPZ:0 (--no-region-empty-frame to suppress)
std::string g_signFile   = "eden_signs.txt";     // sign sidecar (--signs)
std::string g_spawnFile  = "";                   // world spawn sidecar; empty -> derived from the world dir (--spawn-file)
std::string g_motdFile   = "";                   // welcome-message sidecar; empty -> derived from the world dir (--motd-file)
bool        g_haveWorldSpawn = false;            // a default spawn point is configured (file or --spawn)
ewb::Spawn  g_worldSpawn;                        // the default spawn handed to a player with no saved position
bool        g_legacySnapshot = false;            // push the ACTION dump on JOIN (--legacy-snapshot)
int         g_connectLimit = 10;                 // connects per IP per window; 0 = off (--connect-limit)
bool        g_tcpNodelay = true;                 // disable Nagle on client sockets; --tcp-nodelay 0 to keep it

// --- Connection-lifecycle hardening (stage 1.10) ---
int         g_authFailLimit    = 5;   // wrong-password attempts per IP per minute before an escalating lockout; 0 = off (--auth-fail-limit)
int         g_handshakeTimeout = 15;  // seconds a fresh connection has to send JOIN before it is dropped; 0 = off (--handshake-timeout)
int         g_idleConnTimeout  = 300; // seconds of post-JOIN socket silence tolerated; 0 = off (--idle-timeout-conn)
int         g_staleSessionSecs = 15;  // a same-address JOIN evicts a duplicate name silent this long; 0 = never (--stale-session-secs)

// --- Tier 1 operator control socket (stage 3.2) ---
bool        g_controlEnabled = true;             // --no-control-socket disables it
std::string g_controlSocket  = "";               // path; empty until main() derives it from the world dir
std::string g_banFile        = "eden_bans.txt";  // persisted ban list (next to the world)
std::string g_opsFile        = "eden_ops.txt";   // persisted op levels
int         g_defaultLevel   = ewb::CTL_LEVEL_MIN;// op level for a player with no eden_ops.txt entry
// Flood guard (stage 3.4). Filesystem permissions decide who may connect; these
// decide how fast, so an operator's runaway script can't hold the world lock.
double      g_ctlCmdRate   = ewb::CTL_CMD_RATE;   // control commands/sec; 0 = off (--control-rate)
double      g_ctlCmdBurst  = ewb::CTL_CMD_BURST;  // commands allowed at once (--control-burst)
int         g_ctlMaxConns  = ewb::CTL_MAX_CONNS;  // concurrent control connections (--control-max-conns)
long long   g_ctlFillCap   = 0;                   // derived from --we-max-cells in main()
std::atomic<int> g_ctlConns{0};                   // currently open control connections

// --- Audit log (stage 3.4) ---------------------------------------------------
std::string g_auditFile    = "";                  // optional append-only copy of the audit channel (--audit-file)

// --- Tier 2 player command surface (stage 3.3) -------------------------------
bool        g_weEnabled    = true;                       // --no-worldedit disables every in-chat command
long long   g_weMaxCells   = ewb::WE_MAX_EDIT_CELLS;     // largest box one command may read or write (--we-max-cells)
size_t      g_weUndoBudget = ewb::WE_UNDO_BUDGET_BYTES;  // per-player undo+redo bytes (--we-undo-budget)
double      g_weCellRate   = ewb::WE_CELL_RATE;          // cells/sec a player may spend; 0 = unlimited (--we-rate)
double      g_weCellBurst  = ewb::WE_CELL_BURST;         // cells they may spend at once (--we-burst)

// Work one BURN's explosion chain may do, in cells read, before it is truncated
// (stage 7.19, --burn-max-cells). This is a *lock-hold* bound: simAction holds
// g_worldMtx for the whole chain, so it is the one thing one packet can make every
// other player wait for. See explode.h for how the budget is spent.
size_t      g_burnMaxCells = ewb::EXPLODE_DEFAULT_MAX_VISITS;

// --- Protected zones (stage 8.2) ---------------------------------------------
std::string g_zonesFile  = "";                    // eden_zones.txt; empty -> derived from the world dir (--zones-file)
// How long after a refused edit the restore is sent (--zone-revert-delay-ms). 0 sends
// it at once. ⚠️ The default is a placeholder: whether the retail client needs the
// restore to trail its own handling of the edit is not yet measured.
int         g_zoneRevertDelayMs = ewb::ZONE_REVERT_DEFAULT_DELAY_MS;

// --- Player identity (stage 8.6) ---------------------------------------------
std::string g_authFile   = "";                    // eden_auth.txt; empty -> derived from the world dir (--auth-file)

// Per-client output queue bounds (stage 7.3). The policy these feed lives in
// out_queue.h; these are the numbers an operator can move. See docs/configuration.md.
size_t      g_outboxMax     = 1u << 20;      // --client-outbox-max: movement/chat backlog bytes
size_t      g_worldboxMax   = 16u << 20;     // --client-world-max: world-state backlog bytes
size_t      g_regionQueue   = 2;             // --client-region-queue: regions in flight per client
uint64_t    g_regionPending = 16000000;      // --region-pending-records: queued records, all clients
int         g_writeTimeout  = 60;            // --client-write-timeout: seconds with no drain progress

// REGION service counters (the measurement plan §3.2's `region-stats` asks for).
std::atomic<uint64_t> g_rgnRequests{0};
std::atomic<uint64_t> g_rgnCellsScanned{0};
std::atomic<uint64_t> g_rgnRecords{0};
std::atomic<uint64_t> g_rgnBytesOut{0};
std::atomic<uint64_t> g_rgnMicros{0};
std::atomic<uint64_t> g_rgnRefused{0};      // regions refused for output backpressure (7.3)
std::atomic<uint64_t> g_rgnPendingRecs{0};  // records queued for encoding, across all clients
std::atomic<uint64_t> g_slowDrops{0};       // clients disconnected for not draining (7.3)

// Lock-hold instrumentation (stage 7.6 step 1). The counters above measure *work*
// (cells, records, bytes); these measure the thing that actually makes a busy
// server feel slow — how long `g_worldMtx` is unavailable to everyone else. Every
// `ACTION` needs that lock, so a REGION scan or a save snapshot holding it is a
// stall for every other player. Reported by the `region-stats` control verb.
std::atomic<uint64_t> g_rgnLockMicros{0};   // total time serveRegion() held g_worldMtx
std::atomic<uint64_t> g_rgnLockMaxMicros{0};// worst single serveRegion() hold
std::atomic<uint64_t> g_saveCount{0};       // saveWorld() calls that actually wrote
std::atomic<uint64_t> g_saveLockMicros{0};  // total time saveWorld() held g_worldMtx
std::atomic<uint64_t> g_saveLockMaxMicros{0};
std::atomic<uint64_t> g_saveWriteMicros{0}; // total time in the out-of-lock disk write
std::atomic<uint64_t> g_saveBytes{0};       // bytes the last save wrote

/// `max = std::max(max, v)`, for the atomics above.
static inline void bumpMax(std::atomic<uint64_t>& m, uint64_t v) {
    uint64_t cur = m.load(std::memory_order_relaxed);
    while (v > cur && !m.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}

// Safety limits (hardening for public hosting).
static const int    SV_WORLD_HEIGHT   = 256;       // must match the game's T_HEIGHT
static const size_t SV_MAX_WORLD_CELLS_DEFAULT = 4000000;   // default edited-cell ceiling
size_t              g_maxWorldCells   = SV_MAX_WORLD_CELLS_DEFAULT;  // cap edited-cell count (mem/disk guard); --max-world-cells
static const size_t SV_MAX_LINE       = 8192;      // drop a client flooding without '\n'
static const int    SV_MAX_CLIENTS    = 64;        // reject connections beyond this

// REGION is the single most expensive thing a client can ask for and a trivial
// amplification vector (a ~20-byte request producing megabytes of reply), so it is
// paced per connection. 750 ms matches a reference test client's own
// `net::region::REGION_MIN_GAP` and the session cap its `MAX_REGIONS_PER_SESSION`.
// ⚠️ These are *our* limits, not client etiquette — the reference client imposes
// the same numbers on itself, but a hostile or buggy client has no such scruples.
static const long SV_REGION_MIN_GAP_MS = 750;
static const int  SV_MAX_REGIONS_PER_SESSION = 256;

// SIGNQ is a smaller REGION: a 6-byte request answered with the whole sign file.
// At the sign cap below that is a few hundred KB from one line — an amplification
// vector in exactly the same shape, so it gets the same treatment (gated on JOIN,
// paced, session-capped).
static const long SV_SIGNQ_MIN_GAP_MS = 1000;
static const int  SV_MAX_SIGNQ_PER_SESSION = 32;
static const size_t SV_MAX_SIGNS = 20000;      // bound the burst the file can produce

// A player's sign write (`SIGNP:x:y:z:a:b:c:text`) rebuilds the burst every SIGNQ is
// answered from, so one connection's writes are paced. Signs are placed by hand;
// a burst of 8 and 1/s sustained is far above any human cadence.
static const double SV_SIGNP_BURST = 8.0;
static const double SV_SIGNP_RATE  = 1.0;

// Per-connection ACTION budget (stage 1.7, retuned by 1.8 rung 2/3).
// ⚠️ The original 8/s + 64 burst was sized for a *human* placing blocks by hand. It
// silently shreds a legitimate bulk edit: a reference test client's "arm interact
// + fill a box" drains its own queue at ~500 lines/s (default `net::queue` rate), so
// a 1728-cell box arrived as ~96 scattered survivors and the rest were dropped at
// ingest — exactly the "most of my box is gone on reconnect" bug 1.8 found. The
// defaults now accommodate that client's default drain rate; a public operator can tighten them
// with --action-rate / --action-burst (0 = unlimited). ⚠️ BURN still costs far more
// because one ACTION can write hundreds of cells server-side: simExplode's radius-6
// sphere, times its chain depth.
static double SV_ACTION_BURST     = 1024.0;
static double SV_ACTION_RATE      = 512.0;  // tokens/second (0 = unlimited)
static const double SV_ACTION_COST_BURN = 64.0;

// Per-connection movement budget (stage 7.14). POS/VEL/POSVEL are the highest-
// frequency verbs on the wire and, like ACTION, fan out to every other peer, but
// unlike ACTION they had no limiter at all — a scripted flood costs the server a
// parse plus an (N-1)-way broadcast per packet with nothing in the way. The
// private capture notes (see CAPTURE-FINDINGS.md, cited in docs/protocol.md) put
// the retail client's observed movement rate at ~3-4 Hz (POSVEL only; it does not
// send bare POS/VEL). This budget is not meant to pace a real player — it exists
// only to cap a scripted flood — so it sits an order of magnitude above that
// observed rate, with a burst large enough to absorb a client catching up after a
// lag spike (several queued updates arriving back-to-back) without dropping any
// of them. --move-rate / --move-burst (0 = unlimited) let an operator retune it.
static double SV_MOVE_BURST        = 80.0;
static double SV_MOVE_RATE         = 40.0;   // tokens/second (0 = unlimited)

// Per-connection chat budget (stage 7.14). Unlike movement, chat floods are a
// plain griefing vector (every line is relayed verbatim to every peer) and a
// human typist has nowhere near the cadence a script does, so this bucket is
// tight: a handful of lines in a row (burst) is normal after a pause to type,
// but a sustained rate faster than "a few lines per 10 seconds" is not a human.
// Unlike movement, an over-budget chat line is not silently dropped: the sender
// is told they are being throttled (see MSG dispatch), so a real player who
// trips it understands what happened. --chat-rate / --chat-burst (0 = unlimited).
static double SV_CHAT_BURST        = 5.0;
static double SV_CHAT_RATE         = 0.5;   // tokens/second (0 = unlimited) == 1 line/2s sustained

// Per-IP connect pacing. SV_MAX_CLIENTS bounds concurrency but not churn — a
// connect flood still spawns a thread per attempt. The default 10 in 10 s allows a
// human reconnecting repeatedly while flattening a loop.
// ⚠️ It is **per source address**, so a whole LAN behind one NAT address shares one
// allowance, and so does a test harness on loopback — hence --connect-limit N
// (0 disables), which the stage 1.8 live test uses.
static const double SV_CONNECT_WINDOW_SEC  = 10.0;

// Chat is broadcast verbatim to every peer; cap what one line can cost.
static const size_t SV_MAX_CHAT = 256;

// How long a client writer blocks in one send() before it looks up and asks
// whether it has run out of --client-write-timeout (SO_SNDTIMEO, stage 7.3). It
// is a polling granularity, not a deadline: progress resets the deadline.
static const int SV_SEND_POLL_SEC = 5;

// ...and the much shorter budget it gets once the reader has asked it to stop.
// The close path exists to flush a queued denial or kick notice to a peer that is
// still reading; a peer that is not gets the socket closed under it rather than
// holding the reader's thread open for a full write timeout.
static const int SV_CLOSE_DRAIN_SEC = 5;

// Monotonic seconds — the clock the token buckets and the connect limiter run on.
// They take `now` as a parameter so hardening.h stays pure and testable.
static double monoSeconds() {
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

// Best-effort detection of this machine's primary LAN IPv4 address, so the
// matchmaker advertises an address other devices can actually connect to
// (rather than 127.0.0.1, which is what it would otherwise see for a server
// running on the same host as the matchmaker).
std::string detectLanIP() {
    std::string result;
    struct ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) != 0) return result;
    for (struct ifaddrs* ifa = ifs; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;
        char ip[INET_ADDRSTRLEN] = {0};
        auto* sa = (struct sockaddr_in*)ifa->ifa_addr;
        inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
        std::string s = ip;
        if (s.rfind("127.", 0) == 0 || s.rfind("169.254.", 0) == 0) continue;  // loopback/link-local
        // Prefer common private LAN ranges; take the first otherwise.
        if (s.rfind("192.168.", 0) == 0 || s.rfind("10.", 0) == 0 || s.rfind("172.", 0) == 0) {
            result = s; break;
        }
        if (result.empty()) result = s;
    }
    freeifaddrs(ifs);
    return result;
}

// --- Authoritative world MODEL -------------------------------------------------
// The flatland base is deterministic on every client, so we only store deltas:
// cells changed by building, mining, painting, burning or explosions.
//   type 0   = removed (AIR)
//   type 255 = a base (natural) block that was only painted
//   1..254   = a placed block of that type
// This lets the server keep an accurate picture of the landscape and hand a
// correct snapshot to any joiner, regardless of the order edits arrived in.
//
// Stage 7.6: the model behind this is `ewb::WorldStore` — a map of 16^3 chunks,
// not a cell-per-entry hash map. Everything below still speaks in **logical**
// cells (`type 0` = mined air, `255` = painted base), because the store maps its
// in-array "explicitly mined" sentinel back to 0 on every read; see the invariant
// table at the top of world_store.h. Nothing in this file may see a 254.
using Cell = ewb::WorldCell;
static ewb::WorldStore g_world;
// ⚠️ Lock order: g_zonesMtx is taken and **released** before g_worldMtx — an edit path
// copies the zone snapshot out first, then locks the world. Nothing holds g_zonesMtx
// while taking any other lock, so it can never sit inside g_worldMtx.
static std::mutex g_worldMtx;        // lock order: after g_zonesMtx (released), before g_signMtx, never after it (see Signs)
static std::mutex g_saveMtx;         // serializes on-disk writes (world + players)
std::atomic<bool> editsDirty{false};

// Protected zones (stage 8.2; model in zones.h). The set is immutable once published:
// a change swaps in a new one, so a hot path copies the pointer under g_zonesMtx and
// reads the set with no lock at all — the zone lock is never held across a world edit.
static std::mutex                           g_zonesMtx;   // guards the pointer only
static std::shared_ptr<const ewb::ZoneSet>  g_zones = std::make_shared<const ewb::ZoneSet>();
// Serializes the control socket's zone:* writers against each other (stage 8.3):
// each one reads a snapshot, mutates a private copy, saves it, then swaps the
// pointer under g_zonesMtx above. Without this, two concurrent zone edits could
// both read the same snapshot and one's write would silently lose the other's.
// Never held across g_worldMtx or a broadcast — control commands only.
static std::mutex                           g_zonesEditMtx;

static std::shared_ptr<const ewb::ZoneSet> zonesSnapshot() {
    std::lock_guard<std::mutex> lk(g_zonesMtx);
    return g_zones;
}

// One edit's view of the zones: the snapshot, taken before g_worldMtx, and who is
// asking. `level` is the editor's zone-bypass level (auth.h): their op level only if
// they have logged in with a PIN this session (stage 8.6), otherwise -1, which
// bypasses nothing. A claimed name is not an identity, so an op who has not logged
// in is held by every zone like anyone else.
struct ZoneCheck {
    std::shared_ptr<const ewb::ZoneSet> zones;
    int level = -1;
};
static int sessionLevel(const std::string& name, bool verified);   // after the ops file

// Whether this connection has logged in, and under which name. False for a socket
// not in the roster. Takes clientsMutex only.
static bool sessionIdentity(SOCKET s, std::string& name) {
    std::lock_guard<std::mutex> lk(clientsMutex);
    auto it = playerInfoMap.find(s);
    if (it == playerInfoMap.end()) return false;
    name = it->second.username;
    return it->second.verified;
}

// The zone view for an edit by the player on `s`. Who is asking is only looked up
// when some enforcing zone names a bypass level — otherwise nobody bypasses
// anything and the answer is -1 for free. Never call with clientsMutex held.
static ZoneCheck zoneCheck(SOCKET s) {
    ZoneCheck zc{zonesSnapshot(), -1};
    if (zc.zones && zc.zones->hasBypassLevels()) {
        std::string name;
        if (sessionIdentity(s, name))
            zc.level = ewb::auth_zone_bypass_level(sessionLevel(name, true), true);
    }
    return zc;
}

// **The** zone predicate — every player edit path asks this and nothing else: ACTION
// build/mine/paint/burn, the explosion chain, a player's sign write and every WorldEdit
// write. The control socket never asks: it is the operator.
static const ewb::Zone* zoneDenies(const ZoneCheck& zc, int x, int y, int z) {
    return zc.zones ? zc.zones->blocking(x, y, z, zc.level) : nullptr;
}

// The zones that meet a box, as a smaller check of their own; empty when none do,
// which is the common case and costs one pass over the set. The explosion chain
// asks per cell, up to --burn-max-cells times, so it asks this instead of the whole set.
static ZoneCheck zoneCheckNear(const ZoneCheck& zc, int x0, int y0, int z0, int x1, int y1, int z1) {
    ZoneCheck near{nullptr, zc.level};
    if (!zc.zones || !zc.zones->intersects(x0, y0, z0, x1, y1, z1)) return near;
    auto sub = std::make_shared<ewb::ZoneSet>();
    for (const ewb::Zone& zn : zc.zones->zones())
        if (zn.enforced && !zn.bypassedBy(zc.level) && zn.overlaps(x0, y0, z0, x1, y1, z1)) sub->add(zn);
    if (sub->size() == 0) return near;   // every zone in reach is one this editor bypasses
    near.zones = std::move(sub);
    return near;
}

// Block constants mirrored from the game's Constants.h.
enum { SV_AIR=0, SV_BEDROCK=1, SV_TNT=9, SV_FIREWORK=65, SV_STEEL=74, SV_PAINTED_BASE=255 };

// region_query.h re-declares these two sentinels so the Cell -> wire-record table
// is unit-testable without pulling in the server; keep the two definitions honest.
static_assert(SV_AIR == ewb::CELL_AIR, "SV_AIR must match ewb::CELL_AIR");
static_assert(SV_PAINTED_BASE == ewb::CELL_PAINTED_BASE, "SV_PAINTED_BASE must match ewb::CELL_PAINTED_BASE");
// The ingest guard (hardening.h) and the wire guard (region_query.h) must agree on
// the palette, or a colour accepted at ACTION time would be silently dropped by the
// encoder — a cell that renders for the player who painted it and for nobody else.
static_assert((int)ewb::MAX_PAINT_INDEX == (int)ewb::CELL_MAX_PAINT,
              "ewb::MAX_PAINT_INDEX must match ewb::CELL_MAX_PAINT");
static_assert(ewb::MAX_BLOCK_TYPE < SV_PAINTED_BASE,
              "a placeable block type must never collide with the painted-base sentinel");
static_assert(ewb::SIGN_Y_MAX + 1 == SV_WORLD_HEIGHT, "sign y range must match the world height");
static_assert(ewb::WS_WORLD_HEIGHT == SV_WORLD_HEIGHT,
              "world_store.h's chunk-store Y bound must match the world height, or a box query "
              "could silently miss chunks above WS_MAX_CY");
// A validated position must survive `weFeet()`'s lroundf into an int cell without
// overflowing, and must still be inside the coordinate range `ACTION` enforces
// (stage 7.16). The vertical bound is deliberately *above* the world height —
// see ewb::MOVE_Y_MAX for why a legal player y exceeds the topmost block.
static_assert(ewb::MOVE_XZ_MAX == 16777215.0f && ewb::MOVE_XZ_MAX == (float)0xFFFFFF,
              "the movement x/z bound must be the same 24-bit key range ACTION checks");
static_assert(ewb::MOVE_Y_MAX > SV_WORLD_HEIGHT && ewb::MOVE_Y_MAX < 1e6f,
              "the movement y bound must clear the world height and stay far inside int range");

static inline uint64_t wkey(int x,int y,int z){
    return (((uint64_t)(uint32_t)x & 0xFFFFFFull) << 40)
         | (((uint64_t)(uint32_t)z & 0xFFFFFFull) << 16)
         |  ((uint64_t)(uint32_t)y & 0xFFFFull);
}
static inline void wunkey(uint64_t k,int&x,int&y,int&z){
    x = (int)((k >> 40) & 0xFFFFFF);
    z = (int)((k >> 16) & 0xFFFFFF);
    y = (int)( k        & 0xFFFF);
}
// Defined with the sign store below: worldSet() just stored air at key `k`, so any
// signs on that block go too (stage 7.2). Caller holds g_worldMtx.
static void removeSignsOnBlock(uint64_t k);

// Caller must hold g_worldMtx. Returns false when the cap refused a brand-new cell.
//
// The only live writer of g_world (loadWorld is the other, at startup), which is why
// the sign hook lives here: every edit that turns a cell to air — mine, burn, a blast,
// setblock/fill, every WorldEdit command, //paste, //undo, //redo — passes through it.
// A cell the cap refuses is left alone, and so are its signs.
//
// A coordinate outside the world (ewb::ws_in_world) is refused before the cap
// check, which would otherwise test an aliased cell (ROADMAP-SERVER 7.22). Every
// caller validates first; this is the backstop, and it is not a cap refusal.
static bool worldSet(int x,int y,int z,int type,int color){
    if(!ewb::ws_in_world(x,y,z)) return false;
    uint64_t k = wkey(x,y,z);
    // Cap the number of distinct edited cells so a malicious/buggy client can't
    // grow the map (and the on-disk save) without bound. Updates to existing
    // cells are always allowed; only brand-new cells are refused past the cap.
    if(!g_world.contains(x,y,z) && g_world.size()>=g_maxWorldCells){
        // ⚠️ From the player's seat this is silent data loss: their client has
        // already drawn the block. This used to log once per process, and on the
        // first public server that one line scrolled away while every new block
        // players placed for a day was refused. Now: one line a minute, with a
        // count and the fix. The statics are guarded by the g_worldMtx we hold.
        static ewb::TokenBucket logGate(1.0, 1.0 / 60.0);
        static size_t unreported = 0;
        ++unreported;
        if (logGate.allow(monoSeconds())) {
            std::cerr << "[Server] world cell cap reached (" << g_maxWorldCells << "): refused "
                      << unreported << " new cell(s) — players' new blocks are NOT being saved."
                         " Restart with --max-world-cells "
                      << ewb::recommended_max_world_cells(g_world.size()) << " or higher." << std::endl;
            unreported = 0;
        }
        return false;
    }
    if(!g_world.set(x, y, z, (unsigned char)type, (unsigned char)color)) return false;
    editsDirty = true;
    // A sign hangs on a block; a block that is now air has no face left to hang one on.
    if(type==SV_AIR) removeSignsOnBlock(k);
    return true;
}
static bool worldGet(int x,int y,int z, Cell& out){
    return g_world.get(x, y, z, out);
}

// Simulate a TNT / paint explosion centred on (x,y,z). The algorithm itself
// (worklist, depth guard, cell-visit budget — ROADMAP-SERVER 7.7 / 7.19) lives in
// explode.h so it can be unit-tested offline; this just wires it to g_world.
// Caller holds g_worldMtx.
//
// The accessors are plain member functions, not std::function: a chain reads up to
// --burn-max-cells cells, and every one of them was an indirect call (7.19).
//
// The blast sphere reaches EXPLODE_RADIUS past its centre, so a TNT at the edge of
// the world reaches outside it. y is clipped by yMin/yMax inside the algorithm;
// x/z (only reachable from a TNT within 5 blocks of x or z = 0 / 0xFFFFFF) are
// clipped here: outside the world there is nothing to read, and nothing to write —
// which is not a cap refusal, so `set` reports success (ROADMAP-SERVER 7.22).
//
// Protected zones (stage 8.2): `zb`, when set, carries the zones within the chain's
// reach, and every cell the blast was refused is collected into it for the restore.
// Every client simulated the blast itself and did destroy those cells.
struct ZoneBlast {
    ZoneCheck near;                               // zones within EXPLODE_REACH of the root
    ewb::ZoneCellSet hit{ewb::ZONE_BURN_RESTORE_MAX};   // cells to put back, deduplicated
    std::vector<ewb::RevertCell> explosives;      // protected TNT/fireworks: clients chain these
    std::string zone;                             // the first zone the blast hit, for the notice
    int fx = 0, fy = 0, fz = 0;                   // ...and where
};

struct SvExplodeWorld {
    ZoneBlast* zb = nullptr;
    // Caller holds g_worldMtx. Called before the cell is read, once per sample.
    bool protectedAt(int x, int y, int z) const {
        if (!zb || !zb->near.zones) return false;
        const ewb::Zone* zn = zoneDenies(zb->near, x, y, z);
        if (!zn) return false;
        if (zb->hit.empty()) { zb->zone = zn->name; zb->fx = x; zb->fy = y; zb->fz = z; }
        if (zb->hit.add(x, y, z)) {
            Cell c;
            if (worldGet(x, y, z, c) && (c.type == SV_TNT || c.type == SV_FIREWORK))
                zb->explosives.push_back({x, y, z});
        }
        return true;
    }
    bool get(int x, int y, int z, ewb::ExplodeCell& out) const {
        if (!ewb::ws_in_world(x, y, z)) return false;
        Cell c;
        if (!worldGet(x, y, z, c)) return false;
        out.type = c.type; out.color = c.color;
        return true;
    }
    bool set(int x, int y, int z, int type, int color) const {
        if (!ewb::ws_in_world(x, y, z)) return true;
        return worldSet(x, y, z, type, color);
    }
    int airType = SV_AIR, tntType = SV_TNT, fireworkType = SV_FIREWORK;
    int bedrockType = SV_BEDROCK, steelType = SV_STEEL, paintedBaseType = SV_PAINTED_BASE;
    int yMin = 0;
    // Exclusive. Was 1024: a TNT at y 255 wrote y 256..260 into chunks no REGION
    // reply ever reads, counted against the cap and saved forever (7.22).
    int yMax = SV_WORLD_HEIGHT;
};
static_assert(SvExplodeWorld{}.yMax == ewb::WS_WORLD_HEIGHT,
              "the blast must clip at the height the chunk store's box sweep covers");

static ewb::ExplodeResult simExplode(int cx,int cy,int cz, ZoneBlast* zb){
    SvExplodeWorld w;
    w.zb = zb;
    return ewb::simExplode(w, cx, cy, cz, g_burnMaxCells);
}

// Apply one terrain action to the model. mode: 0 build 1 mine 2 burn 3 paint.
// `refused` counts cells the world cell cap refused; the rest of the result is a
// BURN's chain accounting (stage 7.19) and is zero for every other mode.
//
// The caller has already refused an edit *on* a protected cell (stage 8.2); `zb` is
// only for a burn, whose blast can reach into a zone from outside it.
static ewb::ExplodeResult simAction(int mode,int x,int y,int z,int extra, ZoneBlast* zb){
    std::lock_guard<std::mutex> lk(g_worldMtx);
    ewb::ExplodeResult res;
    switch(mode){
        case 0: if(!worldSet(x,y,z, extra, 0)) ++res.refused; break;  // BUILD (extra=type)
        case 1: if(!worldSet(x,y,z, SV_AIR, 0)) ++res.refused; break; // MINE
        case 3: { Cell c; bool have=worldGet(x,y,z,c);            // PAINT (extra=color)
                  if(!worldSet(x,y,z, have? c.type : SV_PAINTED_BASE, extra)) ++res.refused;
                  break; }
        case 2: { Cell c; bool have=worldGet(x,y,z,c);            // BURN
                  if(have && (c.type==SV_TNT || c.type==SV_FIREWORK)) res = simExplode(x,y,z, zb);
                  else if(have && c.type!=SV_AIR) worldSet(x,y,z, SV_AIR, 0);
                  break; }
    }
    return res;
}

// Read the world file. **Both formats load** (stage 7.6): the first four bytes
// decide. `EDMB` is the binary chunk format saveWorld() writes; anything else is
// the legacy `x:y:z:type:color` text this server shipped with, and which
// `eden_import` still writes — so no world needs a migration step, and a world
// saved by an older build keeps working. The format is *not* sticky: unless
// --world-format text says otherwise, the next save writes EDMB.
void loadWorld() {
    std::ifstream f(g_worldFile, std::ios::binary);
    if (!f) return;
    std::string blob((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::lock_guard<std::mutex> lock(g_worldMtx);
    const bool binary = ewb::WorldStore::is_edmb(blob.data(), blob.size());
    if (binary) {
        std::string err;
        if (!g_world.load_edmb(blob.data(), blob.size(), err)) {
            std::cerr << "[Server] WARNING: " << g_worldFile << " is a damaged EDMB world ("
                      << err << "); loaded " << g_world.size()
                      << " cell(s) before the damage. Saving will overwrite it — stop the"
                         " server now if you want to keep the file." << std::endl;
        }
    } else {
        std::istringstream in(blob);
        ewb::world_load_text(in, g_world);
    }
    // world_store.h reserves in-array type 254 for "explicitly mined"; a block of
    // literal type 254 cannot be produced by this server (ACTION caps types at
    // MAX_BLOCK_TYPE, painted base is 255) but could in principle sit in an old
    // hand-made file. Say so rather than changing it silently.
    if (g_world.reserved_coerced())
        std::cerr << "[Server] warning: " << g_world.reserved_coerced()
                  << " cell(s) in " << g_worldFile << " used the reserved block type 254"
                     " and were loaded as mined air (see world_store.h)." << std::endl;
    // A cell outside the world (y >= 256 — which a pre-7.22 server's TNT could write
    // and save — or y/x/z negative or past 24 bits in a hand-made file) was never
    // visible to any client and cannot be edited by any player. Dropped, not aliased.
    if (g_world.out_of_range())
        std::cerr << "[Server] warning: dropped " << g_world.out_of_range()
                  << " cell(s) in " << g_worldFile << " outside the world (y 0.."
                  << (SV_WORLD_HEIGHT - 1) << ", x/z 0..16777215); the next save omits them."
                  << std::endl;
    std::cout << "[Server] Loaded " << g_world.size() << " world cells from " << g_worldFile
              << " (" << (binary ? "EDMB" : "legacy text") << ", " << g_world.chunk_count()
              << " chunks)" << std::endl;
}

void saveWorld() {
    // The snapshot under the lock is a **serialised blob**, not a copy of the
    // model (stage 7.6). Before, this copied the whole cell map — 1.2 s under
    // g_worldMtx on a 13.9M-cell world, which is 1.2 s in which nobody's ACTION
    // lands. Serialising straight to EDMB is both faster and ~4 bytes/cell
    // instead of a ~19-byte text line.
    std::string blob;
    size_t cells = 0;
    const auto tSave0 = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(g_worldMtx);
        if (!editsDirty.exchange(false)) return;   // nothing changed
        cells = g_world.size();
        blob.reserve(g_saveText ? cells * 20 : cells * 4 + g_world.chunk_count() * 16 + 16);
        if (g_saveText) g_world.for_each([&](int x,int y,int z,unsigned char t,unsigned char c){
            char line[64];
            blob.append(line, (size_t)snprintf(line, sizeof line, "%d:%d:%d:%d:%d\n",
                                               x, y, z, (int)t, (int)c));
        });
        else blob = g_world.to_edmb();
    }
    const auto tSave1 = std::chrono::steady_clock::now();
    {   // stage 7.6 step 1: the snapshot's lock hold, which every ACTION waits on.
        const uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                                tSave1 - tSave0).count();
        g_saveLockMicros.fetch_add(us, std::memory_order_relaxed);
        bumpMax(g_saveLockMaxMicros, us);
    }
    // Atomic, durable, serialized write (stage 7.29): temp file -> fsync -> rename() over
    // the real one -> fsync the directory, so neither a crash mid-write nor a power loss
    // right after the rename can leave a truncated world, and two concurrent saves
    // (disconnect + timer) can't interleave. This runs outside g_worldMtx, so the fsyncs
    // delay other saves, never a player's ACTION; their cost is logged below.
    std::lock_guard<std::mutex> save(g_saveMtx);
    std::string err;
    uint64_t syncUs = 0;
    if (!ewb::write_file_durable(g_worldFile, blob, err, &syncUs)) {
        std::cerr << "[Server] save: " << err << std::endl;
        editsDirty = true; return;
    }
    g_saveBytes.store((uint64_t)blob.size(), std::memory_order_relaxed);
    const uint64_t writeUs = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - tSave1).count();
    g_saveCount.fetch_add(1, std::memory_order_relaxed);
    g_saveWriteMicros.fetch_add(writeUs, std::memory_order_relaxed);
    std::cout << "[Server] Saved world (" << cells << " cells, cap " << g_maxWorldCells
              << ", " << g_saveBytes.load(std::memory_order_relaxed) << " B, snapshot "
              << (std::chrono::duration_cast<std::chrono::microseconds>(tSave1 - tSave0).count() / 1000)
              << " ms under lock, write " << (writeUs / 1000) << " ms, of which fsync "
              << (syncUs / 1000) << " ms)." << std::endl;
}

// --- Player position persistence ----------------------------------------------
// Remember where each player was (by username) so they respawn there on rejoin.
struct SavedPos { float x, y, z; };
// Bounded (stage 7.23): keyed on a username, which is untrusted input, so an
// unbounded map here let a client that JOINs under a fresh name every few seconds grow
// the process and the whole-file rewrite in savePlayerPos() forever. Least-recently-
// updated rows are evicted past --max-saved-positions.
static const size_t SV_MAX_SAVED_POS_DEFAULT = 10000;
static size_t g_maxSavedPos = SV_MAX_SAVED_POS_DEFAULT;
static ewb::LruTable<SavedPos> g_playerPos(SV_MAX_SAVED_POS_DEFAULT);
static std::mutex g_posMtx;
// Empty means "derive from --world's directory" (stage 7.15); --players-file
// overrides outright. Resolved once in main() before loadPlayerPos() runs.
static std::string g_posFile = "";
static std::atomic<bool> g_posDirty{false};

void loadPlayerPos() {
    std::ifstream f(g_posFile);
    if (!f) return;
    std::string line;
    size_t dropped = 0, rows = 0;
    std::lock_guard<std::mutex> lk(g_posMtx);
    while (std::getline(f, line)) {
        size_t p1 = line.find(':');          // username can't contain ':' (protocol delimiter)
        if (p1 == std::string::npos) continue;
        std::string name = line.substr(0, p1);
        float x,y,z;
        if (sscanf(line.c_str()+p1+1, "%f:%f:%f", &x,&y,&z) != 3) continue;
        // A row is only as trustworthy as whatever wrote it. `%f` reads `inf`,
        // `nan` and `1e+38` back just as happily as a position, and a server that
        // ran before stage 7.16 could have persisted exactly that — so the file is
        // re-validated on load rather than trusted because the server wrote it.
        if (!ewb::move_pos_valid(x, y, z)) { ++dropped; continue; }
        g_playerPos.put(name, { x,y,z });   // file order is oldest -> newest (savePlayerPos)
        ++rows;
    }
    std::cout << "[Server] Loaded " << g_playerPos.size() << " player positions." << std::endl;
    if (rows > g_playerPos.size())
        std::cerr << "[Server] " << g_posFile << ": kept the newest " << g_playerPos.size()
                  << " of " << rows << " rows (--max-saved-positions " << g_maxSavedPos << ")." << std::endl;
    if (dropped)
        std::cerr << "[Server] " << g_posFile << ": ignored " << dropped
                  << " out-of-range player position(s)." << std::endl;
}

// --- World default spawn (stage 5.3) ----------------------------------------
// `eden_spawn.txt` (one line `x:y:z`, written by eden_import) is the position a
// joining player with no eden_players.txt row of their own is sent to. `--spawn
// x:y:z` sets it directly and skips the file. A missing file is silent; a
// malformed one warns and is ignored — never fatal.
void loadSpawn() {
    if (g_haveWorldSpawn) return;   // --spawn already provided it
    std::ifstream f(g_spawnFile);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        ewb::Spawn s;
        bool skip = false;
        if (ewb::parse_spawn_line(line, s, skip)) {
            // `spawn_store.h` owns the grammar, the range is the caller's policy —
            // and this caller puts the value straight on the wire as `SPAWN`
            // (stage 7.16), so it takes the same bound a player's own position does.
            if (!ewb::move_pos_valid(s.x, s.y, s.z)) {
                std::cerr << "[Server] " << g_spawnFile
                          << ": spawn out of range; ignoring" << std::endl;
                return;
            }
            g_worldSpawn = s;
            g_haveWorldSpawn = true;
            return;
        }
        if (!skip) {
            std::cerr << "[Server] " << g_spawnFile
                      << ": ignoring malformed spawn line" << std::endl;
            return;
        }
    }
}

// --- Message of the day -----------------------------------------------------
// `eden_motd.txt` beside the world (or `--motd-file`) is the operator-settable
// welcome message every joining player is shown, between the built-in welcome
// line and `CAPS:region`. Absent or empty is normal and silent, like `--signs`.
//
// The wire lines are kept pre-formatted so a join costs a copy, not a parse, and
// are swapped under g_motdMtx by the control socket's `motd reload` — an
// operator edits the file and pushes it without restarting the process, exactly
// as `signs reload` does for the sign sidecar.
static std::vector<std::string> g_motdLines;   // wire-ready "[Server] ...\n"
static std::mutex               g_motdMtx;

// Read the sidecar into g_motdLines. Returns how many lines are live afterwards,
// which is what startup logs and what `motd reload` answers with. A missing file
// is not an error: it clears the MOTD and reports zero.
static size_t loadMotd() {
    std::vector<std::string> lines;
    size_t dropped = 0;
    {
        std::ifstream f(g_motdFile);
        if (f) lines = ewb::format_motd_burst(
                   ewb::parse_motd(f, ewb::MOTD_MAX_LINES, ewb::MOTD_MAX_LINE, &dropped));
    }
    if (dropped)
        std::cerr << "[Server] " << g_motdFile << ": MOTD capped at " << ewb::MOTD_MAX_LINES
                  << " lines; dropped " << dropped << "." << std::endl;
    const size_t n = lines.size();
    {
        std::lock_guard<std::mutex> lk(g_motdMtx);
        g_motdLines = std::move(lines);
    }
    return n;
}

void savePlayerPos() {
    std::vector<std::pair<std::string, SavedPos>> snap;   // oldest -> newest: reload restores recency
    {
        std::lock_guard<std::mutex> lk(g_posMtx);
        if (!g_posDirty.exchange(false)) return;
        snap.reserve(g_playerPos.size());
        g_playerPos.for_each([&](const std::string& k, const SavedPos& v) { snap.emplace_back(k, v); });
    }
    std::ostringstream body;
    for (const auto& kv : snap)
        body << kv.first << ":" << kv.second.x << ":" << kv.second.y << ":" << kv.second.z << "\n";
    std::lock_guard<std::mutex> save(g_saveMtx);   // durable temp+rename, serialized
    std::string err;
    if (!ewb::write_file_durable(g_posFile, body.str(), err)) {
        std::cerr << "[Server] save: " << err << std::endl;
        g_posDirty = true;
    }
}

// Write `content` to `path` atomically and durably (temp file + fsync + rename + directory
// fsync, stage 7.29), serialized on g_saveMtx like the world/player writes so a crash
// mid-write can't truncate a sidecar and two writers can't interleave. Returns false on error.
static bool writeFileAtomic(const std::string& path, const std::string& content) {
    std::lock_guard<std::mutex> save(g_saveMtx);
    std::string err;
    if (!ewb::write_file_durable(path, content, err)) {
        std::cerr << "[Server] write: " << err << std::endl;
        return false;
    }
    return true;
}

// --- Audit log (stage 3.4) ---------------------------------------------------
//
// One channel for everything that *changes* server state, whoever changed it:
// the operator through the control socket, a player through an in-chat command.
// Before 3.4 the two tiers disagreed — Tier 1 logged unconditionally, Tier 2
// logged only level-2 commands, and level-1 edits needed `--verbose`. A public
// server wants a record of who changed what regardless of how chatty it feels
// like being, so the audit channel is not `--verbose`-gated and never will be.
//
// Format is one line, greppable, timestamped in UTC because a log without a
// clock is not evidence:
//
//     [Audit] 2026-09-09T14:03:11Z control fill 4096 cells = 2 @ 65530,32,65530
//     [Audit] 2026-09-09T14:03:19Z player:hagge (level 1) //set: 512 cell(s)
//
// It goes to stdout — under systemd that is journald, which rotates and rate
// limits it — and additionally to `--audit-file` if the operator wants a copy
// that survives independently of the journal.
static std::mutex g_auditMtx;

static std::string utcStamp() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

/// `actor` is `control` for the operator socket or `player:<name>` for a
/// command typed in chat; `what` is the verb and its outcome.
static void auditLog(const std::string& actor, const std::string& what) {
    const std::string line = "[Audit] " + utcStamp() + " " + actor + " " + what;
    std::lock_guard<std::mutex> lk(g_auditMtx);
    std::cout << line << std::endl;
    if (g_auditFile.empty()) return;
    // Append-only, reopened per line: an operator may rotate or truncate the
    // file underneath us, and a missing audit line is worse than a slow one.
    std::ofstream f(g_auditFile, std::ios::app);
    if (f) f << line << "\n";
}

// --- Ban list + op levels (stage 3.2) ----------------------------------------
static ewb::BanList g_bans;
static std::mutex    g_banMtx;
static ewb::OpsFile  g_ops;
static std::mutex    g_opsMtx;

// Per-IP wrong-password throttle (stage 1.10). Touched from both the accept loop
// (blocked() gate) and per-client threads (record_failure() at JOIN), so unlike
// the accept-loop-only ConnectLimiter it carries its own lock. Threshold is set
// from --auth-fail-limit in main(); window / escalating cooldown are compiled in.
static ewb::AuthFailureLimiter g_authFail;
static std::mutex              g_authFailMtx;

// Per-name PINs (stage 8.6; model in auth.h). ⚠️ Lock order: g_authMtx may be taken
// *before* clientsMutex (a login publishes `verified` under both, so a concurrent
// `passwd` cannot slip between the check and the flag), never after it — nothing
// that holds clientsMutex takes g_authMtx. g_authEditMtx serializes the control
// socket's passwd/unpasswd writers, like g_zonesEditMtx.
static ewb::AuthFile g_auth;
static std::mutex    g_authMtx;
static std::mutex    g_authEditMtx;
// Failed `/login`s per IP: the same escalating lockout as the JOIN password
// (stage 1.10), in its own table so a typo'd PIN never locks an IP out of joining.
static ewb::AuthFailureLimiter g_loginFail;
static std::mutex              g_loginFailMtx;

void loadBans() {
    std::ifstream f(g_banFile);
    if (!f) return;
    std::lock_guard<std::mutex> lk(g_banMtx);
    g_bans.load(f);
    const size_t n = g_bans.names.size() + g_bans.ips.size();
    if (n) std::cout << "[Server] Loaded " << n << " ban entries from " << g_banFile << std::endl;
}
void saveBans() {
    std::ostringstream ss;
    { std::lock_guard<std::mutex> lk(g_banMtx); g_bans.serialize(ss); }
    writeFileAtomic(g_banFile, ss.str());
}
void loadOps() {
    std::ifstream f(g_opsFile);
    if (!f) return;
    std::lock_guard<std::mutex> lk(g_opsMtx);
    g_ops.load(f);
    if (!g_ops.entries.empty())
        std::cout << "[Server] Loaded " << g_ops.entries.size() << " op levels from " << g_opsFile << std::endl;
}
void saveOps() {
    std::ostringstream ss;
    { std::lock_guard<std::mutex> lk(g_opsMtx); g_ops.serialize(ss); }
    writeFileAtomic(g_opsFile, ss.str());
}

// eden_zones.txt (stage 8.2; grammar in zones.h). No file is the normal case: no zones,
// nothing said. A file that fails to parse is a **refusal to start**, not a warning —
// the loader is all-or-nothing, so carrying on would silently open every zone the
// operator meant to protect. Returns false with `err` set.
static bool loadZones(std::string& err) {
    std::ifstream f(g_zonesFile);
    if (!f) return true;
    ewb::ZoneSet set;
    std::string why;
    int line = 0;
    if (!ewb::ZoneSet::load(f, set, &why, &line)) {
        err = g_zonesFile + ":" + std::to_string(line) + ": " + why;
        return false;
    }
    bool anyPins;
    { std::lock_guard<std::mutex> lk(g_authMtx); anyPins = !g_auth.empty(); }
    size_t enforced = 0;
    for (const ewb::Zone& z : set.zones()) {
        if (z.enforced) ++enforced;
        if (z.enforced && z.hasLevel && !anyPins)
            std::cerr << "[Server] zone '" << z.name << "' is bypassed by players logged in at level "
                      << z.level << "+, but no name has a PIN yet, so nobody can bypass it"
                         " (edenctl passwd <name>)." << std::endl;
    }
    const size_t n = set.size();
    {
        std::lock_guard<std::mutex> lk(g_zonesMtx);
        g_zones = std::make_shared<const ewb::ZoneSet>(std::move(set));
    }
    std::cout << "[Server] Loaded " << n << " zone(s) from " << g_zonesFile << " (" << enforced
              << " enforced)." << std::endl;
    return true;
}

// Save `next` durably and publish it as the live set (stage 8.3; shared with the
// in-game /zone of 8.7). The caller holds g_zonesEditMtx and built `next` from the
// current snapshot, so no concurrent zone edit is lost.
static bool zonePublish(ewb::ZoneSet&& next, std::string& err) {
    if (!ewb::zone_save_file(g_zonesFile, next, err)) return false;
    std::lock_guard<std::mutex> lk(g_zonesMtx);
    g_zones = std::make_shared<const ewb::ZoneSet>(std::move(next));
    return true;
}

// eden_auth.txt (stage 8.6; grammar in auth.h). No file is normal: no PINs, and
// every name behaves as it did before 8.6. A file that fails to parse refuses the
// start, like eden_zones.txt: running on without it would hand every PIN-protected
// name's op level back to whoever claims the name.
static bool loadAuth(std::string& err) {
    std::ifstream f(g_authFile);
    if (!f) return true;
    ewb::AuthFile set;
    std::string why;
    int line = 0;
    if (!ewb::AuthFile::load(f, set, &why, &line)) {
        err = g_authFile + ":" + std::to_string(line) + ": " + why;
        return false;
    }
    struct stat st;
    if (::stat(g_authFile.c_str(), &st) == 0 && (st.st_mode & 077))
        std::cerr << "[Server] warning: " << g_authFile << " is readable by other users (mode "
                  << std::oct << (st.st_mode & 0777) << std::dec
                  << "); it holds PIN hashes — chmod 600 it." << std::endl;
    const size_t n = set.size();
    { std::lock_guard<std::mutex> lk(g_authMtx); g_auth = std::move(set); }
    if (n) std::cout << "[Server] Loaded " << n << " login PIN(s) from " << g_authFile << "." << std::endl;
    return true;
}

// Write `next` as the auth file, 0600, durably. The caller holds g_authEditMtx.
static bool saveAuth(const ewb::AuthFile& next, std::string& err) {
    std::lock_guard<std::mutex> save(g_saveMtx);
    return ewb::write_file_durable(g_authFile, next.serialize(), err, nullptr, 0600);
}

// Random bytes for PINs and salts: the kernel CSPRNG. getentropy() takes at most
// 256 bytes a call; /dev/urandom is the fallback for a libc without it.
static bool randomBytes(uint8_t* p, size_t n) {
    size_t off = 0;
    while (off < n) {
        const size_t take = n - off < 256 ? n - off : 256;
        if (getentropy(p + off, take) != 0) break;
        off += take;
    }
    if (off == n) return true;
    std::ifstream u("/dev/urandom", std::ios::binary);
    return u && u.read(reinterpret_cast<char*>(p + off), (std::streamsize)(n - off)) && u.gcount() == (std::streamsize)(n - off);
}

static bool nameHasPin(const std::string& name) {
    std::lock_guard<std::mutex> lk(g_authMtx);
    return g_auth.has(name);
}
static bool nameHasAnyPin() {
    std::lock_guard<std::mutex> lk(g_authMtx);
    return !g_auth.empty();
}

// The op level a session actually gets (auth.h `auth_effective_level`): a name
// with a PIN is held to --default-level (or its own level, if lower) until it
// logs in. Re-read per use, so an `op`, `deop` or `passwd` applies on the next
// command, not the next session.
static int sessionLevel(const std::string& name, bool verified) {
    const bool pin = nameHasPin(name);
    int file;
    { std::lock_guard<std::mutex> lk(g_opsMtx); file = g_ops.level_of(name, g_defaultLevel); }
    return ewb::auth_effective_level(file, g_defaultLevel, pin, verified);
}

// Record a player's latest position (by username).
//
// The bound is re-checked here, not only at ingest (stage 7.16): this map is
// what `SPAWN` hands a returning player and what `eden_players.txt` persists, so
// it is the one place every writer — `POS`, `POSVEL`, `/tp` — has to pass.
static void rememberPos(const std::string& name, float x, float y, float z){
    if(name.empty()) return;
    if(!ewb::move_pos_valid(x, y, z)) return;
    std::lock_guard<std::mutex> lk(g_posMtx);
    g_playerPos.put(name, { x,y,z });
    g_posDirty = true;
}

// --- Signs (stage 1.5, plus player sign writes) --------------------------------
// The whole `SIGNP` burst is kept pre-formatted and re-sent verbatim per SIGNQ, so
// nothing is built per request. It is rebuilt under g_signMtx whenever the list
// changes: the control socket's `signs reload|add|rm`, a player's sign write, or an
// edit that turns a signed block to air (stage 7.2).
//
// ⚠️ Lock order: g_worldMtx, then g_signMtx — never the reverse. worldSet() takes
// g_signMtx under the world lock when it stores air on a signed block, so nothing that
// holds g_signMtx may take g_worldMtx.
static std::vector<ewb::Sign> g_signs;
// Shared, not copied per SIGNQ (stage 7.30): the burst is up to a few hundred KB and a
// client asks for it up to SV_MAX_SIGNQ_PER_SESSION times. Immutable once built — a
// change swaps in a new blob, so a queued reply keeps the one it was handed.
static std::shared_ptr<const std::string> g_signBlob;
static std::mutex             g_signMtx;

// The blocks that carry at least one sign, keyed by wkey(). This is what keeps
// worldSet()'s hook to one hash lookup per cell: a 1M-cell `fill` or a TNT chain must
// not scan 20k signs per cell. Rebuilt with the list (loadSigns, signsChangedLocked),
// which is O(signs) and fine at the cap. g_signBlockCount mirrors its size, so in a world
// with no signs worldSet() never takes g_signMtx at all. Guarded by g_signMtx.
static std::unordered_set<uint64_t> g_signBlocks;
static std::atomic<size_t>          g_signBlockCount{0};

// Signs worldSet() took off a block, waiting for their audit line. They are recorded
// under g_worldMtx, where a log write is off limits, and written by drainSignRemovals()
// with no lock held. The actor is captured when the sign is removed rather than when the
// line is written, so a drain on another player's thread can't put it under their name.
struct SignRemoval {
    std::string actor;
    int x = 0, y = 0, z = 0;
    std::vector<ewb::Sign> signs;
};
static std::vector<SignRemoval> g_signRemovals;             // guarded by g_signMtx
static bool                     g_signBurstStale = false;   // guarded by g_signMtx; see removeSignsOnBlock

// Who is editing on this thread, for those audit lines: `player:<name>` on a client's
// thread once it has joined, `control` on a control connection, `server` otherwise.
// A client thread can no longer edit before it sets this: stage 7.17's gate drops
// `ACTION` pre-JOIN, so a player's edit is never audited as the server's.
static thread_local std::string t_editActor = "server";

// Caller holds g_signMtx.
static void rebuildSignIndexLocked() {
    g_signBlocks.clear();
    for (const ewb::Sign& s : g_signs) g_signBlocks.insert(wkey(s.x, s.y, s.z));
    g_signBlockCount = g_signBlocks.size();
}

void loadSigns() {
    std::ifstream f(g_signFile);
    if (!f) return;   // no sidecar is the normal case — say nothing
    std::vector<ewb::Sign> signs;
    std::string line;
    size_t lineNo = 0, bad = 0, dropped = 0;
    while (std::getline(f, line)) {
        ++lineNo;
        ewb::Sign s;
        bool skip = false;
        if (!ewb::parse_sign_line(line, s, skip)) {
            if (skip) continue;
            if (++bad <= 5)
                std::cerr << "[Server] " << g_signFile << ":" << lineNo
                          << ": malformed sign line, ignored." << std::endl;
            continue;
        }
        if (signs.size() >= SV_MAX_SIGNS) { ++dropped; continue; }
        signs.push_back(std::move(s));
    }
    if (bad > 5)  std::cerr << "[Server] (" << (bad - 5) << " more malformed sign lines)" << std::endl;
    if (dropped)  std::cerr << "[Server] sign cap reached (" << SV_MAX_SIGNS << "); dropped "
                            << dropped << "." << std::endl;

    std::string blob = ewb::format_sign_burst(signs);
    const size_t n = signs.size(), bytes = blob.size();
    {
        std::lock_guard<std::mutex> lk(g_signMtx);
        g_signs = std::move(signs);
        g_signBlob = std::make_shared<const std::string>(std::move(blob));
        g_signBurstStale = false;
        rebuildSignIndexLocked();
    }
    std::cout << "[Server] Loaded " << n << " signs from " << g_signFile
              << " (" << bytes << " B burst)." << std::endl;
}

// A player's sign write (handleSignWrite) changes the list in memory; the sidecar
// follows on the world's cadence — autosave, a disconnect, the control socket's
// save/stop. Operator `signs add|rm` call saveSigns() at once. Either way every
// write of the file goes through saveSigns(), so there is one serialised path.
static std::atomic<bool> g_signsDirty{false};
static std::mutex        g_signSaveMtx;   // holds saveSigns()' snapshot + write together

// The sign list changed: rebuild the SIGNQ burst and mark the sidecar for the next
// saveSigns(). Caller holds g_signMtx.
static void signsChangedLocked() {
    rebuildSignIndexLocked();
    g_signBlob = std::make_shared<const std::string>(ewb::format_sign_burst(g_signs));
    g_signBurstStale = false;
    g_signsDirty = true;
}

// worldSet() stored air at `k`: take every sign off that block, whatever its face.
// Caller holds g_worldMtx (lock order above).
//
// The list and the index change here and the sidecar is marked dirty, but the SIGNQ
// burst is only marked stale: drainSignRemovals() rebuilds it once when the edit is
// done, and serveSigns() does if a SIGNQ gets in first. Rebuilding it here would format
// every sign in the world once per signed block, under the world lock — a `fill` across
// a sign-dense build would pay that hundreds of times over.
static void removeSignsOnBlock(uint64_t k) {
    if (g_signBlockCount.load(std::memory_order_relaxed) == 0) return;
    std::lock_guard<std::mutex> lk(g_signMtx);
    if (g_signBlocks.erase(k) == 0) return;
    g_signBlockCount = g_signBlocks.size();
    SignRemoval r;
    r.actor = t_editActor;
    // Coordinates from the key rather than the caller's ints, so the list and the
    // index can't disagree about which block this is.
    wunkey(k, r.x, r.y, r.z);
    if (ewb::remove_signs_on_block(g_signs, r.x, r.y, r.z, &r.signs) == 0) return;
    g_signRemovals.push_back(std::move(r));
    g_signBurstStale = true;
    g_signsDirty = true;
}

// Rebuild a burst removeSignsOnBlock() left stale. Caller holds g_signMtx.
static void refreshSignBurstLocked() {
    if (!g_signBurstStale) return;
    g_signBlob = std::make_shared<const std::string>(ewb::format_sign_burst(g_signs));
    g_signBurstStale = false;
}

// Write the audit lines for signs removed with their blocks, one per block, and bring
// the SIGNQ burst up to date. Call with no lock held, once the edit has released
// g_worldMtx and written its own audit line (cause, then effect): the ACTION handler,
// weFinish, //undo and //redo, and the control socket's setblock/fill. saveSigns() calls
// it too, so a path that misses it still gets its lines by the next autosave.
static void drainSignRemovals() {
    std::vector<SignRemoval> done;
    {
        std::lock_guard<std::mutex> lk(g_signMtx);
        refreshSignBurstLocked();
        done.swap(g_signRemovals);
    }
    for (const SignRemoval& r : done) {
        std::string what = r.signs.size() == 1 ? "sign" : std::to_string(r.signs.size()) + " signs";
        what += " removed (block became air) at " + std::to_string(r.x) + "," +
                std::to_string(r.y) + "," + std::to_string(r.z) + ": ";
        for (size_t i = 0; i < r.signs.size(); ++i) what += (i ? " | " : "") + r.signs[i].text;
        auditLog(r.actor, what);
    }
}

// Write eden_signs.txt if the list changed since the last write. The file is
// formatted under g_signMtx and written after releasing it, so a slow disk — or a
// large world save holding g_saveMtx — never stalls a SIGNQ or a player's sign.
// g_signSaveMtx spans snapshot and write, so of two racing saves the later snapshot
// is always the one left on disk.
void saveSigns() {
    drainSignRemovals();   // the backstop for any edit path that didn't drain
    std::lock_guard<std::mutex> serial(g_signSaveMtx);
    std::string file;
    size_t n = 0;
    {
        std::lock_guard<std::mutex> lk(g_signMtx);
        if (!g_signsDirty.exchange(false)) return;
        for (const ewb::Sign& s : g_signs) file += ewb::format_sign_file_line(s);
        n = g_signs.size();
    }
    if (!writeFileAtomic(g_signFile, file)) { g_signsDirty = true; return; }
    std::cout << "[Server] Saved signs (" << n << ")." << std::endl;
}

// Drop signs on blocks the world stores as air (stage 7.2). Before 7.2 a sign outlived
// its block, so a world saved by an older server can hold orphans that reappear as soon
// as someone builds on that block again. A cell the model doesn't hold is untouched base
// terrain and may be solid, so an absent cell never drops a sign.
//
// Run once both the world and the signs are loaded: at startup and after `signs reload`.
// When anything is dropped the sidecar is rewritten, so the file matches memory and the
// reload guard (which refuses while signs are unsaved) doesn't trip.
static size_t pruneOrphanSigns() {
    std::vector<ewb::Sign> dropped;
    {
        std::lock_guard<std::mutex> wl(g_worldMtx);   // lock order: world, then signs
        std::lock_guard<std::mutex> sl(g_signMtx);
        ewb::prune_signs(g_signs, [](const ewb::Sign& s) {
            // "present *and* air" — an absent cell is untouched base terrain and
            // still holds its sign. The chunk store keeps that distinction
            // (stage 7.6): a mined cell reads back as logical type 0.
            Cell c;
            return g_world.get(s.x, s.y, s.z, c) && c.type == SV_AIR;
        }, &dropped);
        if (!dropped.empty()) signsChangedLocked();
    }
    if (dropped.empty()) return 0;
    std::cout << "[Server] dropped " << dropped.size() << " sign(s) on removed blocks" << std::endl;
    const size_t shown = std::min<size_t>(dropped.size(), 20);
    for (size_t i = 0; i < shown; ++i)
        std::cout << "[Server]   " << dropped[i].x << "," << dropped[i].y << "," << dropped[i].z
                  << ": " << dropped[i].text << std::endl;
    if (dropped.size() > shown)
        std::cout << "[Server]   (" << (dropped.size() - shown) << " more)" << std::endl;
    saveSigns();
    return dropped.size();
}

// Keep a persistent registration with the matchmaker for as long as we run.
// The matchmaker treats the open TCP connection as "online"; if we exit it drops
// and we're removed. Reconnects on failure.
//
// Wire (Eden dev, WORKING/matchmakerinfo.txt; own matchmaker in edenmatch.cpp):
//   -> REGISTER:<name>:<port>:<hasPassword>[:<advertiseIP>]
//   <- REGISTERED
//   -> PING:<n>      every ~20s, comfortably inside the ~45s TTL. `n` is the
//                    count channel (production matchmaker source, 2026-09-12
//                    community drop; see WORKING/latestref-analysis-2026-09-12.md
//                    §2 — supersedes the earlier "PING is argument-less" reading
//                    of the lossy dev paste in WORKING/matchmakerinfo.txt).
static int joinedPlayerCount() {
    std::lock_guard<std::mutex> lock(clientsMutex);
    return static_cast<int>(playerInfoMap.size());
}

void matchmakerThread() {
    while (serverRunning) {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s >= 0) {
            sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(g_matchPort);
            if (inet_pton(AF_INET, g_matchHost.c_str(), &a.sin_addr) > 0 &&
                connect(s, (sockaddr*)&a, sizeof(a)) == 0) {
                // Strip ':' / control bytes from the name — the matchmaker line is
                // positionally colon-delimited, so a raw ':' in the name would
                // shift every field after it.
                std::string safeName;
                for (char c : g_serverName)
                    if (c != ':' && (unsigned char)c >= 0x20) safeName += c;
                std::string reg = "REGISTER:" + safeName + ":" + std::to_string(g_port) + ":" +
                                  (g_password.empty() ? "0" : "1") +
                                  (g_advertiseIP.empty() ? "" : ":" + g_advertiseIP) + "\n";
                send(s, reg.c_str(), reg.size(), 0);

                // Read the REGISTERED acknowledgement (best-effort — a matchmaker
                // that stays silent is not fatal, the registration may still be live).
                struct timeval tv{2, 0};
                setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                char ack[64] = {0};
                ssize_t an = recv(s, ack, sizeof(ack) - 1, 0);
                bool acked = (an > 0 && std::string(ack).rfind("REGISTERED", 0) == 0);
                std::cout << "[Server] "
                          << (acked ? "Registered with" : "Sent registration to (no ack from)")
                          << " matchmaker " << g_matchHost << ":" << g_matchPort
                          << " as \"" << g_serverName << "\"" << (g_password.empty() ? "" : " [locked]")
                          << std::endl;

                // Send a count immediately so a fresh server doesn't advertise a
                // stale/zero row for up to 20s until the first heartbeat, then
                // repeat every ~20s, comfortably inside the ~45s TTL.
                while (serverRunning) {
                    std::string ping = "PING:" + std::to_string(joinedPlayerCount()) + "\n";
                    if (send(s, ping.c_str(), ping.size(), 0) <= 0) break;
                    for (int i = 0; i < 20 && serverRunning; i++)
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                }
            }
            close(s);
        }
        if (!serverRunning) break;
        std::this_thread::sleep_for(std::chrono::seconds(5));  // retry
    }
}

// --- per-client output: one writer thread owns the socket (stages 7.3 / 7.4) --
//
// ⚠️ **Nothing below this section may call send() on a player's socket.** The one
// deliberate exception is the accept loop's "Server full" line, which is written
// before any ClientOut exists and is documented there.
//
// Before 7.3 nothing serialised writes to a client socket: serveRegion() streamed
// SNAPZ frames with no lock held while broadcastMessage(), /msg, weSay(),
// weTeleport() and ctlKick() wrote to the same fd from other threads. A blocking
// send() larger than the available send buffer releases the CPU while it waits, so
// another thread's POSVEL landed *inside* a base64 payload and the frame arrived
// short or undecodable. Records are sorted (z, x, y, flag) and framed at a flat
// 3000, so one destroyed frame is one 1-block-wide row across the whole reply box:
// the "world resets in strips on a weak link" report. A per-socket write mutex
// would fix that and make 7.4 worse — a blocked region burst would hold it while a
// broadcaster waited on it *under clientsMutex*. So the fix is structural instead:
// every producer enqueues, exactly one thread per client drains, and a queue entry
// is always a whole line or a whole frame (out_queue.h).
//
// That also fixes 7.4: broadcastMessage() used to hold clientsMutex across its
// blocking send(), and JOIN needs the same mutex, so one backed-up client froze
// every join, chat line, movement relay and block edit server-wide. No syscall now
// runs under any shared lock.
//
// **Lock order: any server lock -> g_outsMtx -> ClientOut::m.** Nothing inside a
// g_outsMtx or a ClientOut::m critical section takes another lock — those two are
// terminal — so no ordering against g_worldMtx, g_signMtx, g_posMtx, g_saveMtx or
// clientsMutex has to be remembered at the ~30 call sites that produce output.
struct ClientOut {
    SOCKET fd = INVALID_SOCKET;
    int    id = 0;

    std::mutex              m;         // guards everything below. Innermost lock.
    std::condition_variable cv;        // work arrived, or the reader asked us to stop
    std::condition_variable drained;   // the queue went idle (flushOut waits here)
    ewb::OutQueue q;
    std::string name;                  // log label; "client #N" until JOIN names them
    bool closing    = false;           // reader wants the writer to drain and exit
    bool tooSlow    = false;           // over the world-state budget; finish hi, then go
    bool dead       = false;           // the writer gave up (peer gone / write timeout)
    bool writing    = false;           // an item is in the writer's hand, not the queue
    bool dropWarned = false;           // "not keeping up" logged once per client
    // Written by the reader thread on every recv, read by a JOIN that wants this
    // client's name (stage 7.5) — hence atomic rather than under `m`.
    std::atomic<double> lastRecv{0.0};      // monoSeconds() of the last byte received
    std::atomic<bool>   evicted{false};     // a same-address rejoin took our name; stand down
    uint64_t sentBytes = 0;

    std::thread th;                    // the writer; joined by the reader in closeOut()
};

static std::mutex g_outsMtx;                                  // guards g_outs only
static std::map<SOCKET, std::shared_ptr<ClientOut>> g_outs;   // live client writers

// Look a client's output up by socket. Returns a *shared* pointer, so the client
// may depart between this call and the enqueue and the object stays alive.
// ⚠️ Never call this while holding g_outsMtx or a ClientOut::m.
static std::shared_ptr<ClientOut> outFor(SOCKET s) {
    std::lock_guard<std::mutex> lk(g_outsMtx);
    auto it = g_outs.find(s);
    return it == g_outs.end() ? std::shared_ptr<ClientOut>() : it->second;
}

// Every live client except one, as owning pointers. This is the "snapshot, release,
// then enqueue" half of the 7.4 fix: broadcasts hold g_outsMtx for a map walk and
// nothing else, and never hold it across an enqueue.
static std::vector<std::shared_ptr<ClientOut>> outSnapshot(SOCKET except) {
    std::vector<std::shared_ptr<ClientOut>> v;
    std::lock_guard<std::mutex> lk(g_outsMtx);
    v.reserve(g_outs.size());
    for (const auto& kv : g_outs)
        if (kv.first != except) v.push_back(kv.second);
    return v;
}

// A client whose world-state backlog is over budget. The update cannot be dropped —
// a missing ACTION relay silently diverges that client's world with nothing left to
// resync it, which is the bug class this whole section exists to kill — so the
// client goes instead and resyncs properly on rejoin.
//
// ⚠️ This **marks** the client and returns; it does not tear the connection down
// here. Two reasons. Producers must never block, and it is called from inside a
// broadcast loop. And "too slow" does not mean "not reading" — a client draining at
// 200 KB/s while an operator's `fill` relay arrives at 2 MB/s is over budget and
// still perfectly able to read one more line. So the notice goes into the
// latency-sensitive queue, which drains first, and the writer delivers it, drops
// the rest, and closes the socket (shutdown(), which wakes the reader for the
// normal cleanup path — the ctlKick convention).
static void dropTooSlow(const std::shared_ptr<ClientOut>& o) {
    if (!o) return;
    std::string who;
    size_t queued = 0;
    {
        std::lock_guard<std::mutex> lk(o->m);
        if (o->dead || o->closing || o->tooSlow) return;   // once per client
        o->tooSlow = true;
        who        = o->name;
        queued     = o->q.queued_bytes();
        o->q.push_hi("[Server] Connection too slow.\n");
    }
    g_slowDrops.fetch_add(1, std::memory_order_relaxed);
    std::cerr << "[Server] " << who << " is " << queued
              << " B behind on world updates (--client-world-max " << g_worldboxMax
              << "); disconnecting. They will resync on rejoin." << std::endl;
    o->cv.notify_all();
}

// --- producers. All four are safe from any thread and are no-ops once the client
// has gone. None of them blocks on the network; none takes another lock. ---------

// A latency-sensitive, order-insensitive line: movement, chat, [Server] notices,
// the welcome, CAPS, SPAWN, PONG.
static void pushHi(const std::shared_ptr<ClientOut>& o, const std::string& line) {
    if (!o || line.empty()) return;
    bool warn = false;
    std::string who;
    {
        std::lock_guard<std::mutex> lk(o->m);
        if (o->closing || o->dead || o->tooSlow) return;
        if (!o->q.push_hi(line) && !o->dropWarned) { o->dropWarned = true; warn = true; who = o->name; }
    }
    o->cv.notify_one();
    if (warn)
        std::cerr << "[Server] " << who << " is not keeping up; dropping stale movement and"
                     " chat lines (their world state is still delivered in full)." << std::endl;
}

// World state the client cannot ask for again: an ACTION relay, a SIGNP relay, a
// refused-edit correction. Over budget disconnects them — see dropTooSlow().
static void pushWorld(const std::shared_ptr<ClientOut>& o, std::string blob) {
    if (!o || blob.empty()) return;
    bool over;
    {
        std::lock_guard<std::mutex> lk(o->m);
        if (o->closing || o->dead || o->tooSlow) return;
        over = !o->q.push_world(std::move(blob));
    }
    if (over) { dropTooSlow(o); return; }
    o->cv.notify_one();
}

// Same contract as pushWorld(), for a blob one broadcast is handing to several
// clients (stage 7.13's broadcastWorld fix): the caller builds the shared_ptr
// once and every target's queue takes a refcount bump instead of its own copy
// of the whole blob.
static void pushWorld(const std::shared_ptr<ClientOut>& o,
                       const std::shared_ptr<const std::string>& blob) {
    if (!o || !blob || blob->empty()) return;
    bool over;
    {
        std::lock_guard<std::mutex> lk(o->m);
        if (o->closing || o->dead || o->tooSlow) return;
        over = !o->q.push_world(blob);
    }
    if (over) { dropTooSlow(o); return; }
    o->cv.notify_one();
}

// The answer to a request the client made and can make again: the SIGNQ burst.
// False means refuse the request — the client re-asks, and nothing about the world
// is lost. (The legacy snapshot is *not* one of these: it is unsolicited, so it
// goes through pushWorld.) The blob is shared rather than owned (stage 7.30): the
// SIGNQ burst is one immutable string every client is answered from.
static bool pushReply(const std::shared_ptr<ClientOut>& o,
                      const std::shared_ptr<const std::string>& blob) {
    if (!o || !blob) return false;
    bool ok;
    {
        std::lock_guard<std::mutex> lk(o->m);
        if (o->closing || o->dead || o->tooSlow) return false;
        ok = o->q.push_reply(blob);
    }
    if (ok) o->cv.notify_one();
    return ok;
}

// A scanned REGION reply, encoded frame by frame by the writer. False means this
// client already has --client-region-queue replies in flight: refuse, the same
// answer the 750 ms gap limiter gives.
static bool pushRegion(const std::shared_ptr<ClientOut>& o, ewb::RegionJob job) {
    if (!o) return false;
    bool ok;
    {
        std::lock_guard<std::mutex> lk(o->m);
        if (o->closing || o->dead || o->tooSlow) return false;
        ok = o->q.push_region(std::move(job));
    }
    if (ok) o->cv.notify_one();
    return ok;
}

// Socket-addressed convenience wrappers, so the ~30 existing output call sites keep
// reading the way they did. Each is one g_outsMtx map lookup; a broadcast uses
// outSnapshot() instead of calling these in a loop.
//
// ⚠️ There is deliberately **no `sendLine(SOCKET, const char*, size_t)` overload**
// (stage 7.16). Every call site passed an `snprintf` return as the length, and
// `snprintf` returns what it *would* have written — so an over-long line made
// `std::string(d, n)` read past the stack buffer and put the spill on the wire.
// Build the line as a std::string; `spawnLine()` below is the safe shape for a
// printf-style one.
static void sendLine(SOCKET s, const std::string& line)      { pushHi(outFor(s), line); }
static void sendWorldTo(SOCKET s, const std::string& blob)   { pushWorld(outFor(s), blob); }

// The one `SPAWN:x:y:z` formatter (join restore, world spawn, `/tp`). Clamping
// the `snprintf` return to what the buffer actually holds makes the over-read
// structurally impossible whatever reaches it; the ingest validation in
// `parse_move_pos` is what keeps it from ever truncating (stage 7.16).
static std::string spawnLine(float x, float y, float z) {
    char sp[96];
    const int n = std::snprintf(sp, sizeof sp, "SPAWN:%.2f:%.2f:%.2f\n", x, y, z);
    return std::string(sp, n < 0 ? 0u : std::min(static_cast<size_t>(n), sizeof sp - 1));
}

// Block until this client's queue is empty, or `ms` elapses. Used where a line has
// to reach the peer before the socket goes away (the kick notice); everything else
// relies on the close path's drain.
static void flushOut(const std::shared_ptr<ClientOut>& o, int ms) {
    if (!o) return;
    std::unique_lock<std::mutex> lk(o->m);
    o->drained.wait_for(lk, std::chrono::milliseconds(ms),
                        [&] { return o->dead || (o->q.idle() && !o->writing); });
}

// The one clean-stop path: signal handler wake, idle-timeout self-stop, and the
// control socket's `stop` all funnel through this. Saves the world/positions/
// signs, gives every connected client's writer up to SV_CLOSE_DRAIN_SEC to empty
// its queue, then exits immediately — `_exit`, not `exit`, so a shutdown racing
// other live threads (writer threads, the matchmaker thread) never runs static
// destructors out from under them (7.9's "idle-timeout std::exit" finding).
static void shutdownAndExit() {
    serverRunning = false;
    saveWorld();
    savePlayerPos();
    saveSigns();
    for (const auto& o : outSnapshot(INVALID_SOCKET))
        flushOut(o, SV_CLOSE_DRAIN_SEC * 1000);
    std::cout << "[Server] Shutting down." << std::endl;
    std::cout.flush();
    _exit(0);
}

// Give this client's log lines their player name once JOIN has one.
static void nameOut(SOCKET s, const std::string& name) {
    auto o = outFor(s);
    if (!o) return;
    std::lock_guard<std::mutex> lk(o->m);
    o->name = name;
}

// --- the writer thread -------------------------------------------------------

// Write one whole buffer. The only place a player's socket is written.
//
// Retries EINTR **keeping its offset** — the sendAll() this replaces abandoned the
// rest of its buffer on any n <= 0, including EINTR, which was a third mid-line
// truncation source independent of any concurrency (7.3 finding 9). SO_SNDTIMEO
// turns a peer that has stopped reading into an EAGAIN we can time out on rather
// than a thread parked forever; only a real error or --client-write-timeout
// seconds with no progress at all ends the connection.
static bool writeAll(ClientOut& o, const std::string& buf, double& lastProgress, bool leaving) {
    size_t off = 0;
    while (off < buf.size()) {
        const ssize_t n = send(o.fd, buf.data() + off, buf.size() - off, 0);
        if (n > 0) { off += (size_t)n; lastProgress = monoSeconds(); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!serverRunning) return false;
            // Re-read the flag rather than trusting the one this write started
            // with: the reader may have asked us to stop while this very send()
            // was blocked, and it is waiting on our join() to close the fd. A
            // departing client must not hold its own thread for a full write
            // timeout because the send it happened to be inside began earlier.
            if (!leaving) {
                std::lock_guard<std::mutex> lk(o.m);
                leaving = o.closing || o.tooSlow;
            }
            const double budget = leaving ? (double)SV_CLOSE_DRAIN_SEC
                                          : (g_writeTimeout > 0 ? (double)g_writeTimeout : 0.0);
            if (budget > 0.0 && monoSeconds() - lastProgress > budget) return false;
            continue;   // just behind, not gone
        }
        return false;   // EPIPE / ECONNRESET / shutdown() under us
    }
    return true;
}

static void clientWriter(std::shared_ptr<ClientOut> o) {
    ewb::OutItem item;
    double lastProgress = monoSeconds();
    bool   sawClosing   = false;

    // Per-region-job accumulators. `lo` is FIFO and a job's frames are contiguous,
    // so exactly one job is ever in flight here.
    size_t jobFrames = 0, jobRecords = 0, jobBytes = 0;
    long   jobEncodeUs = 0;
    std::chrono::steady_clock::time_point jobStart = std::chrono::steady_clock::now();

    for (;;) {
        bool leaving;   // the reader asked us to stop, or this client is too far behind
        {
            std::unique_lock<std::mutex> lk(o->m);
            o->cv.wait(lk, [&] { return o->closing || o->tooSlow || o->dead || !o->q.idle(); });
            if (o->dead) break;
            leaving = o->closing || o->tooSlow;
            // A client on its way out has no use for terrain, and dropping its
            // pending regions here releases the record vectors (and their
            // pending-record accounting) immediately instead of at the drain
            // deadline. Queued hi lines survive, so a denial, a kick notice or the
            // "Connection too slow" line still goes out first.
            if (leaving) o->q.drop_lo();
            if (!o->q.take(item)) {
                o->writing = false;
                o->drained.notify_all();
                if (leaving) break;
                continue;
            }
            o->writing = true;
        }
        if (leaving && !sawClosing) { sawClosing = true; lastProgress = monoSeconds(); }

        const auto t0 = std::chrono::steady_clock::now();
        std::string bytes;
        // `buf` is what actually gets written: `bytes` for Frame/Bytes items, or
        // the shared blob directly for SharedBytes — stage 7.13, so a broadcast
        // blob shared across clients (`broadcastWorld`) is never copied at all,
        // not even here on the drain side.
        const std::string* buf = &bytes;
        if (item.kind == ewb::OutItem::Kind::Frame) {
            if (item.first) {
                jobFrames = jobRecords = jobBytes = 0;
                jobEncodeUs = 0;
                jobStart = t0;
            }
            try {
                bytes = ewb::encode_item(item);
            } catch (const std::exception& e) {
                // A deflate failure must not take the server down: this thread is
                // joined, not caught, and an escaping exception is std::terminate.
                // Abandoning the rest of the burst is safe now — the client sees a
                // short reply and asks again; it can no longer corrupt one.
                std::string who;
                { std::lock_guard<std::mutex> lk(o->m); who = o->name; o->writing = false; }
                std::cerr << "[Server] REGION encode failed for " << who << ": " << e.what()
                          << std::endl;
                item.reset();
                o->drained.notify_all();
                continue;
            }
        } else if (item.kind == ewb::OutItem::Kind::SharedBytes) {
            buf = item.shared_bytes.get();
            if (!buf) buf = &bytes;   // defensive; push_world never queues a null/empty shared blob
        } else {
            bytes = std::move(item.bytes);
        }
        const auto t1 = std::chrono::steady_clock::now();

        const bool ok = writeAll(*o, *buf, lastProgress, leaving);

        if (item.kind == ewb::OutItem::Kind::Frame) {
            ++jobFrames;
            jobRecords  += item.count;
            jobBytes    += bytes.size();
            jobEncodeUs += (long)std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
            g_rgnBytesOut.fetch_add(bytes.size(), std::memory_order_relaxed);
            if (ok && item.last) {
                // The measurement 7.3 asked for: `encode N ms` used to span encode
                // *and* send, because the two were interleaved in one loop, so an
                // operator could not see backpressure as backpressure. Now they are
                // separate numbers produced by separate threads.
                const long drainMs = (long)std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - jobStart).count();
                std::string who;
                { std::lock_guard<std::mutex> lk(o->m); who = o->name; }
                std::ostringstream drainLine;   // one flush instead of several (stage 7.12)
                drainLine << "[Server] REGION drain " << who << ": " << jobFrames << " frame(s), "
                          << jobRecords << " records, " << jobBytes << " B wire, encode "
                          << (jobEncodeUs / 1000) << " ms, drain " << drainMs << " ms";
                std::cout << drainLine.str() << std::endl;
            }
        }

        {
            std::lock_guard<std::mutex> lk(o->m);
            o->writing = false;
            o->sentBytes += buf->size();
            if (!ok) o->dead = true;
        }
        o->drained.notify_all();
        item.reset();
        if (!ok) break;
    }

    // Drop anything still queued so the record vectors (and the global pending
    // count they hold) are released the moment this writer stops, not whenever the
    // last producer lets go of its shared_ptr.
    bool gaveUp = false, stalled = false;
    std::string who;
    {
        std::lock_guard<std::mutex> lk(o->m);
        o->q.drop_lo();
        gaveUp  = (o->dead || o->tooSlow) && !o->closing;
        // dropTooSlow() has already said why, with the numbers; only a write that
        // failed or timed out still owes an explanation.
        stalled = o->dead && !o->tooSlow;
        who     = o->name;
    }
    o->drained.notify_all();
    if (gaveUp) {
        // The peer is gone, has not drained a byte in --client-write-timeout
        // seconds, or fell too far behind on world state. Its reader may still be
        // happily receiving (a client that stops reading but keeps sending would
        // otherwise sit here until the idle ceiling), so wake it the same way
        // ctlKick does and let it run the normal cleanup path.
        if (stalled && serverRunning)
            std::cerr << "[Server] " << who << ": output stalled; closing the connection."
                      << std::endl;
        shutdown(o->fd, SHUT_RDWR);
    }
    // ⚠️ The fd is NOT closed here. The reader thread owns its lifetime and closes
    // it after joining this one, so no producer can ever write a recycled fd.
}

// Start a client's writer and register it. Called from the accept loop, before the
// reader thread exists, so every line the reader can produce has somewhere to go.
static std::shared_ptr<ClientOut> openOut(SOCKET fd, int id) {
    auto o  = std::make_shared<ClientOut>();
    o->fd   = fd;
    o->id   = id;
    o->name = "client #" + std::to_string(id);
    o->lastRecv.store(monoSeconds(), std::memory_order_relaxed);
    o->q.set_limits(ewb::OutQueue::Limits{g_outboxMax, g_worldboxMax, g_regionQueue});
    // Without this a writer blocked on a peer that stopped reading is unkillable
    // short of shutdown(); with it, --client-write-timeout is enforceable.
    struct timeval tv{ SV_SEND_POLL_SEC, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    {
        std::lock_guard<std::mutex> lk(g_outsMtx);
        g_outs[fd] = o;
    }
    o->th = std::thread(clientWriter, o);
    return o;
}

// Retire a client's writer: unregister it (no producer can find it again), ask it
// to drain what is queued, and join. Called by that client's own reader thread and
// by nobody else, which is what makes the join safe.
static void closeOut(SOCKET fd) {
    std::shared_ptr<ClientOut> o;
    {
        std::lock_guard<std::mutex> lk(g_outsMtx);
        auto it = g_outs.find(fd);
        if (it == g_outs.end()) return;
        o = it->second;
        g_outs.erase(it);
    }
    {
        std::lock_guard<std::mutex> lk(o->m);
        o->closing = true;
    }
    o->cv.notify_all();
    if (o->th.joinable()) o->th.join();
}

// ⚠️ **Legacy, off by default** (stage 1.3, --legacy-snapshot). The real client has
// never been observed receiving this dump — it asks for `REGION` and is answered
// with `SNAPZ` — and the `ACTION:server:0:...` wire shape here is only
// *community*-corroborated (the modded server's applyBlockChanges relies on it),
// never captured. Kept because it is the fallback if SNAPZ bring-up stalls, and
// because it is the only path that works against a client that never sends REGION.
void sendWorldSnapshot(SOCKET clientSocket) {
    // Build the whole snapshot into ONE buffer and send it in bulk, instead of a
    // send() syscall per cell. For a heavily-edited world that's the difference
    // between tens of thousands of tiny syscalls (slow) and a handful of big writes.
    std::string blob;
    size_t cells = 0;
    {
        std::lock_guard<std::mutex> lock(g_worldMtx);
        cells = g_world.size();
        blob.reserve(cells * 48);   // ~one to three lines per cell
        char line[96];
        g_world.for_each([&](int x, int y, int z, unsigned char type, unsigned char color) {
            const Cell c{type, color};
            // "server" sender so the client never mistakes these for its own echoes.
            if (c.type == SV_AIR) {
                blob.append(line, snprintf(line, sizeof(line), "ACTION:server:0:%d:%d:%d:1\n", x,y,z));                 // mine
            } else if (c.type == SV_PAINTED_BASE) {
                // A *base* block that was only painted: paint alone. Mining first
                // would delete the natural block we are trying to recolour.
                blob.append(line, snprintf(line, sizeof(line), "ACTION:server:0:%d:%d:%d:3:%d\n", x,y,z,(int)c.color)); // paint base
            } else {
                // mine -> build -> paint. `mode 0` alone does **not** overwrite an
                // occupied cell (plan §0.5.3, community-attested), so a server-pushed
                // edit must always clear first or it silently no-ops on any client
                // whose local terrain already has something there.
                blob.append(line, snprintf(line, sizeof(line), "ACTION:server:0:%d:%d:%d:1\n", x,y,z));                 // mine
                blob.append(line, snprintf(line, sizeof(line), "ACTION:server:0:%d:%d:%d:0:%d\n", x,y,z,(int)c.type));  // build
                if (c.color != 0 && c.color <= ewb::CELL_MAX_PAINT)
                    blob.append(line, snprintf(line, sizeof(line), "ACTION:server:0:%d:%d:%d:3:%d\n", x,y,z,(int)c.color)); // then paint
            }
        });
    }
    // pushWorld, not pushReply: this is an unsolicited push of world state that the
    // client has no way to ask for again, and it is the whole world in one blob —
    // routinely larger than --client-world-max. The world-state budget bounds the
    // *backlog*, so a single oversized update at join time is still delivered.
    const size_t bytes = blob.size();
    pushWorld(outFor(clientSocket), std::move(blob));
    std::cout << "[Server] Queued legacy world snapshot (" << cells << " cells, " << bytes
              << " bytes) for a new player." << std::endl;
}

// --- REGION -> SNAPZ ----------------------------------------------------------
// Per-connection pacing state for the two request/response bursts (REGION, SIGNQ).
// Lives as a local in handleClient, so it is touched by exactly one thread and
// needs no lock. Only a *served* request restarts the gap, so a spammer gets
// exactly one reply per gap and never builds a queue.
struct BurstLimiter {
    std::chrono::steady_clock::time_point last{};
    bool haveLast   = false;   // no request served yet (a zero time_point is a real instant)
    int  served     = 0;
    bool capWarned  = false;   // log the session cap once, not once per spam line

    /// Returns true (and charges the request) when it may be served now.
    bool take(std::chrono::steady_clock::time_point now, long minGapMs, int sessionCap,
              const char* what, const std::string& who) {
        if (haveLast) {
            const long gap = (long)std::chrono::duration_cast<std::chrono::milliseconds>(
                                 now - last).count();
            if (gap < minGapMs) {
                if (g_verbose) std::cout << "[Server] throttled " << what << " from " << who
                                         << " (" << gap << " ms < " << minGapMs << ")" << std::endl;
                return false;   // drop, don't queue — queueing hands the amplification back
            }
        }
        if (served >= sessionCap) {
            if (!capWarned) {
                std::cerr << "[Server] " << who << " hit the " << what << " session cap ("
                          << sessionCap << "); ignoring further requests." << std::endl;
                capWarned = true;
            }
            return false;
        }
        last = now;
        haveLast = true;
        ++served;
        return true;
    }
};

// Answer one `SIGNQ` with the pre-formatted burst. No terminator — the capture
// shows a run of `SIGNP` lines and then nothing (see sign_store.h). A world with no
// signs is answered with silence, which is what "nothing" means here.
static void serveSigns(SOCKET clientSocket, const std::string& who, BurstLimiter& lim) {
    if (!lim.take(std::chrono::steady_clock::now(), SV_SIGNQ_MIN_GAP_MS,
                  SV_MAX_SIGNQ_PER_SESSION, "SIGNQ", who))
        return;

    std::shared_ptr<const std::string> blob;
    size_t count;
    {
        std::lock_guard<std::mutex> lk(g_signMtx);
        refreshSignBurstLocked();   // an edit may have just taken signs off a block
        blob  = g_signBlob;      // a refcount bump; the send happens with no lock held
        count = g_signs.size();
    }
    if (!blob || blob->empty()) {
        if (g_verbose) std::cout << "[Server] SIGNQ from " << who << ": no signs." << std::endl;
        return;
    }
    const size_t blobBytes = blob->size();
    if (!pushReply(outFor(clientSocket), blob)) {
        // Backpressure, not an error: this client is still working through an
        // earlier burst. Refusing is what the REGION path does too — they re-ask.
        if (g_verbose) std::cout << "[Server] SIGNQ from " << who
                                 << ": refused (output queue full)." << std::endl;
        return;
    }
    if (g_verbose) std::cout << "[Server] SIGNQ from " << who << ": queued " << count
                             << " signs (" << blobBytes << " B)." << std::endl;
}

// Answer one `REGION:<x>:<z>` with a burst of `SNAPZ` frames.
//
// ⚠️ **The world lock is held for the scan only.** Matching records are copied into
// a local vector under `g_worldMtx`; sorting, deflate, base64 and send() all happen
// after it is released. The real server appears to hold its world locked for the
// full ~1 s a region takes — that is precisely the behaviour a reference test
// client's `net/region.rs` etiquette rules exist to avoid triggering, and reproducing it
// would make every other player's edits queue behind one player's walk.
//
// ⚠️ **The scan is chunk-indexed** (stage 7.6). It used to be a filtered pass over
// the whole cell map, which measured ~80 ms of lock hold per region on a
// 13.9M-cell world — every other player's ACTION queued behind it, 256 times a
// session per client. `world_store.h` keeps the model as 16^3 chunks, so this now
// touches only the chunks the box covers. `region-stats` reports the lock hold;
// if it ever climbs again, the next lever is emitting records per chunk instead of
// into one vector.
//
// ⚠️ **This function no longer sends anything** (stage 7.3). It scans, sorts, and
// hands the record vector to the client's writer thread, which encodes one frame
// per turn. Three things fall out: frames can no longer be spliced by another
// thread's write; the per-region memory stays the record vector instead of gaining
// a queue of encoded base64 on top of it (up to ~17 MB for a worst-case box); and
// deflate moves off this thread, so a slow reader no longer blocks the client's own
// recv loop — which is what kept a departing player's name taken for a minute (7.5).
// A record vector that reports its own size to the global pending-record counter
// for exactly as long as it is alive — queued, being encoded, or held by a producer
// mid-hand-off. Making the accounting the vector's own deleter is what keeps it
// correct across every way a job can end: drained, dropped at close, or abandoned
// because the client went away with frames still queued.
static std::shared_ptr<const std::vector<ewb::SnapRec>>
makePendingRecs(std::vector<ewb::SnapRec>&& v) {
    auto* p = new std::vector<ewb::SnapRec>(std::move(v));
    g_rgnPendingRecs.fetch_add(p->size(), std::memory_order_relaxed);
    return std::shared_ptr<const std::vector<ewb::SnapRec>>(
        p, [](const std::vector<ewb::SnapRec>* q) {
            g_rgnPendingRecs.fetch_sub(q->size(), std::memory_order_relaxed);
            delete q;
        });
}

static void serveRegion(SOCKET clientSocket, const std::string& who, int cx, int cz,
                        BurstLimiter& lim) {
    using clock = std::chrono::steady_clock;

    // Validate the point the same way ACTION validates coordinates: an out-of-range
    // point produces a box that wraps the 24-bit key space.
    if (!ewb::region_point_valid(cx, cz)) {
        if (g_verbose) std::cout << "[Server] rejected out-of-range REGION (" << cx << ","
                                 << cz << ") from " << who << std::endl;
        return;
    }

    if (!lim.take(clock::now(), SV_REGION_MIN_GAP_MS, SV_MAX_REGIONS_PER_SESSION, "REGION", who))
        return;

    const ewb::RegionBox box = ewb::region_box(cx, cz, g_regionRadius);

    std::vector<ewb::SnapRec> recs;
    size_t scanned = 0, inBox = 0, worldCells = 0;
    ewb::WorldStore::BoxScan scan;
    clock::time_point tLock0;
    const auto t0 = clock::now();
    {
        std::lock_guard<std::mutex> lock(g_worldMtx);
        tLock0 = clock::now();   // stage 7.6: hold, not wait-plus-hold
        worldCells = g_world.size();
        // Stage 7.6: only the chunks whose x/z footprint meets the box. This used
        // to walk every cell in the world and filter — on a 13.9M-cell world that
        // was a 13.9M-iteration scan under the lock every ACTION needs, per
        // region, up to SV_MAX_REGIONS_PER_SESSION times a session.
        scan = g_world.for_each_in_box(box.x0, box.x1, box.z0, box.z1,
                                       [&](int x, int y, int z, unsigned char t, unsigned char c) {
            ewb::emit_cell_records(x, y, z, t, c, recs);
        });
        scanned = scan.cells_visited;
        inBox   = scan.cells_emitted;
    }   // <-- lock released here; nothing below re-takes it.
    const auto t1 = clock::now();
    {   // stage 7.6 step 1: how long every other player's ACTION waited on us.
        const uint64_t lockUs =
            (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(t1 - tLock0).count();
        g_rgnLockMicros.fetch_add(lockUs, std::memory_order_relaxed);
        bumpMax(g_rgnLockMaxMicros, lockUs);
    }

    if (g_regionSort) ewb::sort_records(recs);
    const auto t2 = clock::now();

    const size_t records = recs.size();
    const size_t frames  = records ? ewb::snapz_frame_count(records) : (g_regionEmptyFrame ? 1 : 0);

    ewb::RegionJob job;
    job.recs = makePendingRecs(std::move(recs));   // counts itself as pending from here
    // An unbuilt region has no records. We answer with an explicit `SNAPZ:0:`
    // (a well-formed frame that decodes to zero records) — 1.8 rung 2/3 showed
    // a reference test client needs a real frame back: it treats one as "answered" (resetting
    // its ABORT_AFTER_EMPTY counter) where silence leaves it stuck on "waiting
    // for the world snapshot" and aborts a ring sweep after 3 empty points.
    // --no-region-empty-frame restores pre-1.8 silence to A/B test the real
    // client in 1.9 (the real server has never been *observed* answering one).
    job.empty_frame = g_regionEmptyFrame;

    // Global memory guard, on top of the per-client queue depth: one worst-case box
    // is ~3.5 M records (~68 MB), so a handful of clients walking new terrain at
    // once is the shape that runs a small VPS out of RAM. Refusing is honest
    // backpressure — the client re-asks — where dropping frames would be the strips
    // bug wearing a different hat.
    const uint64_t pending = g_rgnPendingRecs.load(std::memory_order_relaxed);
    const bool overGlobal  = g_regionPending && pending > g_regionPending;
    if (overGlobal || !pushRegion(outFor(clientSocket), std::move(job))) {
        g_rgnRefused.fetch_add(1, std::memory_order_relaxed);
        std::ostringstream refusedLine;   // one flush instead of several (stage 7.12)
        refusedLine << "[Server] REGION #" << lim.served << " " << who << " refused: "
                    << (overGlobal ? "server region backlog" : "this client's region queue")
                    << " is full (" << records << " records, " << pending
                    << " pending server-wide). They will re-ask.";
        std::cerr << refusedLine.str() << std::endl;
        return;
    }

    // The measurement the plan asks for: requests served, cells scanned, ms/region.
    // Not gated on --verbose — one line per region is not chatty, and the
    // scan-vs-encode split is what decides whether a spatial index is worth
    // building. ⚠️ Bytes out, encode time and drain time are the writer thread's to
    // report (the `REGION drain` line), because nothing has been encoded yet.
    const long scanMs = (long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    const long sortMs = (long)std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();

    // Feed the `region-stats` control command (stage 3.2).
    g_rgnRequests.fetch_add(1, std::memory_order_relaxed);
    g_rgnCellsScanned.fetch_add(scanned, std::memory_order_relaxed);
    g_rgnRecords.fetch_add(records, std::memory_order_relaxed);
    g_rgnMicros.fetch_add(
        (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(t2 - t0).count(),
        std::memory_order_relaxed);

    // `unitbuf` (main(), for live logs under journald) flushes on every `<<`, so
    // composing into one string first turns this from 30 write() syscalls into
    // one (stage 7.12) with identical output and identical liveness.
    std::ostringstream regionLine;
    regionLine << "[Server] REGION #" << lim.served << " " << who << " (" << cx << "," << cz
               << ") box x[" << box.x0 << ".." << box.x1 << "] z[" << box.z0 << ".." << box.z1
               << "]: " << inBox << " cells from " << scan.chunks_visited << "/"
               << scan.chunks_total << " chunks (" << scanned << " slots, world "
               << worldCells << " cells) -> " << records << " records, "
               << frames << " frame(s) queued, scan " << scanMs << " ms (lock held "
               << (std::chrono::duration_cast<std::chrono::microseconds>(t1 - tLock0).count() / 1000)
               << " ms), sort " << sortMs << " ms";
    std::cout << regionLine.str() << std::endl;
}

// Relay a latency-sensitive line — movement, chat, a [Server] notice — to every
// client but the sender.
//
// ⚠️ This used to hold clientsMutex across a blocking send() to each peer, and
// JOIN needs the same mutex, so one backed-up client froze every join, chat line,
// movement relay and block edit on the server (stage 7.4). It now snapshots owning
// pointers, releases the lock, and only then enqueues: no syscall runs under any
// shared lock, and a client departing mid-broadcast stays alive for the enqueue.
void broadcastMessage(const std::string& message, SOCKET senderSocket) {
    if (message.empty()) return;
    const auto targets = outSnapshot(senderSocket);
    for (const auto& o : targets) pushHi(o, message);
}

// Relay **world state** — an ACTION relay, a WorldEdit burst, a SIGNP write — to
// every client but the sender. Same snapshot-then-enqueue shape; the difference is
// which queue it lands in. World state shares one ordered stream per client so an
// edit can never overtake the bulk reply it belongs after, and a client too far
// behind to hold it is disconnected rather than quietly diverged (out_queue.h).
void broadcastWorld(std::string blob, SOCKET senderSocket) {
    if (blob.empty()) return;
    const auto targets = outSnapshot(senderSocket);
    if (targets.empty()) return;
    // Stage 7.13: one shared blob, then every target's enqueue is a refcount bump
    // instead of a copy of the whole thing — this used to copy `blob` once per client
    // (megabytes x N for a `//set` burst). Stage 7.30: taken by value and moved in, so
    // a caller's local is not copied even once.
    const auto shared = std::make_shared<const std::string>(std::move(blob));
    for (const auto& o : targets) pushWorld(o, shared);
}

void removeClient(SOCKET clientSocket) {
    std::lock_guard<std::mutex> lock(clientsMutex);
    clients.erase(std::remove(clients.begin(), clients.end(), clientSocket), clients.end());
    playerInfoMap.erase(clientSocket);
}

// Parse message with format "PREFIX:data1:data2:..."
//
// Stage 7.13: this used to build a std::stringstream and getline() out of it,
// which every inbound line pays for (POSVEL, by far the highest-frequency verb,
// most of all). std::getline(ss, part, ':') has one edge case a naive substr
// split has to reproduce deliberately, not by accident: it does NOT emit a
// trailing empty field after a trailing delimiter (getline hits EOF with nothing
// left to extract and just stops), but it DOES emit empty fields everywhere else
// (leading colon, consecutive colons). Hence the loop below stops as soon as
// `start` reaches the end of the string instead of always pushing one more
// field. Equivalence against the old stringstream implementation is checked in
// protocol_test.cpp ("parseMessage: hand-rolled split matches std::getline").
std::vector<std::string> parseMessage(const std::string& message) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start < message.size()) {
        const size_t colon = message.find(':', start);
        if (colon == std::string::npos) {
            parts.push_back(message.substr(start));
            break;
        }
        parts.push_back(message.substr(start, colon - start));
        start = colon + 1;
    }
    return parts;
}

// --- Tier 1 operator control socket (stage 3.2) -----------------------------
// A `0600` unix domain socket at <worlddir>/edenserver.sock. Filesystem
// permissions are the authentication (plan §3.1). Line grammar is `verb[:rest]`
// (see control.h); admin block edits go through the same world model + broadcast
// path as player edits, so peers don't silently diverge until their next REGION.

// Disconnect a connected player by exact username. Returns their reported IP, or
// "" if nobody by that name is connected.
static std::string ctlKick(const std::string& name, const std::string& reason) {
    SOCKET target = INVALID_SOCKET;
    std::string ip;
    {
        std::lock_guard<std::mutex> lock(clientsMutex);
        for (const auto& kv : playerInfoMap)
            if (kv.second.username == name) { target = kv.first; ip = kv.second.ip; break; }
    }
    if (target == INVALID_SOCKET) return "";
    // Queue the notice, then give the writer a moment to actually put it on the
    // wire before the socket goes away. The blocking send() this replaces got that
    // ordering for free; with a writer thread it has to be asked for.
    auto out = outFor(target);
    pushHi(out, "[Server] You were " + reason + ".\n");
    flushOut(out, 250);
    // shutdown() (not close()) unblocks the client thread's recv(); it then runs
    // its own cleanup (savePlayerPos / removeClient / closeOut / close).
    shutdown(target, SHUT_RDWR);
    return ip.empty() ? std::string("?") : ip;
}

// One `x:y:z:type[:color]` edit relayed to every client with the reserved
// `server` sender. Shared by Tier 1 (setblock/fill) and Tier 2 (every WorldEdit
// command), so there is one relay shape to get right; the shape itself — mine ->
// build -> paint, or a lone paint for a painted-base cell — is
// `ewb::we_emit_edit_wire`, unit-tested in worldedit_test. The caller writes the
// model; this only builds the wire.
static void emitEditWire(std::string& wire, int x, int y, int z, int type, int color) {
    ewb::we_emit_edit_wire(wire, x, y, z, type, color);
}

// The whole relay for an applied batch. **Call this after releasing g_worldMtx.**
//
// Stage 3.4 measured the capped scan both tiers do (131072 cells, world at its
// 4,000,000-cell ceiling, everything changed — the worst case the caps permit):
//
//     lookup + world write + batch record : ~13 ms   <- needs the lock
//     building the 8.5 MB ACTION relay    : ~24 ms   <- needs nothing
//     read-only scan (`//copy`)           :  ~2 ms
//
// The formatting was two thirds of a 51 ms lock hold and none of it touches the
// world, so the batch is decided under the lock and the wire is built from it
// out here. That is the plan's "snapshot under the lock, work outside it" rule
// at its cheapest: worst-case lock hold drops ~4x for a few lines, no new
// buffering, and the caps did not have to move. (Measurement lives in
// WORKING/ROADMAP-SERVER.md §3.4; re-run it before raising any of the caps.)
static std::string emitEditBatch(const std::vector<ewb::WeEdit>& batch) {
    std::string wire;
    wire.reserve(batch.size() * 64);
    for (const ewb::WeEdit& e : batch)
        emitEditWire(wire, e.x, e.y, e.z, e.newType, e.newColor);
    return wire;
}

// Fill an inclusive box. The volume is capped and validated by the caller; here
// we take the world lock once for the whole box (the plan §3.4 rule: never
// iterate a box under g_worldMtx unbounded — the cap is what bounds it), build
// the wire burst, release the lock, then broadcast once. Only cells `worldSet`
// accepted are relayed (stage 7.26): a cell the world cap refused is counted in
// `refused` and never reaches a player's screen, exactly as weCommit does.
static ewb::CtlFillResult ctlFillBox(int x0, int y0, int z0, int x1, int y1, int z1, int type, int color) {
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    if (z0 > z1) std::swap(z0, z1);
    ewb::CtlFillResult res;
    std::vector<ewb::WeEdit> batch;
    batch.reserve((size_t)ewb::ctl_fill_volume(x0, y0, z0, x1, y1, z1));
    {
        std::lock_guard<std::mutex> lock(g_worldMtx);
        for (int x = x0; x <= x1; ++x)
            for (int z = z0; z <= z1; ++z)
                for (int y = y0; y <= y1; ++y) {
                    if (!worldSet(x, y, z, type == SV_AIR ? SV_AIR : type, color)) { ++res.refused; continue; }
                    batch.push_back({x, y, z, 0, 0, (unsigned char)type, (unsigned char)color});
                }
    }
    std::string wire = emitEditBatch(batch);   // formatting, outside the lock
    if (!wire.empty()) broadcastWorld(std::move(wire), INVALID_SOCKET);
    res.applied = (long long)batch.size();
    return res;
}

// " (refused N at the world cell cap)" for an audit line, or "" when nothing was.
static std::string ctlRefusedNote(const ewb::CtlFillResult& r) {
    return r.refused ? " (refused " + std::to_string(r.refused) + " at the world cell cap)" : "";
}

// --- Protected zones: the visible undo (stage 8.2) ---------------------------
//
// A refused player edit has usually already happened on that player's screen — the
// client draws a mine, a build or a blast before it tells the server — so refusing it
// in the model is only half the job. The other half is a **restore**: the
// `ACTION:server:0:…` lines that redraw each refused cell as the model holds it
// (`ewb::zone_restore_wire`), then the `SIGNP` of any sign on a restored block,
// since a client hides a sign while its block is air.
//
// The restore is built when it is *sent*, from the model as it is then, not when the
// edit was refused: a control-socket `setblock` that lands in between is what the
// player should see, and a restore built earlier would paint over it.
//
// It is sent --zone-revert-delay-ms after the refusal, by one thread draining a
// RevertQueue (zone_guard.h), coalesced per (client, cell). With a delay of 0 there is
// no queue: the restore goes straight onto the refusing thread's output. Either way it
// is world state — the ordered stream — so it lands after anything the same thread
// queued before it, which for a burn is the relay that makes every peer run the blast.

// RevertQueue target for "every connected client" (a burn restore). Per-client
// targets are ClientOut::id + 1, so they never collide with it.
static constexpr uint64_t ZONE_TARGET_ALL = 0;

struct RevertTo { std::weak_ptr<ClientOut> out; };   // expired = that client left
static std::mutex                          g_revertMtx;   // guards g_revertQ; takes no other lock
static std::condition_variable             g_revertCv;
static ewb::RevertQueue<RevertTo>          g_revertQ;
static std::atomic<uint64_t>               g_revertDropped{0};

// The restore for `cells`, built from the model now. Cell values are read under
// g_worldMtx and formatted after it (the "decide under the lock, format outside it"
// rule every relay here follows); signs are gathered under g_signMtx afterwards.
static std::string buildRestore(const std::vector<ewb::RevertCell>& cells) {
    struct Now { bool present; Cell c; };
    std::vector<Now> now(cells.size());
    {
        std::lock_guard<std::mutex> lk(g_worldMtx);
        for (size_t i = 0; i < cells.size(); ++i)
            now[i].present = worldGet(cells[i].x, cells[i].y, cells[i].z, now[i].c);
    }
    std::string wire;
    wire.reserve(cells.size() * 72);
    for (size_t i = 0; i < cells.size(); ++i)
        ewb::zone_restore_wire(wire, cells[i].x, cells[i].y, cells[i].z, now[i].present,
                               now[i].c.type, now[i].c.color);
    if (g_signBlockCount.load(std::memory_order_relaxed) == 0) return wire;
    std::lock_guard<std::mutex> lk(g_signMtx);
    std::unordered_set<uint64_t> signed_;
    for (const ewb::RevertCell& c : cells) {
        const uint64_t k = wkey(c.x, c.y, c.z);
        if (g_signBlocks.count(k)) signed_.insert(k);
    }
    if (signed_.empty()) return wire;
    for (const ewb::Sign& s : g_signs)
        if (signed_.count(wkey(s.x, s.y, s.z))) wire += ewb::format_signp(s);
    return wire;
}

static void sendRestore(uint64_t target, const RevertTo& to, const std::vector<ewb::RevertCell>& cells) {
    std::string wire = buildRestore(cells);
    if (wire.empty()) return;
    if (target == ZONE_TARGET_ALL) broadcastWorld(std::move(wire), INVALID_SOCKET);
    else if (auto o = to.out.lock()) pushWorld(o, std::move(wire));
}

// Restore `cells` on one client's screen (`to`), or on everyone's (`to` null).
static void scheduleRestore(const std::shared_ptr<ClientOut>& to, const std::vector<ewb::RevertCell>& cells) {
    if (cells.empty()) return;
    const uint64_t target = to ? (uint64_t)to->id + 1 : ZONE_TARGET_ALL;
    if (g_zoneRevertDelayMs <= 0) { sendRestore(target, RevertTo{to}, cells); return; }
    size_t dropped = 0;
    {
        std::lock_guard<std::mutex> lk(g_revertMtx);
        g_revertQ.push(monoSeconds() + g_zoneRevertDelayMs / 1000.0, target, RevertTo{to}, cells, &dropped);
    }
    g_revertCv.notify_one();
    if (dropped) {
        // A flood of refusals across many clients. The model is right either way;
        // what is lost is the redraw, which those players get back on rejoin.
        static std::mutex gateMtx;
        static ewb::TokenBucket gate(1.0, 1.0 / 60.0);
        const uint64_t total = g_revertDropped.fetch_add(dropped) + dropped;
        std::lock_guard<std::mutex> lk(gateMtx);
        if (gate.allow(monoSeconds()))
            std::cerr << "[Server] zone restore queue full (" << ewb::ZONE_REVERT_MAX_PENDING
                      << " cells); " << total << " restore(s) dropped so far. The world is intact;"
                         " affected players see it correctly on rejoin." << std::endl;
    }
}

// The one thread that sends delayed restores (started only when the delay is > 0).
static void revertThread() {
    std::unique_lock<std::mutex> lk(g_revertMtx);
    while (serverRunning) {
        if (g_revertQ.empty()) { g_revertCv.wait_for(lk, std::chrono::seconds(1)); continue; }
        const double now = monoSeconds(), due = g_revertQ.next_due();
        if (due > now) {
            g_revertCv.wait_for(lk, std::chrono::duration<double>(due - now));
            continue;
        }
        ewb::RevertQueue<RevertTo>::Job job;
        if (!g_revertQ.pop_due(now, job)) continue;
        lk.unlock();                                    // never hold it into g_worldMtx
        sendRestore(job.target, job.payload, job.cells);
        lk.lock();
    }
}

// Aggregated audit of refusals: one line per player per zone per 10 s (zone_guard.h).
static std::mutex         g_zoneAuditMtx;   // guards g_zoneAudit; takes no other lock
static ewb::ZoneAuditAgg  g_zoneAudit;

static void zoneAudit(const std::string& player, const std::string& zone, const std::string& verb,
                      int x, int y, int z) {
    std::string line;
    bool write;
    {
        std::lock_guard<std::mutex> lk(g_zoneAuditMtx);
        write = g_zoneAudit.note(monoSeconds(), player, zone, verb, x, y, z, line);
    }
    if (write) auditLog("player:" + player, line);
}

// Write the counts folded into closed windows. From the autosave tick.
static void flushZoneAudit() {
    std::vector<std::pair<std::string, std::string>> lines;
    {
        std::lock_guard<std::mutex> lk(g_zoneAuditMtx);
        lines = g_zoneAudit.flush(monoSeconds());
    }
    for (const auto& pl : lines) auditLog("player:" + pl.first, pl.second);
}

// A refused edit, from the refusing player's side: the chat notice (paced by the
// connection's own `notice` bucket — the capNotice shape) and the audit line.
static void zoneRefused(SOCKET s, ewb::TokenBucket& notice, const std::string& player,
                        const std::string& zone, const char* verb, int x, int y, int z) {
    if (g_verbose)
        std::cout << "[" << player << "] " << verb << " at (" << x << "," << y << "," << z
                  << ") refused: protected zone " << zone << std::endl;
    if (notice.allow(monoSeconds()))
        sendLine(s, "[Server] This area is protected ('" + zone + "').\n");
    zoneAudit(player, zone, verb, x, y, z);
}

// Handle one complete control line. `reply` is sent back (a trailing '\n' is
// added if missing); set `stopServer` to request shutdown.
static void handleControlLine(const std::string& line, std::string& reply, bool& stopServer) {
    std::string verb, rest;
    if (!ewb::ctl_split(line, verb, rest)) return;   // blank line

    const ewb::CtlSpec* spec = ewb::ctl_find(verb);
    if (!spec) { reply = "error: unknown command '" + verb + "' (try 'help')"; return; }

    // Field-count check (say / signs are free-form and skip it).
    if (spec->max_args >= 0 && verb != "say") {
        const auto f = ewb::ctl_fields(rest, spec->max_args + 1);
        const int n = rest.empty() ? 0 : (int)f.size();
        if (n < spec->min_args || (spec->max_args >= 0 && n > spec->max_args)) {
            reply = std::string("usage: ") + spec->usage;
            return;
        }
    }

    if (verb == "help") { reply = ewb::ctl_help_text(); return; }

    if (verb == "who") {
        // PIN holders first, before clientsMutex (g_authMtx is never taken inside it).
        std::set<std::string> pinNames;
        { std::lock_guard<std::mutex> ak(g_authMtx); for (const auto& n : g_auth.names()) pinNames.insert(n); }
        std::lock_guard<std::mutex> lock(clientsMutex);
        std::ostringstream ss;
        ss << playerInfoMap.size() << " player(s):\n";
        for (const auto& kv : playerInfoMap) {
            const PlayerInfo& p = kv.second;
            const bool pin = pinNames.count(p.username) > 0;
            int file; { std::lock_guard<std::mutex> ol(g_opsMtx); file = g_ops.level_of(p.username, g_defaultLevel); }
            // The level this session actually has (stage 8.6), not the file's.
            const int lvl = ewb::auth_effective_level(file, g_defaultLevel, pin, p.verified);
            ss << "  " << p.username << " (T" << p.characterType << ") "
               << p.ip << "  @ " << (int)p.posX << "," << (int)p.posY << "," << (int)p.posZ
               << "  level " << lvl;
            // Output backlog (stage 7.3). This is the number that says "this
            // player is on a weak link" before they are dropped for it, and the
            // one to look at when someone reports the world arriving late.
            // Lock order: clientsMutex -> g_outsMtx -> ClientOut::m, as declared.
            if (auto o = outFor(kv.first)) {
                std::lock_guard<std::mutex> ok(o->m);
                ss << "  queued " << o->q.queued_bytes() << " B (peak " << o->q.peak_bytes()
                   << ", " << o->q.region_jobs() << " region(s))";
                if (o->q.dropped_lines())
                    ss << " dropped " << o->q.dropped_lines() << " stale line(s)";
            }
            // Last on the row, so a parser anchored on "  level " / "queued " keeps
            // working. verified = logged in; unverified = has a PIN, not logged in
            // (held to --default-level); none = no PIN (level is by name alone).
            ss << "  auth " << (p.verified ? "verified" : pin ? "unverified" : "none");
            ss << "\n";
        }
        reply = ss.str();
        return;
    }

    if (verb == "say") {
        std::string text = ewb::sanitize_text(rest, SV_MAX_CHAT);
        if (text.empty()) { reply = "error: empty message"; return; }
        std::string m = "[Server] " + text + "\n";
        auditLog("control", "say: " + text);
        broadcastMessage(m, INVALID_SOCKET);
        reply = "ok";
        return;
    }

    if (verb == "kick") {
        const auto f = ewb::ctl_fields(rest, 2);
        const std::string name = f[0];
        const std::string reason = f.size() > 1 ? ewb::sanitize_text(f[1], 128) : "kicked by the operator";
        const std::string ip = ctlKick(name, reason);
        if (ip.empty()) { reply = "error: no player named '" + name + "'"; return; }
        auditLog("control", "kick " + name + " @ " + ip + " (" + reason + ")");
        reply = "ok: kicked " + name;
        return;
    }

    if (verb == "ban") {
        const std::string token = ewb::ctl_fields(rest, 1)[0];
        bool added; { std::lock_guard<std::mutex> lk(g_banMtx); added = g_bans.add(token); }
        saveBans();
        // Kick anyone matching now (by name, or by IP if a name-ban's holder is on).
        std::vector<std::string> kickNames;
        {
            std::lock_guard<std::mutex> lock(clientsMutex);
            for (const auto& kv : playerInfoMap)
                if (kv.second.username == token || kv.second.ip == token)
                    kickNames.push_back(kv.second.username);
        }
        for (const auto& n : kickNames) ctlKick(n, "banned");
        auditLog("control", "ban " + token + (added ? "" : " (already listed)") +
                            (kickNames.empty() ? "" : ", disconnected " + std::to_string(kickNames.size())));
        reply = std::string("ok: banned ") + token + (kickNames.empty() ? "" : " (disconnected " + std::to_string(kickNames.size()) + ")");
        return;
    }

    if (verb == "unban") {
        const std::string token = ewb::ctl_fields(rest, 1)[0];
        bool removed; { std::lock_guard<std::mutex> lk(g_banMtx); removed = g_bans.remove(token); }
        if (removed) saveBans();
        auditLog("control", "unban " + token + (removed ? "" : " (was not listed)"));
        reply = removed ? "ok: unbanned " + token : "error: '" + token + "' was not banned";
        return;
    }

    if (verb == "banlist") {
        std::lock_guard<std::mutex> lk(g_banMtx);
        if (g_bans.empty()) { reply = "ban list is empty"; return; }
        std::ostringstream ss;
        ss << (g_bans.names.size() + g_bans.ips.size()) << " ban entr(y/ies):\n";
        for (const auto& s : g_bans.ips)   ss << "  " << s << "  (ip)\n";
        for (const auto& s : g_bans.names) ss << "  " << s << "  (name)\n";
        reply = ss.str();
        return;
    }

    if (verb == "save") {
        saveWorld();
        savePlayerPos();
        saveSigns();
        auditLog("control", "save");
        reply = "ok: saved";
        return;
    }

    if (verb == "stop") {
        auditLog("control", "stop requested");
        reply = "ok: stopping";
        stopServer = true;
        return;
    }

    if (verb == "op") {
        const auto f = ewb::ctl_fields(rest, 2);
        const int lvl = std::atoi(f[1].c_str());
        if (!ewb::ctl_level_valid(lvl)) { reply = "error: level must be 0..2"; return; }
        { std::lock_guard<std::mutex> lk(g_opsMtx); g_ops.set(f[0], lvl); }
        saveOps();
        auditLog("control", "op " + f[0] + " -> level " + std::to_string(lvl));
        reply = "ok: " + f[0] + " is now level " + std::to_string(lvl);
        return;
    }

    if (verb == "deop") {
        const std::string name = ewb::ctl_fields(rest, 1)[0];
        bool had; { std::lock_guard<std::mutex> lk(g_opsMtx); had = g_ops.erase(name); }
        if (had) saveOps();
        auditLog("control", "deop " + name + (had ? "" : " (had no entry)"));
        reply = had ? "ok: cleared " + name : "error: '" + name + "' had no op entry";
        return;
    }

    if (verb == "setblock") {
        const auto f = ewb::ctl_fields(rest, 5);
        int x, y, z, type, color = 0;
        try {
            x = std::stoi(f[0]); y = std::stoi(f[1]); z = std::stoi(f[2]); type = std::stoi(f[3]);
            if (f.size() > 4) color = std::stoi(f[4]);
        } catch (...) { reply = "error: non-numeric argument"; return; }
        if (y < 0 || y >= SV_WORLD_HEIGHT || x < 0 || z < 0 || x > 0xFFFFFF || z > 0xFFFFFF) {
            reply = "error: coordinate out of range"; return;
        }
        if (!ewb::action_extra_valid(0, type) && type != SV_AIR) { reply = "error: block type out of range (0..127)"; return; }
        if (color != 0 && !ewb::action_extra_valid(3, color)) { reply = "error: color out of range (0..54)"; return; }
        const ewb::CtlFillResult res = ctlFillBox(x, y, z, x, y, z, type, color);
        auditLog("control", "setblock " + std::to_string(x) + "," + std::to_string(y) + "," +
                            std::to_string(z) + " = " + std::to_string(type) +
                            (color ? " color " + std::to_string(color) : "") + ctlRefusedNote(res));
        drainSignRemovals();   // after the edit's own audit line
        reply = ewb::ctl_setblock_reply(res);
        return;
    }

    if (verb == "fill") {
        const auto f = ewb::ctl_fields(rest, 8);
        int v[6], type, color = 0;
        try {
            for (int i = 0; i < 6; ++i) v[i] = std::stoi(f[i]);
            type = std::stoi(f[6]);
            if (f.size() > 7) color = std::stoi(f[7]);
        } catch (...) { reply = "error: non-numeric argument"; return; }
        for (int i : {1, 4})
            if (v[i] < 0 || v[i] >= SV_WORLD_HEIGHT) { reply = "error: y out of range (0.." + std::to_string(SV_WORLD_HEIGHT - 1) + ")"; return; }
        for (int i : {0, 2, 3, 5})
            if (v[i] < 0 || v[i] > 0xFFFFFF) { reply = "error: x/z out of range"; return; }
        if (!ewb::action_extra_valid(0, type) && type != SV_AIR) { reply = "error: block type out of range (0..127)"; return; }
        if (color != 0 && !ewb::action_extra_valid(3, color)) { reply = "error: color out of range (0..54)"; return; }
        const long long vol = ewb::ctl_fill_volume(v[0], v[1], v[2], v[3], v[4], v[5]);
        if (vol > g_ctlFillCap) {
            reply = "error: box is " + std::to_string(vol) + " cells (max " +
                    std::to_string(g_ctlFillCap) + ", " +
                    std::to_string(ewb::CTL_FILL_CAP_MULTIPLE) + "x --we-max-cells)";
            return;
        }
        const ewb::CtlFillResult res = ctlFillBox(v[0], v[1], v[2], v[3], v[4], v[5], type, color);
        auditLog("control", "fill " + std::to_string(res.applied) + " cells = " + std::to_string(type) +
                            (color ? " color " + std::to_string(color) : "") + " @ " +
                            std::to_string(v[0]) + "," + std::to_string(v[1]) + "," + std::to_string(v[2]) +
                            ".." + std::to_string(v[3]) + "," + std::to_string(v[4]) + "," + std::to_string(v[5]) +
                            ctlRefusedNote(res));
        drainSignRemovals();   // after the edit's own audit line
        reply = ewb::ctl_fill_reply(res);
        return;
    }

    if (verb == "zones") {
        auto zs = zonesSnapshot();
        if (!zs || zs->zones().empty()) { reply = "no protected zones"; return; }
        std::ostringstream ss;
        ss << zs->size() << " zone(s):\n";
        for (const ewb::Zone& z : zs->zones()) {
            const long long cells = ewb::ctl_fill_volume(z.x0, z.y0, z.z0, z.x1, z.y1, z.z1);
            ss << "  " << z.name << "  (" << z.x0 << "," << z.y0 << "," << z.z0 << ")..("
               << z.x1 << "," << z.y1 << "," << z.z1 << ")  " << (z.enforced ? "all" : "off")
               << "  level " << (z.hasLevel ? std::to_string(z.level) : std::string("-"))
               << "  " << cells << " cell(s)\n";
        }
        reply = ss.str();
        return;
    }

    // topmap (stage 8.5; grammar and reply in topmap.h). The world lock is taken
    // once per 16x16 chunk column the samples fall in and released between them,
    // so a full 256x256 map is thousands of microsecond-long holds, never one long
    // one — a player's REGION or ACTION waits for at most one chunk column.
    if (verb == "topmap") {
        ewb::TopmapReq req;
        std::string err;
        if (!ewb::topmap_parse(ewb::ctl_fields(rest, 0), req, err)) { reply = "error: " + err; return; }
        static const ewb::BaseProfile kBase = ewb::eden_default_profile();
        ewb::TopmapGrid grid(req, kBase);
        const auto t0 = std::chrono::steady_clock::now();
        ewb::topmap_for_each_chunk_column(req, [&](int, int, const std::vector<ewb::TopmapSample>& batch) {
            std::lock_guard<std::mutex> lk(g_worldMtx);
            for (const ewb::TopmapSample& t : batch)
                g_world.for_each_in_column(t.x, t.z, [&](int y, unsigned char type, unsigned char color) {
                    grid.feed(t.i, t.j, y, type, color);
                });
        });
        reply = grid.render();
        if (g_verbose)
            std::cout << "[Server] control topmap " << req.w << "x" << req.h << " step " << req.step << " in "
                      << std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0).count()
                      << " ms (" << reply.size() << " B)" << std::endl;
        return;
    }

    // passwd / unpasswd / pins (stage 8.6). The PIN is generated here, stored only as
    // a salted hash (eden_auth.txt, 0600) and appears in exactly one place: this
    // reply. It is never audited or logged. Any live session under the name is
    // logged out, so a re-issued PIN takes effect at once.
    if (verb == "passwd" || verb == "unpasswd") {
        const std::string name = ewb::ctl_fields(rest, 1)[0];
        if (ewb::validate_username(name) != ewb::NameVerdict::Ok) {
            reply = "error: '" + name + "' is not a valid player name"; return;
        }
        std::lock_guard<std::mutex> editLk(g_authEditMtx);
        ewb::AuthFile next;
        { std::lock_guard<std::mutex> ak(g_authMtx); next = g_auth; }
        const bool had = next.has(name);
        std::string pin;
        if (verb == "passwd") {
            ewb::AuthRecord rec;
            if (!ewb::auth_make_pin(randomBytes, pin) || !ewb::auth_make_record(pin, randomBytes, rec)) {
                reply = "error: no random source available; PIN not issued"; return;
            }
            next.set(name, rec);
        } else if (!next.erase(name)) {
            reply = "error: '" + name + "' has no PIN"; return;
        }
        std::string err;
        if (!saveAuth(next, err)) { reply = "error: save failed: " + err; return; }
        std::vector<SOCKET> loggedOut;
        {
            std::lock_guard<std::mutex> ak(g_authMtx);
            g_auth = std::move(next);
            std::lock_guard<std::mutex> lk(clientsMutex);
            for (auto& kv : playerInfoMap)
                if (kv.second.username == name && kv.second.verified) {
                    kv.second.verified = false;
                    loggedOut.push_back(kv.first);
                }
        }
        for (SOCKET ls : loggedOut)
            sendLine(ls, verb == "passwd"
                             ? "[Server] Your PIN was changed by the operator; /login again with the new one.\n"
                             : "[Server] Your PIN was removed by the operator.\n");
        if (verb == "passwd") {
            auditLog("control", "passwd " + name + (had ? " (replaced)" : " (issued)") +
                                    (loggedOut.empty() ? "" : ", logged out the live session"));
            reply = "ok: PIN for " + name + ": " + pin + "  (shown once; the player types /login " + pin + ")";
        } else {
            auditLog("control", "unpasswd " + name + (loggedOut.empty() ? "" : ", logged out the live session"));
            reply = "ok: removed the PIN for " + name + "; the name is claimed by name alone again";
        }
        return;
    }

    if (verb == "pins") {
        std::vector<std::string> names;
        { std::lock_guard<std::mutex> ak(g_authMtx); names = g_auth.names(); }
        if (names.empty()) { reply = "no names have a PIN"; return; }
        std::map<std::string, bool> online;   // name -> logged in
        {
            std::lock_guard<std::mutex> lk(clientsMutex);
            for (const auto& kv : playerInfoMap) online[kv.second.username] = kv.second.verified;
        }
        std::ostringstream ss;
        ss << names.size() << " name(s) with a PIN:\n";
        for (const std::string& n : names) {
            auto it = online.find(n);
            ss << "  " << n << "  " << (it == online.end() ? "offline" : it->second ? "online verified" : "online unverified") << "\n";
        }
        reply = ss.str();
        return;
    }

    // zone:add|set|flags|rm|reload — the stage 8.3 control verbs. Each mutating
    // subcommand reads a snapshot, edits a private copy, saves it to
    // g_zonesFile, then swaps the pointer under g_zonesMtx — g_zonesEditMtx
    // serializes these against each other (see its declaration). Effective on
    // the next edit, no restart. The control socket itself is never subject to
    // a zone (zoneDenies is never asked here) — it *is* the operator.
    if (verb == "zone") {
        const auto f = ewb::ctl_fields(rest, 0);
        const std::string sub = f.empty() ? "" : f[0];

        if (sub == "reload") {
            std::lock_guard<std::mutex> editLk(g_zonesEditMtx);
            std::string err;
            if (!loadZones(err)) { reply = "error: " + err; return; }
            const size_t n = zonesSnapshot()->size();
            auditLog("control", "zone reload (" + std::to_string(n) + " zone(s))");
            reply = "ok: reloaded " + std::to_string(n) + " zone(s) from " + g_zonesFile;
            return;
        }

        if (sub == "rm") {
            if (f.size() != 2) { reply = "usage: " + std::string(spec->usage); return; }
            const std::string name = f[1];
            std::lock_guard<std::mutex> editLk(g_zonesEditMtx);
            ewb::ZoneSet next = *zonesSnapshot();
            if (!next.remove(name)) { reply = "error: no zone named '" + name + "'"; return; }
            std::string saveErr;
            if (!zonePublish(std::move(next), saveErr)) { reply = "error: save failed: " + saveErr; return; }
            auditLog("control", "zone rm " + name);
            reply = "ok: removed zone '" + name + "'";
            return;
        }

        if (sub == "add" || sub == "set" || sub == "flags") {
            const size_t p = rest.find(':');
            if (p == std::string::npos) { reply = "usage: " + std::string(spec->usage); return; }
            const std::string tail = rest.substr(p + 1);
            const auto tf = ewb::ctl_fields(tail, 0);

            std::lock_guard<std::mutex> editLk(g_zonesEditMtx);
            ewb::ZoneSet next = *zonesSnapshot();
            std::string name, err, line;
            ewb::Zone z;

            if (sub == "add") {
                if (tf.size() < 7 || tf.size() > 9) { reply = "usage: " + std::string(spec->usage); return; }
                name = tf[0];
                line = tail;
                if (tf.size() == 7) line += ":all";   // flags default to enforced
                if (!ewb::zone_parse_line(line, z, &err)) { reply = "error: " + err; return; }
                if (!next.add(z, &err)) { reply = "error: " + err; return; }
            } else if (sub == "set") {
                if (tf.size() != 7) { reply = "usage: " + std::string(spec->usage); return; }
                name = tf[0];
                const ewb::Zone* existing = next.find(name);
                if (!existing) { reply = "error: no zone named '" + name + "'"; return; }
                line = tail;
                line += existing->enforced ? ":all" : ":off";
                if (existing->hasLevel) line += ":" + std::to_string(existing->level);
                next.remove(name);
                if (!ewb::zone_parse_line(line, z, &err)) { reply = "error: " + err; return; }
                if (!next.add(z, &err)) { reply = "error: " + err; return; }
            } else {   // flags
                if (tf.size() < 2 || tf.size() > 3) { reply = "usage: " + std::string(spec->usage); return; }
                name = tf[0];
                const ewb::Zone* existing = next.find(name);
                if (!existing) { reply = "error: no zone named '" + name + "'"; return; }
                line = name + ":" +
                    std::to_string(existing->x0) + ":" + std::to_string(existing->y0) + ":" + std::to_string(existing->z0) + ":" +
                    std::to_string(existing->x1) + ":" + std::to_string(existing->y1) + ":" + std::to_string(existing->z1) + ":" +
                    tf[1];
                if (tf.size() == 3) line += ":" + tf[2];
                else if (existing->hasLevel) line += ":" + std::to_string(existing->level);
                next.remove(name);
                if (!ewb::zone_parse_line(line, z, &err)) { reply = "error: " + err; return; }
                if (!next.add(z, &err)) { reply = "error: " + err; return; }
            }

            std::string saveErr;
            if (!zonePublish(std::move(next), saveErr)) { reply = "error: save failed: " + saveErr; return; }

            auditLog("control", "zone " + sub + " " + name);
            const std::string verbed = sub == "add" ? "added" : sub == "set" ? "resized" : "flags updated";
            std::string note;
            if (z.hasLevel) {
                note = " (bypassed by players logged in at level " + std::to_string(z.level) + "+";
                if (!nameHasAnyPin()) note += "; no name has a PIN yet, so nobody can: edenctl passwd <name>";
                note += ")";
            }
            reply = "ok: zone '" + name + "' " + verbed + note;
            return;
        }

        reply = "usage: " + std::string(spec->usage);
        return;
    }

    if (verb == "signs") {
        const auto f = ewb::ctl_fields(rest, 0);
        const std::string sub = f.empty() ? "" : f[0];
        if (sub == "reload") {
            // A player's sign change lives only in memory until the next save;
            // reloading over it would throw it away without a word.
            if (g_signsDirty) {
                reply = "error: players have placed or removed signs that are not saved yet. Run 'save' first"
                        " (it rewrites the sign file, so hand-edit it only while the server is stopped)";
                return;
            }
            loadSigns();
            const size_t dropped = pruneOrphanSigns();
            size_t n;
            { std::lock_guard<std::mutex> lk(g_signMtx); n = g_signs.size(); }
            const std::string note =
                dropped ? "dropped " + std::to_string(dropped) + " on removed blocks" : "";
            auditLog("control", "signs reload (" + std::to_string(n) + (dropped ? ", " + note : "") + ")");
            reply = "ok: reloaded " + std::to_string(n) + " signs" + (dropped ? " (" + note + ")" : "");
            return;
        }
        if (sub == "add") {
            // rest is "add:<x>:<y>:<z>:<a>:<b>:<c>:<text>"; hand the tail to the
            // same parser the sidecar loader uses.
            const size_t p = rest.find(':');
            if (p == std::string::npos) { reply = "usage: signs:add:<x>:<y>:<z>:<a>:<b>:<c>:<text>"; return; }
            ewb::Sign s; bool skip = false;
            if (!ewb::parse_sign_line(rest.substr(p + 1), s, skip)) {
                reply = "error: malformed sign line"; return;
            }
            // The same door a player's sign write goes through (stage 7.30): the cap
            // applies, and a sign for a slot that already holds one replaces it rather
            // than sitting beside it as a duplicate a later player edit would prune.
            size_t total = 0;
            ewb::SignUpsert r;
            {
                std::lock_guard<std::mutex> lk(g_signMtx);
                r = ewb::upsert_sign(g_signs, s, SV_MAX_SIGNS);
                if (r == ewb::SignUpsert::Added || r == ewb::SignUpsert::Replaced) signsChangedLocked();
                total = g_signs.size();
            }
            const std::string at = std::to_string(s.x) + "," + std::to_string(s.y) + "," + std::to_string(s.z);
            if (r == ewb::SignUpsert::Full) {
                reply = "error: sign cap reached (" + std::to_string(SV_MAX_SIGNS) + "); sign not added";
                return;
            }
            if (r == ewb::SignUpsert::Unchanged) {
                reply = "ok: sign at " + at + " already present (" + std::to_string(total) + " total)";
                return;
            }
            saveSigns();
            auditLog("control", std::string(r == ewb::SignUpsert::Added ? "signs add " : "signs replace ") + at);
            reply = std::string(r == ewb::SignUpsert::Added ? "ok: added sign at " : "ok: replaced sign at ") +
                    at + " (" + std::to_string(total) + " total)";
            return;
        }
        if (sub == "rm") {
            if (f.size() < 4) { reply = "usage: signs:rm:<x>:<y>:<z>"; return; }
            int x, y, z;
            try { x = std::stoi(f[1]); y = std::stoi(f[2]); z = std::stoi(f[3]); }
            catch (...) { reply = "error: non-numeric coordinate"; return; }
            size_t removed = 0;
            {
                std::lock_guard<std::mutex> lk(g_signMtx);
                removed = ewb::remove_signs_on_block(g_signs, x, y, z);
                if (removed) signsChangedLocked();
            }
            if (removed) saveSigns();
            auditLog("control", "signs rm " + std::to_string(x) + "," + std::to_string(y) + "," +
                                std::to_string(z) + " (" + std::to_string(removed) + " removed)");
            reply = removed ? "ok: removed " + std::to_string(removed) + " sign(s)"
                            : "error: no sign at " + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(z);
            return;
        }
        reply = "usage: " + std::string(spec->usage);
        return;
    }

    if (verb == "motd") {
        const auto f = ewb::ctl_fields(rest, 1);
        const std::string sub = f.empty() ? "" : f[0];
        if (sub == "reload") {
            // Nothing in the server ever writes eden_motd.txt — it is operator
            // input only — so unlike `signs reload` there is no unsaved in-memory
            // state to protect and no refusal case. Re-read and swap.
            const size_t n = loadMotd();
            auditLog("control", "motd reload (" + std::to_string(n) + " line(s))");
            reply = n ? "ok: reloaded " + std::to_string(n) + " MOTD line(s) from " + g_motdFile
                      : "ok: MOTD is now empty (" + g_motdFile + " is missing or has no text)";
            return;
        }
        if (sub == "show") {
            std::vector<std::string> motd;
            { std::lock_guard<std::mutex> lk(g_motdMtx); motd = g_motdLines; }
            if (motd.empty()) { reply = "(no MOTD; " + g_motdFile + " is missing or has no text)"; return; }
            std::string out;
            for (const std::string& l : motd) out += l;   // each already ends in '\n'
            reply = out;
            return;
        }
        reply = "usage: " + std::string(spec->usage);
        return;
    }

    if (verb == "region-stats") {
        const uint64_t reqs = g_rgnRequests.load(std::memory_order_relaxed);
        const uint64_t us   = g_rgnMicros.load(std::memory_order_relaxed);
        std::ostringstream ss;
        ss << "REGION service since start:\n"
           << "  requests served : " << reqs << "\n"
           << "  slots scanned   : " << g_rgnCellsScanned.load(std::memory_order_relaxed)
                                     << "  (chunk slots visited, not the whole world — stage 7.6)\n"
           << "  records emitted : " << g_rgnRecords.load(std::memory_order_relaxed) << "\n"
           << "  bytes out       : " << g_rgnBytesOut.load(std::memory_order_relaxed) << "\n"
           << "  total time      : " << (us / 1000) << " ms  (scan + sort; encode and"
                                        " drain are the writer's)\n"
           << "  mean per region : " << (reqs ? (double)us / reqs / 1000.0 : 0.0) << " ms\n"
           << "world lock held (stage 7.6 — what every other player's ACTION waits on):\n"
           << "  REGION scans    : " << (g_rgnLockMicros.load(std::memory_order_relaxed) / 1000)
                                     << " ms total, mean "
                                     << (reqs ? (double)g_rgnLockMicros.load(std::memory_order_relaxed) / reqs / 1000.0 : 0.0)
                                     << " ms, worst "
                                     << (g_rgnLockMaxMicros.load(std::memory_order_relaxed) / 1000.0) << " ms\n"
           << "  world saves     : " << g_saveCount.load(std::memory_order_relaxed) << " write(s), snapshot "
                                     << (g_saveLockMicros.load(std::memory_order_relaxed) / 1000)
                                     << " ms under lock (worst "
                                     << (g_saveLockMaxMicros.load(std::memory_order_relaxed) / 1000.0)
                                     << " ms), disk "
                                     << (g_saveWriteMicros.load(std::memory_order_relaxed) / 1000)
                                     << " ms, last file " << g_saveBytes.load(std::memory_order_relaxed) << " B\n"
           << "backpressure since start (stage 7.3):\n"
           << "  regions refused : " << g_rgnRefused.load(std::memory_order_relaxed)
                                     << "  (client queue full or server backlog)\n"
           << "  records queued  : " << g_rgnPendingRecs.load(std::memory_order_relaxed)
                                     << " of " << g_regionPending << " right now\n"
           << "  clients dropped : " << g_slowDrops.load(std::memory_order_relaxed)
                                     << "  (too far behind on world updates)\n";
        reply = ss.str();
        return;
    }

    reply = "error: command '" + verb + "' recognised but not implemented";
}

// One control connection: read '\n'-framed lines, answer each, until the peer
// closes. `stop` sets serverRunning=false and exits the process after a flush.
static void handleControlClient(int fd) {
    t_editActor = "control";   // who the audit names when a setblock/fill removes a sign
    // A read timeout, so an abandoned `nc -U` cannot hold one of the connection
    // slots (and its thread) until the process exits.
    if (ewb::CTL_IDLE_TIMEOUT_SEC > 0) {
        timeval tv{ewb::CTL_IDLE_TIMEOUT_SEC, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    std::string acc;
    char buf[1024];
    bool stopServer = false;
    // Per-connection command pacing (stage 3.4). Tier 1 is the *more* privileged
    // surface and until now had only the oversized-line guard below, so a local
    // script in a retry loop could issue world-locking commands as fast as the
    // kernel carried them. See control.h's CtlFlood for why this exists.
    ewb::CtlFlood flood(g_ctlCmdBurst, g_ctlCmdRate);
    bool flooded = false;

    while (!stopServer && !flooded) {
        const ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            const char* m = "error: idle timeout\n";
            send(fd, m, strlen(m), 0);
            break;
        }
        if (n <= 0) break;
        acc.append(buf, (size_t)n);
        if (acc.find('\n') == std::string::npos && acc.size() > SV_MAX_LINE) {
            const char* m = "error: oversized line\n";
            send(fd, m, strlen(m), 0);
            break;
        }
        size_t nl;
        while ((nl = acc.find('\n')) != std::string::npos) {
            const std::string line = acc.substr(0, nl);
            acc.erase(0, nl + 1);
            if (line.empty()) continue;

            const ewb::CtlFlood::Verdict v = flood.check(monoSeconds());
            if (v != ewb::CtlFlood::Allow) {
                const char* m = (v == ewb::CtlFlood::Disconnect)
                                    ? "error: rate limited (disconnecting)\n"
                                    : "error: rate limited, slow down\n";
                send(fd, m, strlen(m), 0);
                if (v == ewb::CtlFlood::Disconnect) {
                    std::cerr << "[Server] control socket: disconnected a flooding client after "
                              << flood.strikes << " refusals." << std::endl;
                    flooded = true;
                    break;
                }
                continue;   // drop the command, keep the connection
            }

            std::string reply;
            handleControlLine(line, reply, stopServer);
            if (!reply.empty()) {
                if (reply.back() != '\n') reply += '\n';
                // A reply can be large (a full `topmap` is ~700 KB), and one send()
                // on a stream socket may take only part of it.
                size_t off = 0;
                while (off < reply.size()) {
                    const ssize_t n = send(fd, reply.data() + off, reply.size() - off, 0);
                    if (n < 0 && errno == EINTR) continue;
                    if (n <= 0) break;
                    off += (size_t)n;
                }
            }
            if (stopServer) break;
        }
    }
    close(fd);
    g_ctlConns.fetch_sub(1, std::memory_order_relaxed);
    if (stopServer) {
        if (!g_controlSocket.empty()) unlink(g_controlSocket.c_str());
        std::cout << "Server terminated (control: stop)." << std::endl;
        shutdownAndExit();
    }
}

// Accept loop for the control socket. Runs on its own thread.
static void controlThread() {
    const int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) { std::cerr << "[Server] control socket: " << strerror(errno) << std::endl; return; }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (g_controlSocket.size() >= sizeof(addr.sun_path)) {
        std::cerr << "[Server] control socket path too long (" << g_controlSocket.size()
                  << " >= " << sizeof(addr.sun_path) << "); disabling. Use --control-socket." << std::endl;
        close(s);
        return;
    }
    std::strncpy(addr.sun_path, g_controlSocket.c_str(), sizeof(addr.sun_path) - 1);

    unlink(g_controlSocket.c_str());   // clear a stale socket from a previous run
    if (bind(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
        std::cerr << "[Server] control socket bind " << g_controlSocket << ": "
                  << strerror(errno) << std::endl;
        close(s);
        return;
    }
    // Filesystem permissions ARE the authentication — owner only.
    if (chmod(g_controlSocket.c_str(), S_IRUSR | S_IWUSR) != 0)
        std::cerr << "[Server] control socket chmod 0600 failed: " << strerror(errno) << std::endl;
    if (listen(s, 8) != 0) {
        std::cerr << "[Server] control socket listen: " << strerror(errno) << std::endl;
        close(s);
        return;
    }
    std::cout << "[Server] Control socket at " << g_controlSocket << " (0600). Drive it with edenctl."
              << std::endl;
    std::cout << "[Server] Control limits: " << g_ctlMaxConns << " concurrent connection(s), ";
    if (g_ctlCmdRate <= 0.0) std::cout << "no command rate limit";
    else                     std::cout << g_ctlCmdRate << " cmd/s (burst " << g_ctlCmdBurst << ")";
    std::cout << ", fill \u2264 " << g_ctlFillCap << " cells." << std::endl;

    double lastCtlAcceptFailLog = 0.0;
    while (serverRunning) {
        const int c = accept(s, nullptr, nullptr);
        if (c < 0) {
            if (!serverRunning) break;
            // Same policy as the client listener (stage 7.10; missed here until 7.25):
            // a persistent EMFILE/ENFILE — which the client loop's own exhaustion
            // produces — must not become a tight 100%-CPU spin on the one socket an
            // operator needs working at that moment.
            const int err = errno;
            const auto act = ewb::classify_accept_error(err);
            if (act != ewb::AcceptErrorAction::Retry) {
                const double now = monoSeconds();
                if (now - lastCtlAcceptFailLog >= 1.0) {
                    std::cerr << "[Server] Control accept failed: " << strerror(err) << std::endl;
                    lastCtlAcceptFailLog = now;
                }
                if (act == ewb::AcceptErrorAction::BackoffSleep)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }
        // Bound the thread count as well as the command rate: accepting is the
        // cheap half, spawning a thread per connection is not.
        if (g_ctlConns.fetch_add(1, std::memory_order_relaxed) >= g_ctlMaxConns) {
            g_ctlConns.fetch_sub(1, std::memory_order_relaxed);
            const char* m = "error: too many control connections\n";
            send(c, m, strlen(m), 0);
            close(c);
            continue;
        }
        std::thread(handleControlClient, c).detach();
    }
    close(s);
    unlink(g_controlSocket.c_str());
}


// --- Tier 2 player command surface (stage 3.3) -------------------------------
// These are chat lines: the player types `//set 2` into the game's chat box, the
// client sends an ordinary `MSG:`, and everything below runs on input from a peer
// that has done nothing but complete a JOIN. That is why this tier — not the
// operator socket above — is the security-critical one.
//
// Three structural rules, each closing a numbered defect the community WorldEdit
// patch shipped with (plan §0.5.7):
//
//   1. Every command is a row in `worldedit.h`'s table with a permission floor,
//      checked *before* dispatch. A handler cannot forget a check it never makes
//      (defect 2).
//   2. A selection is only readable through `Selection::box`, which refuses an
//      oversized box. `//copy` cannot inherit the unbounded-iteration hole
//      because there is no unbounded accessor to inherit (defect 1).
//   3. Per-connection state — selection, clipboard, undo, budgets — lives on the
//      connection's own stack, not in a socket-keyed map. There is no
//      `playerInfoMap[sock]` to default-construct a junk player with (defect 6)
//      and no username-keyed state to collide with a duplicate name (defect 9).

// The block `//up` stands the player on. 58 is the modded patch's choice (glass);
// it is a placed block like any other and is recorded in the undo batch.
static const int SV_WE_PLATFORM_BLOCK = 58;

// Whose whisper `/r` answers, keyed by the *recipient's* name. This is the one
// piece of Tier 2 state another thread has to write — the sender's thread marks
// the recipient — so it is the one piece that is global and locked.
static std::map<std::string, std::string> g_lastWhisper;
static std::mutex                         g_whisperMtx;

// The cells one command left alone because a protected zone covers them (stage 8.2).
// WorldEdit is server-authoritative — nothing was drawn before the server decided — so
// a skipped cell needs no restore, only a line in the reply.
struct WeZoneSkip {
    size_t cells = 0;
    std::string zone;      // the first zone that refused a cell
    int x = 0, y = 0, z = 0;
    void note(const ewb::Zone& zn, int cx, int cy, int cz) {
        if (cells++ == 0) { zone = zn.name; x = cx; y = cy; z = cz; }
    }
};

// Everything else a connection remembers between commands. One instance per
// `handleClient` frame; freed with the connection, no cleanup path to forget.
struct WeSession {
    WeZoneSkip                zoneSkip; // reset by every commit; read by the reply
    ewb::Selection            sel;
    std::vector<ewb::ClipCell> clip;
    ewb::UndoStore            hist;
    ewb::TokenBucket          cells;    // cells/sec, not commands/sec
    ewb::TokenBucket          cmds;     // a cheap guard on command *spam*
    WeSession()
        : cells(g_weCellBurst, g_weCellRate), cmds(20.0, 5.0) {
        hist.budget_bytes = g_weUndoBudget;
    }
};

// Unicast a `[Server] ...` line to one player.
static void weSay(SOCKET s, const std::string& text) {
    sendLine(s, "[Server] " + text + "\n");
}

// A player's permission level right now — re-read per command, so a `deop` from
// the control socket takes effect on the next line, not the next session. A name
// with a PIN gets its level only once this connection has logged in (stage 8.6).
static int weLevel(SOCKET s, const std::string& name) {
    std::string rosterName;
    const bool verified = sessionIdentity(s, rosterName);
    return sessionLevel(name, verified);
}

// Read a connected player's position. `find`, never `operator[]` — plan §0.5.7
// defect 6: the patch's `playerInfoMap[clientSocket]` default-constructed an
// entry for any command that arrived before JOIN.
static bool weClientPos(SOCKET s, float& x, float& y, float& z) {
    std::lock_guard<std::mutex> lock(clientsMutex);
    auto it = playerInfoMap.find(s);
    if (it == playerInfoMap.end()) return false;
    x = it->second.posX; y = it->second.posY; z = it->second.posZ;
    return true;
}

// The player's feet, rounded to a cell — what `//pos1`, `//copy` and `//paste`
// are all relative to. posY is eye/centre height, so feet are one below.
static bool weFeet(SOCKET s, int& x, int& y, int& z) {
    float fx, fy, fz;
    if (!weClientPos(s, fx, fy, fz)) return false;
    x = (int)lroundf(fx);
    y = (int)lroundf(fy - 1.0f);
    z = (int)lroundf(fz);
    return true;
}

static bool weCellInRange(int x, int y, int z) {
    return y >= 0 && y < SV_WORLD_HEIGHT && x >= 0 && z >= 0 && x <= 0xFFFFFF && z <= 0xFFFFFF;
}

// Charge a command against the player's cell budget before any of it runs. The
// unit is cells because a single `//sphere 40 2` is one command and ~268 000
// cells; the cost charged is the box the command will *scan*, since scanning is
// what holds the world lock.
static bool weCharge(WeSession& we, SOCKET s, long long cells) {
    if (g_weCellRate <= 0.0) return true;
    if (we.cells.allow(monoSeconds(), (double)cells)) return true;
    weSay(s, "Slow down — you have spent your edit budget for now.");
    return false;
}

// Refuse a box that is too big to read, and say how big it was. This is the
// message a player sees when they hit the cap that defect 1 did not have.
static bool weBoxOk(SOCKET s, long long volume) {
    if (volume <= g_weMaxCells) return true;
    weSay(s, "That area is " + std::to_string(volume) + " cells; the limit is " +
             std::to_string(g_weMaxCells) + ".");
    return false;
}

// Read a cell the way every WorldEdit command sees it: a stored cell as stored, an
// absent one as the natural block every client draws there (`ewb::we_base_at`,
// stage 3.7). Returns whether the cell is stored. Caller holds g_worldMtx.
//
// Reading an absent cell as its natural block rather than as air is what makes
// the undo record right: the "before" half of a `//set` over untouched ground is
// grass, so `//undo` puts grass back (3.7 G; it used to put air).
static bool weRead(int x, int y, int z, int& type, int& color) {
    Cell cur{0, 0};
    if (worldGet(x, y, z, cur)) { type = cur.type; color = cur.color; return true; }
    const ewb::BaseVoxel b = ewb::we_base_at(y);
    type = b.type;
    color = b.paint;
    return false;
}

// The world cell cap, checked once for a whole decided batch before any of it is
// written. `fresh` is how many of its cells the world does not hold yet — the only
// ones the cap can refuse. (3.7 C: this used to charge the box's whole volume, so
// a near-full world refused edits that stored nothing new.) Refuse rather than
// truncate: a partially applied edit is worse than a refused one, and the player
// can see why. Caller holds g_worldMtx.
static bool weCapAllows(size_t fresh, SOCKET s) {
    if (g_world.size() + fresh <= g_maxWorldCells) return true;
    weSay(s, "The world is at its edited-cell limit; that edit was refused.");
    return false;
}

// Write a decided batch. The caller holds g_worldMtx and has passed weCapAllows,
// so worldSet has no reason to refuse; a cell it refuses anyway is dropped, so what
// is relayed and recorded for undo is exactly what was written.
static void weWriteLocked(std::vector<ewb::WeEdit>& batch) {
    size_t kept = 0;
    for (size_t i = 0; i < batch.size(); ++i) {
        const ewb::WeEdit e = batch[i];
        if (worldSet(e.x, e.y, e.z, e.newType, e.newColor)) batch[kept++] = e;
    }
    batch.resize(kept);
}

// Write a decided set of cells into the model and relay them.
//
// `edits` supplies x/y/z and the new type/colour; the old values are filled in
// here, under the same lock as the write, so an undo record can never disagree
// with what was actually overwritten. Cells that would not change
// (`ewb::we_edit_changes`) and out-of-range cells are dropped. The batch is
// decided first and written second, so the cap is checked against the cells it
// actually adds.
//
// The lock covers deciding and writing the batch and nothing else — formatting
// the relay (the larger half, see `emitEditBatch`) and sending it both happen
// after it is released, so a large batch never blocks another player's REGION
// behind string building or a socket write.
//
// Protected zones (stage 8.2): a cell a zone covers is skipped and counted in `skip`,
// and the rest of the batch applies. The zone snapshot is taken before g_worldMtx; the
// per-cell check only runs when the batch's bounding box meets a zone at all, and only
// on cells that would actually change.
static std::vector<ewb::WeEdit> weCommit(const std::vector<ewb::WeEdit>& edits, SOCKET s,
                                         WeZoneSkip& skip) {
    std::vector<ewb::WeEdit> batch;
    batch.reserve(edits.size());
    skip = WeZoneSkip{};
    ZoneCheck zc = zoneCheck(s);
    if (edits.empty() || zc.zones->size() == 0) {
        zc.zones.reset();
    } else {
        int x0 = edits[0].x, y0 = edits[0].y, z0 = edits[0].z, x1 = x0, y1 = y0, z1 = z0;
        for (const ewb::WeEdit& e : edits) {
            x0 = std::min(x0, e.x); y0 = std::min(y0, e.y); z0 = std::min(z0, e.z);
            x1 = std::max(x1, e.x); y1 = std::max(y1, e.y); z1 = std::max(z1, e.z);
        }
        zc = zoneCheckNear(zc, x0, y0, z0, x1, y1, z1);
    }
    {
        std::lock_guard<std::mutex> lock(g_worldMtx);
        size_t fresh = 0;
        for (const ewb::WeEdit& e : edits) {
            if (!weCellInRange(e.x, e.y, e.z)) continue;
            int ct, cc;
            const bool have = weRead(e.x, e.y, e.z, ct, cc);
            if (!ewb::we_edit_changes(have, ct, cc, e.newType, e.newColor)) continue;
            if (const ewb::Zone* zn = zoneDenies(zc, e.x, e.y, e.z)) { skip.note(*zn, e.x, e.y, e.z); continue; }
            if (!have) ++fresh;
            batch.push_back({e.x, e.y, e.z, (unsigned char)ct, (unsigned char)cc,
                             e.newType, e.newColor});
        }
        if (!weCapAllows(fresh, s)) { batch.clear(); return batch; }
        weWriteLocked(batch);
    }
    std::string wire = emitEditBatch(batch);   // formatting, outside the lock
    if (!wire.empty()) broadcastWorld(std::move(wire), INVALID_SOCKET);
    return batch;
}

// Scan an inclusive box, ask `want` what each cell should become, and commit the
// answer. `want(x, y, z, present, curType, curColor, newType, newColor) -> bool`
// returns false to leave a cell alone.
//
// `present` distinguishes "a player carved this to air" (`type 0`) from "nobody
// has touched this cell" (absent). The patch this replaces conflated the two, so
// `//replace 0 <block>` silently filled every untouched cell in the box with a
// placed block. Its own help text promised the opposite ("WorldEdit only detects
// player-made blocks"). For an absent cell `curType`/`curColor` are the natural
// block (`weRead`), so a callback that reads them sees what the player sees.
//
// A cell whose answer changes nothing is skipped (`ewb::we_edit_changes`): that is
// what keeps `//set 0` in open sky from storing thousands of air cells (3.7 B).
//
// The caller has already volume-checked the box and charged the budget.
//
// Protected cells are skipped into `skip`, exactly as in weCommit.
template <typename F>
static std::vector<ewb::WeEdit> weEditBox(const ewb::WeBox& box, F&& want, SOCKET s, WeZoneSkip& skip) {
    std::vector<ewb::WeEdit> batch;
    skip = WeZoneSkip{};
    const ZoneCheck zc = zoneCheckNear(zoneCheck(s), box.x0, box.y0, box.z0, box.x1, box.y1, box.z1);
    {
        std::lock_guard<std::mutex> lock(g_worldMtx);
        size_t fresh = 0;
        for (int x = box.x0; x <= box.x1; ++x)
            for (int z = box.z0; z <= box.z1; ++z)
                for (int y = box.y0; y <= box.y1; ++y) {
                    if (!weCellInRange(x, y, z)) continue;
                    int curType, curColor;
                    const bool have = weRead(x, y, z, curType, curColor);
                    int newType = curType, newColor = curColor;
                    if (!want(x, y, z, have, curType, curColor, newType, newColor)) continue;
                    if (!ewb::we_edit_changes(have, curType, curColor, newType, newColor)) continue;
                    if (const ewb::Zone* zn = zoneDenies(zc, x, y, z)) { skip.note(*zn, x, y, z); continue; }
                    if (!have) ++fresh;
                    batch.push_back({x, y, z, (unsigned char)curType, (unsigned char)curColor,
                                     (unsigned char)newType, (unsigned char)newColor});
                }
        if (!weCapAllows(fresh, s)) { batch.clear(); return batch; }
        weWriteLocked(batch);
    }
    std::string wire = emitEditBatch(batch);   // formatting, outside the lock
    if (!wire.empty()) broadcastWorld(std::move(wire), INVALID_SOCKET);
    return batch;
}

// Tell the player (always — it is the reply to their command) and the audit channel
// (folded, like every zone refusal) about cells the last commit skipped. Call it before
// the command's completion line: docs/commands.md promises that line comes last.
static void weZoneReport(WeSession& we, SOCKET s, const std::string& username, const std::string& verb) {
    const WeZoneSkip& k = we.zoneSkip;
    if (!k.cells) return;
    weSay(s, std::to_string(k.cells) + " cell(s) skipped: protected area '" + k.zone + "'.");
    zoneAudit(username, k.zone, verb + " (" + std::to_string(k.cells) + " cell(s))", k.x, k.y, k.z);
}

// Record an applied batch for undo and tell the player what happened.
static void weFinish(WeSession& we, SOCKET s, const std::string& username,
                     const std::string& verb, std::vector<ewb::WeEdit> batch) {
    const size_t n = batch.size();
    if (n) {
        we.hist.record(std::move(batch));
        editsDirty = true;
        // Unconditional, not --verbose: an edit that changed the world is the
        // thing a public server most needs a record of, and every mutating
        // command lands here (the two that don't — //undo///redo and //up —
        // audit themselves). Volume is bounded by the same cell budget that
        // bounds the edits, so this cannot be flooded faster than the edits can.
        auditLog("player:" + username, verb + ": " + std::to_string(n) + " cell(s)");
        drainSignRemovals();   // signs on blocks this edit turned to air
    }
    weZoneReport(we, s, username, verb);   // before the completion line, which is always last
    weSay(s, verb + ": " + std::to_string(n) + " block(s) changed.");
}

// Resolve the selection for a command that needs one, reporting the reason it
// could not. Every box-editing command funnels through here.
static bool weSelection(WeSession& we, SOCKET s, ewb::WeBox& box) {
    long long vol = 0;
    if (!we.sel.complete()) {
        weSay(s, "Set //pos1 and //pos2 first.");
        return false;
    }
    if (!we.sel.box(box, g_weMaxCells, vol)) {
        weBoxOk(s, vol);
        return false;
    }
    return weCharge(we, s, vol);
}

// Teleport: the server tells one client where it now is. `SPAWN` is the same
// primitive the join sequence uses to restore a saved position (plan §0.5.2) —
// community-attested as a live teleport, not capture-confirmed.
static void weTeleport(SOCKET s, const std::string& username, float x, float y, float z) {
    rememberPos(username, x, y, z);
    sendLine(s, spawnLine(x, y, z));
}

// `/login <pin>` (stage 8.6). Called from the MSG handler *before* the WorldEdit
// dispatcher, and before its "commands are disabled" check — identity is not
// WorldEdit — so the line never reaches anything that logs, audits or echoes a
// command. ⚠️ Nothing here may print `line` or the PIN: failures log the name and
// the address only.
//
// Cost control, cheapest first: a malformed PIN, a name with no PIN, an IP inside
// its lockout and this connection's own attempt budget are all refused before the
// (deliberately slow) hash runs, so a /login flood costs the server a token-bucket
// check per line, not a PBKDF2 each.
static void handleLogin(SOCKET s, const std::string& username, const std::string& ip,
                        const std::string& line, ewb::TokenBucket& attempts) {
    const std::vector<std::string> tok = ewb::we_split_args(line);
    if (tok.size() != 2) { weSay(s, "Usage: /login <pin>"); return; }
    const std::string& pin = tok[1];
    // Free to refuse: nothing is hashed, nothing counted against the budget below.
    if (!ewb::auth_pin_wellformed(pin)) {
        weSay(s, "Wrong PIN. A PIN is " + std::to_string(ewb::AUTH_PIN_DIGITS) + " digits.");
        return;
    }

    std::string rosterName;
    if (sessionIdentity(s, rosterName)) { weSay(s, "You are already logged in."); return; }
    ewb::AuthRecord rec;
    {
        std::lock_guard<std::mutex> lk(g_authMtx);
        const ewb::AuthRecord* r = g_auth.find(username);
        if (!r) { weSay(s, "The name " + username + " has no PIN, so there is nothing to log in to."); return; }
        rec = *r;
    }
    const double now = monoSeconds();
    {
        std::lock_guard<std::mutex> lk(g_loginFailMtx);
        if (g_loginFail.blocked(ip, now)) {
            weSay(s, "Too many wrong PINs from your address; try again later.");
            return;
        }
    }
    if (!attempts.allow(now)) { weSay(s, "Too many login attempts; wait a few seconds."); return; }

    if (!ewb::auth_verify(rec, pin)) {   // slow on purpose; no lock held
        size_t inWindow;
        bool locked;
        {
            std::lock_guard<std::mutex> lk(g_loginFailMtx);
            inWindow = g_loginFail.record_failure(ip, monoSeconds());
            locked = g_loginFail.blocked(ip, monoSeconds());
        }
        std::cout << "[Server] failed /login for " << username << " from " << ip;
        if (inWindow) std::cout << " (" << inWindow << " in window)";
        std::cout << "." << std::endl;
        if (locked)
            auditLog("player:" + username, "/login locked out " + ip + " after " +
                                               std::to_string(inWindow) + " wrong PIN(s)");
        weSay(s, "Wrong PIN.");
        return;
    }

    // Publish under g_authMtx -> clientsMutex (the declared order): if an operator
    // re-issued or removed the PIN while the hash ran, the record we checked is no
    // longer the live one and the login must not count.
    bool stale = false, gone = false;
    {
        std::lock_guard<std::mutex> ak(g_authMtx);
        const ewb::AuthRecord* r = g_auth.find(username);
        if (!r || r->hash != rec.hash || r->salt != rec.salt) {
            stale = true;
        } else {
            std::lock_guard<std::mutex> lk(clientsMutex);
            auto it = playerInfoMap.find(s);
            if (it == playerInfoMap.end() || it->second.username != username) gone = true;
            else it->second.verified = true;
        }
    }
    if (gone) return;
    if (stale) { weSay(s, "Your PIN was changed a moment ago; ask the operator for the new one."); return; }
    const int lvl = sessionLevel(username, true);
    auditLog("player:" + username, "logged in from " + ip + " (level " + std::to_string(lvl) + ")");
    weSay(s, "Logged in as " + username + ". Your level is " + std::to_string(lvl) + ".");
}

// One complete Tier 2 command line (the chat text, '/'-prefixed and already
// sanitised). `regionLimiter` is the connection's REGION pacing — `/resync` is a
// REGION by another name and is paced by the same budget, so it cannot be used
// to step around it.
static void handleWorldEditLine(SOCKET s, const std::string& username,
                                const std::string& line, WeSession& we,
                                BurstLimiter& regionLimiter) {
    const std::vector<std::string> tok = ewb::we_split_args(line);
    if (tok.empty()) return;
    const std::string verb = tok[0];
    const int argc = (int)tok.size() - 1;
    // `/login` never gets here (the MSG handler takes it first, see handleLogin).
    // If it ever did, drop it rather than let the audit below see a PIN.
    if (verb == "/login") return;

    if (!we.cmds.allow(monoSeconds())) return;   // command spam; silent, costs nothing

    const ewb::WeSpec* spec = ewb::we_find(verb);
    if (!spec) {
        weSay(s, "Unknown command '" + verb + "'. Try /help.");
        return;
    }

    // ⚠️ The permission gate. It is here, once, before dispatch — not in each
    // handler, which is how the patch this replaces came to have none at all.
    const int level = weLevel(s, username);
    const int need = (verb == "/tp") ? ewb::we_tp_required_level(argc) : spec->min_level;
    if (level < need) {
        weSay(s, "You do not have permission for that (level " + std::to_string(need) + " required).");
        return;
    }
    if (!ewb::we_args_ok(*spec, argc)) {
        weSay(s, "Usage: " + std::string(spec->usage));
        return;
    }

    // Anything that reaches across players — today that is only `/tp <player>`,
    // which discloses their exact position (plan §0.5.7 defect 8) — is audited
    // on the way in, because it changes no cells and so never reaches weFinish.
    // Mutating commands are audited on the way *out* with the count they
    // actually changed; gating on `need` rather than `level` is what keeps an
    // operator's `//set` from being logged twice.
    if (need >= ewb::WE_LEVEL_OPERATOR)
        auditLog("player:" + username, "(level " + std::to_string(level) + ") " + line);

    // --- level 0: read-only and self-scoped ----------------------------------

    if (verb == "/help") {
        int page = 1;
        if (argc == 1 && !ewb::we_parse_int(tok[1], page)) page = 1;
        const std::vector<std::string> lines = ewb::we_help_lines(level);
        // The game's chat pane is a few lines tall, so paginate rather than
        // flooding it with thirty lines nobody can scroll back through.
        const int per = 6;
        const int pages = ((int)lines.size() + per - 1) / per;
        if (page < 1 || page > pages) {
            weSay(s, "Page must be 1.." + std::to_string(pages) + ".");
            return;
        }
        weSay(s, "--- help " + std::to_string(page) + "/" + std::to_string(pages) + " ---");
        for (int i = (page - 1) * per; i < (int)lines.size() && i < page * per; ++i)
            weSay(s, lines[i]);
        return;
    }

    if (verb == "/msg" || verb == "/r") {
        std::string target, text;
        if (verb == "/msg") {
            target = tok[1];
            text   = ewb::we_rest_after(line, 2);
        } else {
            {
                std::lock_guard<std::mutex> lk(g_whisperMtx);
                auto it = g_lastWhisper.find(username);
                if (it != g_lastWhisper.end()) target = it->second;
            }
            if (target.empty()) { weSay(s, "Nobody has whispered to you."); return; }
            text = ewb::we_rest_after(line, 1);
        }
        text = ewb::sanitize_text(text, SV_MAX_CHAT);
        if (text.empty()) { weSay(s, "Say something."); return; }
        if (target == username) { weSay(s, "You are already talking to yourself."); return; }

        SOCKET dest = INVALID_SOCKET;
        {
            std::lock_guard<std::mutex> lock(clientsMutex);
            for (const auto& kv : playerInfoMap)
                if (kv.second.username == target) { dest = kv.first; break; }
        }
        if (dest == INVALID_SOCKET) { weSay(s, "'" + target + "' is not here."); return; }
        {
            std::lock_guard<std::mutex> lk(g_whisperMtx);
            g_lastWhisper[target] = username;   // so their /r comes back to us
        }
        const std::string toThem = "[" + username + " whispers] " + text + "\n";
        const std::string toUs   = "[you tell " + target + "] " + text + "\n";
        sendLine(dest, toThem);
        sendLine(s, toUs);
        return;
    }

    if (verb == "/id") {
        // A name or a number, resolved through the stage 3.6 tables
        // (`eden_names.h`). The tables are community/RE-derived and non-
        // authoritative (plan §0.5.1); numbers are the ground truth and always
        // work.
        const std::string& arg = tok[1];
        int v = 0;
        if (ewb::we_parse_int(arg, v)) {
            std::string out = arg + ":";
            const char* bn = ewb::eden_block_name(v);
            const char* cn = ewb::eden_paint_name(v);
            if (v >= 0 && v <= ewb::MAX_BLOCK_TYPE)
                out += std::string(" block") + (*bn ? " '" + std::string(bn) + "'" : "");
            if (v >= 0 && v <= ewb::MAX_PAINT_INDEX)
                out += std::string(out.size() > arg.size() + 1 ? ", " : " ") + "colour" +
                       (*cn ? " '" + std::string(cn) + "'" : "");
            if (out == arg + ":")
                out += " not a valid block (0.." + std::to_string(ewb::MAX_BLOCK_TYPE) +
                       ") or colour (0.." + std::to_string(ewb::MAX_PAINT_INDEX) + ") id";
            weSay(s, out);
            return;
        }
        const int b = ewb::eden_block_id(arg);
        const int c = ewb::eden_paint_id(arg);
        if (b < 0 && c < 0) { weSay(s, "'" + arg + "' is not a known block or colour name."); return; }
        std::string out = arg + ":";
        if (b >= 0) out += " block " + std::to_string(b);
        if (c >= 0) out += std::string(b >= 0 ? ", " : " ") + "colour " + std::to_string(c);
        weSay(s, out);
        return;
    }

    if (verb == "/searchblocks" || verb == "/searchcolors") {
        const bool colours = (verb == "/searchcolors");
        int n = 0;
        const char* const* table = colours ? ewb::eden_paint_table(n) : ewb::eden_block_table(n);
        std::string hits;
        const int count = ewb::eden_search(table, n, tok[1], colours, 200, hits);
        if (count == 0) weSay(s, "No " + std::string(colours ? "colour" : "block") +
                                 " name contains '" + tok[1] + "'.");
        else            weSay(s, hits);
        return;
    }

    if (verb == "/resync") {
        // A `REGION` the player asked for by name. Three lines, because stage 1.1
        // already built the spatial query and the SNAPZ encoder — the patch this
        // replaces hand-rolled a 274 625-cell probe under the world lock instead.
        float px, py, pz;
        if (!weClientPos(s, px, py, pz)) { weSay(s, "The server does not have your position yet."); return; }
        serveRegion(s, username, (int)lroundf(px), (int)lroundf(pz), regionLimiter);
        return;
    }

    // --- /tp -----------------------------------------------------------------

    if (verb == "/tp") {
        float px, py, pz;
        if (!weClientPos(s, px, py, pz)) { weSay(s, "The server does not have your position yet."); return; }
        float tx, ty, tz;
        if (argc == 3) {
            if (!ewb::we_parse_coord(tok[1], px, tx) ||
                !ewb::we_parse_coord(tok[2], py, ty) ||
                !ewb::we_parse_coord(tok[3], pz, tz)) {
                weSay(s, "Usage: /tp <x> <y> <z>  (~ is your current value)");
                return;
            }
        } else if (argc == 1) {
            // Level 2 only — this reads out another player's exact position
            // (defect 8). The gate is above; this is the lookup.
            bool found = false;
            std::lock_guard<std::mutex> lock(clientsMutex);
            for (const auto& kv : playerInfoMap)
                if (kv.second.username == tok[1]) {
                    tx = kv.second.posX; ty = kv.second.posY; tz = kv.second.posZ;
                    found = true;
                    break;
                }
            if (!found) { weSay(s, "'" + tok[1] + "' is not here."); return; }
        } else {
            weSay(s, "Usage: " + std::string(spec->usage));
            return;
        }
        if (!weCellInRange((int)lroundf(tx), (int)lroundf(ty), (int)lroundf(tz))) {
            weSay(s, "That destination is outside the world.");
            return;
        }
        weTeleport(s, username, tx, ty, tz);
        return;
    }

    // --- protected zones (stage 8.7) ------------------------------------------
    //
    // Level 2 (the table's floor, checked above) **and** logged in with a PIN: a
    // zone is the one thing that stops other players, so an operator's claimed name
    // is not enough to make or remove one. Every change goes through the same
    // snapshot-copy-save-swap as the control socket's zone:* verbs (zonePublish),
    // and is audited by the gate above with the command line.
    if (verb == "/zone") {
        std::string rosterName;
        if (!sessionIdentity(s, rosterName)) {
            weSay(s, "Zone commands need you to be logged in: /login <pin>.");
            return;
        }
        if (!ewb::we_zone_args_ok(tok)) { weSay(s, "Usage: " + std::string(spec->usage)); return; }
        const std::string& sub = tok[1];
        auto boxText = [](const ewb::Zone& z) {
            return "(" + std::to_string(z.x0) + "," + std::to_string(z.y0) + "," + std::to_string(z.z0) +
                   ")..(" + std::to_string(z.x1) + "," + std::to_string(z.y1) + "," + std::to_string(z.z1) + ")";
        };

        if (sub == "list") {
            int page = 1;
            if (tok.size() == 3 && !ewb::we_parse_int(tok[2], page)) page = 1;
            const auto zs = zonesSnapshot();
            if (!zs || zs->size() == 0) { weSay(s, "No protected zones."); return; }
            const int per = 5;
            const int pages = ((int)zs->size() + per - 1) / per;
            if (page < 1 || page > pages) { weSay(s, "Page must be 1.." + std::to_string(pages) + "."); return; }
            weSay(s, "--- zones " + std::to_string(page) + "/" + std::to_string(pages) + " (" +
                         std::to_string(zs->size()) + ") ---");
            for (int i = (page - 1) * per; i < (int)zs->size() && i < page * per; ++i) {
                const ewb::Zone& z = zs->zones()[(size_t)i];
                weSay(s, z.name + " " + boxText(z) + (z.enforced ? "" : " off") +
                             (z.hasLevel ? " level " + std::to_string(z.level) : ""));
            }
            return;
        }

        if (sub == "here") {
            int fx, fy, fz;
            if (!weFeet(s, fx, fy, fz)) { weSay(s, "The server does not have your position yet."); return; }
            const auto zs = zonesSnapshot();
            std::string hit;
            for (const ewb::Zone& z : zs->zones())
                if (z.contains(fx, fy, fz))
                    hit += (hit.empty() ? "" : ", ") + z.name + (z.enforced ? "" : " (off)");
            weSay(s, hit.empty() ? "You are not in a protected zone." : "You are in: " + hit + ".");
            return;
        }

        std::lock_guard<std::mutex> editLk(g_zonesEditMtx);
        ewb::ZoneSet next = *zonesSnapshot();
        std::string err;

        if (sub == "rm") {
            if (!next.remove(tok[2])) { weSay(s, "There is no zone named '" + tok[2] + "'."); return; }
            if (!zonePublish(std::move(next), err)) { weSay(s, "Could not save the zones: " + err); return; }
            weSay(s, "Removed zone '" + tok[2] + "'.");
            return;
        }

        // create: the //pos1..//pos2 box. A zone is about stopping edits, and a
        // selection is usually drawn on the surface, so by default it covers the
        // whole column (bedrock to sky, as a map-drawn zone does) — `exact` keeps
        // the selection's own heights.
        if (!we.sel.complete()) { weSay(s, "Set //pos1 and //pos2 first."); return; }
        ewb::WeBox box;
        long long vol = 0;
        we.sel.box(box, LLONG_MAX, vol);
        const bool exact = tok.size() == 4;
        const int y0 = exact ? box.y0 : 0, y1 = exact ? box.y1 : SV_WORLD_HEIGHT - 1;
        const std::string zl = tok[2] + ":" + std::to_string(box.x0) + ":" + std::to_string(y0) + ":" +
                               std::to_string(box.z0) + ":" + std::to_string(box.x1) + ":" +
                               std::to_string(y1) + ":" + std::to_string(box.z1) + ":all";
        ewb::Zone z;
        if (!ewb::zone_parse_line(zl, z, &err)) { weSay(s, "Cannot make that zone: " + err + "."); return; }
        if (!next.add(z, &err)) { weSay(s, "Cannot make that zone: " + err + "."); return; }
        if (!zonePublish(std::move(next), err)) { weSay(s, "Could not save the zones: " + err); return; }
        weSay(s, "Protected zone '" + z.name + "' " + boxText(z) + " created. Nobody can edit inside it in game.");
        return;
    }

    // --- selection -----------------------------------------------------------

    if (verb == "//pos1" || verb == "//pos2") {
        // No arguments: your feet. Three: an explicit corner (stage 3.7), in the
        // `/tp` grammar with `~` relative to your feet — so `//pos1 ~ ~ ~` is
        // `//pos1`, and a corner given in absolute numbers needs no position at
        // all, which is what lets a tool set a selection without walking to it.
        if (!ewb::we_pos_args_ok(argc)) { weSay(s, "Usage: " + std::string(spec->usage)); return; }
        int x = 0, y = 0, z = 0;
        const bool relative = argc == 0 || ewb::we_coord_relative(tok[1]) ||
                              ewb::we_coord_relative(tok[2]) || ewb::we_coord_relative(tok[3]);
        if (relative && !weFeet(s, x, y, z)) { weSay(s, "The server does not have your position yet."); return; }
        if (argc == 3) {
            if (!ewb::we_parse_cell(tok[1], x, x) || !ewb::we_parse_cell(tok[2], y, y) ||
                !ewb::we_parse_cell(tok[3], z, z)) {
                weSay(s, "Usage: " + verb + " <x> <y> <z>  (~ is your feet)");
                return;
            }
            if (!weCellInRange(x, y, z)) { weSay(s, "That corner is outside the world."); return; }
        }
        if (verb == "//pos1") we.sel.set1(x, y, z); else we.sel.set2(x, y, z);
        std::string msg = verb.substr(2) + " = " + std::to_string(x) + ", " +
                          std::to_string(y) + ", " + std::to_string(z);
        // Report the size now, so a player learns their selection is too big
        // before they type the command that would have been refused.
        if (we.sel.complete()) {
            ewb::WeBox b;
            long long vol = 0;
            we.sel.box(b, g_weMaxCells, vol);
            msg += "  (" + std::to_string(vol) + " cells";
            msg += vol > g_weMaxCells ? " — over the " + std::to_string(g_weMaxCells) + " limit)" : ")";
        }
        weSay(s, msg);
        return;
    }

    // --- box edits -----------------------------------------------------------

    if (verb == "//set" || verb == "//walls") {
        int type = 0, color = 0;
        if (!ewb::we_parse_block(tok[1], type)) { weSay(s, "Unknown block '" + tok[1] + "'."); return; }
        if (argc == 2 && !ewb::we_parse_color(tok[2], color)) { weSay(s, "Unknown colour '" + tok[2] + "'."); return; }
        ewb::WeBox box;
        if (!weSelection(we, s, box)) return;
        const bool walls = (verb == "//walls");
        auto batch = weEditBox(box, [&](int x, int, int z, bool, int, int, int& nt, int& nc) {
            if (walls && x != box.x0 && x != box.x1 && z != box.z0 && z != box.z1) return false;
            nt = type; nc = color;
            return true;
        }, s, we.zoneSkip);
        weFinish(we, s, username, verb, std::move(batch));
        return;
    }

    if (verb == "//paint" || verb == "//unpaint" || verb == "//strip") {
        // `//paint` always names its colour: the arity table requires one, and
        // `//unpaint` is the command that means "colour 0". A no-argument
        // `//paint` that silently stripped paint would be a destructive default.
        int color = 0;
        if (verb == "//paint" && !ewb::we_parse_color(tok[1], color)) {
            weSay(s, "Unknown colour '" + tok[1] + "'.");
            return;
        }
        ewb::WeBox box;
        if (!weSelection(we, s, box)) return;
        // Painting an untouched cell records it as SV_PAINTED_BASE: "the natural
        // block that was here, now painted". That sentinel is exactly what the
        // ACTION paint path and the SNAPZ encoder already agree on, and it is
        // relayed as a lone paint (ewb::we_emit_edit_wire). An untouched cell
        // arrives here as its natural block, so `ct == SV_AIR` is carved air *or*
        // open sky — neither has anything to paint (3.7 A: every sky cell in the
        // box used to become a painted-base cell).
        auto batch = weEditBox(box, [&](int, int, int, bool have, int ct, int, int& nt, int& nc) {
            if (ct == SV_AIR) return false;              // nothing to paint
            nt = have ? ct : SV_PAINTED_BASE;
            nc = color;
            return true;
        }, s, we.zoneSkip);
        weFinish(we, s, username, verb, std::move(batch));
        return;
    }

    if (verb == "//replace") {
        int from = 0, to = 0, color = 0, colorFilter = -1;
        if (!ewb::we_parse_block(tok[1], from) || !ewb::we_parse_block(tok[2], to)) {
            weSay(s, "Usage: " + std::string(spec->usage));
            return;
        }
        if (argc >= 3 && !ewb::we_parse_color(tok[3], color)) { weSay(s, "Unknown colour '" + tok[3] + "'."); return; }
        if (argc >= 4 && !ewb::we_parse_color(tok[4], colorFilter)) { weSay(s, "Unknown colour '" + tok[4] + "'."); return; }
        ewb::WeBox box;
        if (!weSelection(we, s, box)) return;
        auto batch = weEditBox(box, [&](int, int, int, bool have, int ct, int cc, int& nt, int& nc) {
            if (!have || ct != from) return false;                     // player-made cells only
            if (colorFilter >= 0 && cc != colorFilter) return false;
            nt = to; nc = color;
            return true;
        }, s, we.zoneSkip);
        weFinish(we, s, username, verb, std::move(batch));
        return;
    }

    if (verb == "//replacenear") {
        int radius = 0, from = 0, to = 0, color = 0, colorFilter = -1;
        if (!ewb::we_parse_int(tok[1], radius) || radius < 0 || radius > ewb::WE_MAX_RADIUS) {
            weSay(s, "Radius must be 0.." + std::to_string(ewb::WE_MAX_RADIUS) + ".");
            return;
        }
        if (!ewb::we_parse_block(tok[2], from) || !ewb::we_parse_block(tok[3], to)) {
            weSay(s, "Usage: " + std::string(spec->usage));
            return;
        }
        if (argc >= 4 && !ewb::we_parse_color(tok[4], color)) { weSay(s, "Unknown colour '" + tok[4] + "'."); return; }
        if (argc >= 5 && !ewb::we_parse_color(tok[5], colorFilter)) { weSay(s, "Unknown colour '" + tok[5] + "'."); return; }
        int cx, cy, cz;
        if (!weFeet(s, cx, cy, cz)) { weSay(s, "The server does not have your position yet."); return; }
        const ewb::WeBox box = ewb::we_make_box(cx - radius, cy - radius, cz - radius,
                                                cx + radius, cy + radius, cz + radius);
        // The same cap the selection path uses, applied to the box a radius
        // implies. This is the point of capping at *read* time: a command that
        // never touches Selection still cannot outrun the bound.
        const long long vol = ewb::we_box_volume(box);
        if (!weBoxOk(s, vol) || !weCharge(we, s, vol)) return;
        auto batch = weEditBox(box, [&](int x, int y, int z, bool have, int ct, int cc, int& nt, int& nc) {
            if (!ewb::we_in_sphere(x - cx, y - cy, z - cz, radius)) return false;
            if (!have || ct != from) return false;
            if (colorFilter >= 0 && cc != colorFilter) return false;
            nt = to; nc = color;
            return true;
        }, s, we.zoneSkip);
        weFinish(we, s, username, verb, std::move(batch));
        return;
    }

    // --- shapes --------------------------------------------------------------

    if (verb == "//sphere" || verb == "//hsphere" || verb == "//cyl" || verb == "//hcyl") {
        const bool cylinder = (verb == "//cyl" || verb == "//hcyl");
        const bool hollow   = (verb == "//hsphere" || verb == "//hcyl");
        int radius = 0, height = 1, type = 0, color = 0;
        if (!ewb::we_parse_int(tok[1], radius) || radius < 0 || radius > ewb::WE_MAX_RADIUS) {
            weSay(s, "Radius must be 0.." + std::to_string(ewb::WE_MAX_RADIUS) + ".");
            return;
        }
        int next = 2;
        if (cylinder) {
            if (!ewb::we_parse_int(tok[2], height) || height < 1 || height > SV_WORLD_HEIGHT) {
                weSay(s, "Height must be 1.." + std::to_string(SV_WORLD_HEIGHT) + ".");
                return;
            }
            next = 3;
        }
        if (!ewb::we_parse_block(tok[next], type)) { weSay(s, "Unknown block '" + tok[next] + "'."); return; }
        if (argc > next && !ewb::we_parse_color(tok[next + 1], color)) {
            weSay(s, "Unknown colour '" + tok[next + 1] + "'.");
            return;
        }
        int cx, cy, cz;
        if (!weFeet(s, cx, cy, cz)) { weSay(s, "The server does not have your position yet."); return; }
        const ewb::WeBox box = cylinder
            ? ewb::we_make_box(cx - radius, cy, cz - radius, cx + radius, cy + height - 1, cz + radius)
            : ewb::we_make_box(cx - radius, cy - radius, cz - radius, cx + radius, cy + radius, cz + radius);
        const long long vol = ewb::we_box_volume(box);
        if (!weBoxOk(s, vol) || !weCharge(we, s, vol)) return;
        auto batch = weEditBox(box, [&](int x, int y, int z, bool, int, int, int& nt, int& nc) {
            const int dx = x - cx, dy = y - cy, dz = z - cz;
            const bool in = cylinder
                ? (hollow ? ewb::we_in_ring(dx, dz, radius)   : ewb::we_in_disc(dx, dz, radius))
                : (hollow ? ewb::we_in_sphere_shell(dx, dy, dz, radius)
                          : ewb::we_in_sphere(dx, dy, dz, radius));
            if (!in) return false;
            nt = type; nc = color;
            return true;
        }, s, we.zoneSkip);
        weFinish(we, s, username, verb, std::move(batch));
        return;
    }

    // --- clipboard -----------------------------------------------------------

    if (verb == "//copy") {
        ewb::WeBox box;
        if (!weSelection(we, s, box)) return;
        int fx, fy, fz;
        if (!weFeet(s, fx, fy, fz)) { weSay(s, "The server does not have your position yet."); return; }
        // ⚠️ This is defect 1's command. It is safe here for exactly one reason:
        // `weSelection` above refused an oversized box before a lock was taken.
        std::vector<ewb::ClipCell> clip;
        {
            std::lock_guard<std::mutex> lock(g_worldMtx);
            for (int x = box.x0; x <= box.x1; ++x)
                for (int z = box.z0; z <= box.z1; ++z)
                    for (int y = box.y0; y <= box.y1; ++y) {
                        Cell c;
                        if (!worldGet(x, y, z, c)) continue;   // untouched cells aren't ours to copy
                        ewb::ClipCell cell{x - fx, y - fy, z - fz, 0, 0};
                        if (ewb::we_clip_cell(y, c.type, c.color, cell)) clip.push_back(cell);
                    }
        }
        we.clip.swap(clip);
        weSay(s, "Copied " + std::to_string(we.clip.size()) + " player-made block(s).");
        return;
    }

    if (verb == "//paste") {
        if (we.clip.empty()) { weSay(s, "Your clipboard is empty."); return; }
        int fx, fy, fz;
        if (!weFeet(s, fx, fy, fz)) { weSay(s, "The server does not have your position yet."); return; }
        if (!weCharge(we, s, (long long)we.clip.size())) return;
        std::vector<ewb::WeEdit> want;
        want.reserve(we.clip.size());
        for (const ewb::ClipCell& c : we.clip)
            want.push_back({fx + c.rx, fy + c.ry, fz + c.rz, 0, 0, c.type, c.color});
        weFinish(we, s, username, verb, weCommit(want, s, we.zoneSkip));
        return;
    }

    if (verb == "//rotate") {
        int angle = 0;
        if (!ewb::we_parse_int(tok[1], angle) || angle % 90 != 0) {
            weSay(s, "Angle must be 90, 180 or 270.");
            return;
        }
        if (we.clip.empty()) { weSay(s, "Your clipboard is empty."); return; }
        ewb::we_rotate_clipboard(we.clip, angle / 90);
        // ⚠️ Ramp/wedge *orientation* under rotation is unverified — two
        // codebases disagree on the direction (plan §0.5.4). Position rotation is
        // exact; a rotated ramp may face the wrong way until that is settled.
        weSay(s, "Clipboard rotated " + std::to_string(((angle / 90) % 4 + 4) % 4 * 90) +
                 " degrees. Ramp facing is not yet verified.");
        return;
    }

    // --- history -------------------------------------------------------------

    if (verb == "//undo" || verb == "//redo") {
        const bool undo = (verb == "//undo");
        const auto& stack = undo ? we.hist.undo : we.hist.redo;
        if (stack.empty()) {
            weSay(s, undo ? "Nothing to undo." : "Nothing to redo.");
            return;
        }
        // Charged before the batch moves stacks: a refused //undo used to move
        // it to the redo stack unapplied, silently skipping it (stage 3.7).
        if (!weCharge(we, s, (long long)stack.back().size())) return;
        std::vector<ewb::WeEdit> batch;
        if (undo) we.hist.take_undo(batch); else we.hist.take_redo(batch);
        // Undo replays the batch backwards. A cell nobody had touched before the
        // edit was recorded as its natural block (weRead), so that is what comes
        // back — written as an explicit block and relayed as a build, which every
        // client applies. It is not restored by erasing the entry: `erase` means
        // "fall back to base terrain" in the model but there is no wire message
        // that says that, so it would desync every client that saw the edit —
        // plan §0.5.7 defect 4, in reverse. (Before 3.7 G the record said air.)
        const std::vector<ewb::WeEdit> apply = undo ? ewb::we_invert(batch) : batch;
        std::vector<ewb::WeEdit> want;
        want.reserve(apply.size());
        for (const ewb::WeEdit& e : apply)
            want.push_back({e.x, e.y, e.z, 0, 0, e.newType, e.newColor});
        const auto done = weCommit(want, s, we.zoneSkip);
        if (!done.empty())
            auditLog("player:" + username, verb + ": " + std::to_string(done.size()) + " cell(s)");
        drainSignRemovals();   // an undone build or a redone mine can remove signs
        weZoneReport(we, s, username, verb);
        weSay(s, std::string(undo ? "Undid " : "Redid ") + std::to_string(done.size()) + " block(s).");
        return;
    }

    // --- //up ----------------------------------------------------------------

    if (verb == "//up") {
        int dist = 0;
        if (!ewb::we_parse_int(tok[1], dist) || dist < 1 || dist >= SV_WORLD_HEIGHT) {
            weSay(s, "Distance must be 1.." + std::to_string(SV_WORLD_HEIGHT - 1) + ".");
            return;
        }
        int fx, fy, fz;
        if (!weFeet(s, fx, fy, fz)) { weSay(s, "The server does not have your position yet."); return; }
        const int destY = fy + dist;                 // where the player's feet end up
        if (!weCellInRange(fx, destY, fz)) { weSay(s, "That would take you outside the world."); return; }
        if (!weCharge(we, s, 1)) return;
        const std::vector<ewb::WeEdit> want = {
            {fx, destY - 1, fz, 0, 0, (unsigned char)SV_WE_PLATFORM_BLOCK, 0}
        };
        auto batch = weCommit(want, s, we.zoneSkip);
        if (!batch.empty()) {
            we.hist.record(std::move(batch));
            editsDirty = true;
            auditLog("player:" + username, "//up: 1 cell at " + std::to_string(fx) + "," +
                                           std::to_string(destY - 1) + "," + std::to_string(fz));
        }
        weTeleport(s, username, (float)fx, (float)(destY + 1), (float)fz);
        weZoneReport(we, s, username, verb);   // a teleport is not an edit; the platform is
        weSay(s, "Whoosh.");
        return;
    }

    weSay(s, "'" + verb + "' is not available on this server.");
}

// `SIGNP:x:y:z:a:b:c:text` from a joined player: the retail client placing or
// editing a sign (shape caught live, LIVE-FINDINGS 2026-09-08). Before this handler
// the line fell through to the unrecognised-line branch and every sign a player
// placed was lost.
//
// The sign goes into its slot — same block, same face (sign_store.h) — replacing
// what was there. The list and the SIGNQ burst change at once; the sidecar follows
// on the next save, like the world.
static void handleSignWrite(SOCKET s, const std::string& who, const std::string& line,
                            ewb::TokenBucket& zoneNotice) {
    ewb::Sign sign;
    if (!ewb::parse_client_signp(line, sign)) {
        if (g_verbose) std::cout << "[Server] malformed SIGNP from " << who << ": "
                                 << line.substr(0, 64) << std::endl;
        return;
    }
    // A sign on a protected block (stage 8.2): not stored, not relayed. If the slot
    // already held a sign, the writer's client is now showing their edit over it, so
    // the original is sent back. A brand-new sign has nothing to send back and no line
    // we know of un-draws it; nobody else ever sees it, and its writer loses it on
    // their next join (LIVE-FINDINGS 8.0 E4).
    if (const ewb::Zone* zn = zoneDenies(zoneCheck(s), sign.x, sign.y, sign.z)) {
        std::string back;
        {
            std::lock_guard<std::mutex> lk(g_signMtx);
            for (const ewb::Sign& cur : g_signs)
                if (ewb::same_sign_slot(cur, sign)) back += ewb::format_signp(cur);
        }
        if (!back.empty()) sendWorldTo(s, back);
        zoneRefused(s, zoneNotice, who, zn->name, "sign", sign.x, sign.y, sign.z);
        return;
    }
    ewb::SignUpsert r;
    {
        std::lock_guard<std::mutex> lk(g_signMtx);
        r = ewb::upsert_sign(g_signs, sign, SV_MAX_SIGNS);
        if (r == ewb::SignUpsert::Added || r == ewb::SignUpsert::Replaced) signsChangedLocked();
    }
    if (r == ewb::SignUpsert::Full) {
        std::cerr << "[Server] sign cap reached (" << SV_MAX_SIGNS << "); refused a sign from "
                  << who << "." << std::endl;
        weSay(s, "This world has reached its sign limit, so that sign was not saved.");
        return;
    }
    if (r == ewb::SignUpsert::Unchanged) return;
    auditLog("player:" + who,
             std::string(r == ewb::SignUpsert::Added ? "sign placed" : "sign edited") + " at " +
                 std::to_string(sign.x) + "," + std::to_string(sign.y) + "," +
                 std::to_string(sign.z) + ": " + sign.text);
    // ⚠️ Relayed in the `server`-sender shape SIGNQ is answered with, which the
    // retail client renders from a join burst. Whether it applies one mid-session
    // is unconfirmed; a peer that ignores it still gets the sign on its next join.
    broadcastWorld(ewb::format_signp(sign), s);
}

void handleClient(SOCKET clientSocket, int clientId, std::string clientIP) {
    char recvBuffer[BUFFER_SIZE];
    std::string username = "Player" + std::to_string(clientId);
    int characterType = 0;
    bool joined = false;          // a successful JOIN gates every verb but JOIN/PING (stage 7.17)
    bool preJoinWarned = false;   // log the first pre-JOIN verb per connection, not each one
    BurstLimiter regionLimiter;   // per-connection REGION pacing
    BurstLimiter signLimiter;     // ...and SIGNQ pacing
    ewb::TokenBucket actionBucket(SV_ACTION_BURST, SV_ACTION_RATE);   // stage 1.7
    bool actionWarned = false;    // log the first refusal per connection, not each one
    ewb::TokenBucket moveBucket(SV_MOVE_BURST, SV_MOVE_RATE);   // stage 7.14: POS/VEL/POSVEL
    bool moveWarned = false;      // log the first refusal per connection, not each one
    ewb::TokenBucket chatBucket(SV_CHAT_BURST, SV_CHAT_RATE);   // stage 7.14: MSG
    ewb::TokenBucket chatThrottleNotice(1.0, 1.0 / 10.0);   // tell the player, but <= 1 per 10 s
    WeSession we;                 // Tier 2 selection / clipboard / undo (stage 3.3)
    ewb::TokenBucket loginAttempts(3.0, 1.0 / 10.0);   // /login: 3 at once, then 1 per 10 s (stage 8.6)
    ewb::TokenBucket signWriteBucket(SV_SIGNP_BURST, SV_SIGNP_RATE);   // player SIGNP writes
    bool signWarned = false;      // log the first sign-write refusal, not each one
    ewb::TokenBucket capNotice(1.0, 1.0 / 30.0);   // "world is full" to this player, <= 1 per 30 s
    ewb::TokenBucket burnTruncNotice(1.0, 1.0 / 30.0);  // "that chain was too big" (stage 7.19)
    ewb::TokenBucket burnTruncLog(1.0, 1.0 / 30.0);     // ...and the operator's copy of it
    ewb::TokenBucket zoneNotice(1.0, 1.0 / 10.0);   // "this area is protected", <= 1 per 10 s (stage 8.2)
    ewb::TokenBucket zoneTruncNotice(1.0, 1.0 / 30.0);  // "too much of that blast to put back"
    // "Invalid POS/VEL/POSVEL/ACTION message" were unconditional and per-malformed-
    // packet, so a garbage-packet flood was an unthrottled journal flood too
    // (stage 7.12). Same shape as capNotice: <= 1 line per 30 s per connection.
    ewb::TokenBucket invalidMsgNotice(1.0, 1.0 / 30.0);

    // Client->server messages are newline-framed: accumulate bytes and process
    // one complete '\n'-terminated line at a time. This makes parsing robust to
    // TCP coalescing (e.g. a POSVEL and an ACTION arriving in one recv()).
    std::string acc;
    bool disconnect = false;
    const std::shared_ptr<ClientOut> myOut = outFor(clientSocket);   // lastRecv / evicted (stage 7.5)

    // Connection-lifecycle timeout (stage 1.10). Without this a peer can open a
    // TCP connection, send nothing, and hold one of SV_MAX_CLIENTS slots until
    // the ~2 h keepalive reaps it. SO_RCVTIMEO makes recv() return EAGAIN after
    // `secs`; the loop below turns that into a drop. Armed to the short handshake
    // window now, relaxed to the idle ceiling once JOIN succeeds (the real client
    // sends PING every 10 s, so a silent joined socket is dead).
    auto setRecvTimeout = [&](int secs) {
        struct timeval tv{ secs, 0 };
        setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    };
    if (g_handshakeTimeout > 0) setRecvTimeout(g_handshakeTimeout);

    while (serverRunning && !disconnect) {
        int bytesReceived = recv(clientSocket, recvBuffer, BUFFER_SIZE - 1, 0);
        if (bytesReceived < 0) {
            // EAGAIN/EWOULDBLOCK here is the SO_RCVTIMEO firing, not a real error:
            // a connection that never sent JOIN, or a joined one gone silent past
            // the idle ceiling. Either way the socket is dead to us.
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (!joined)
                    std::cerr << "[Server] client #" << clientId << " (" << clientIP
                              << ") sent no JOIN within " << g_handshakeTimeout
                              << "s; dropping." << std::endl;
                else if (g_verbose)
                    std::cout << "[Server] " << username << " idle past "
                              << g_idleConnTimeout << "s; dropping." << std::endl;
            }
            break;
        }
        if (bytesReceived == 0) break;   // peer closed
        if (myOut) myOut->lastRecv.store(monoSeconds(), std::memory_order_relaxed);
        acc.append(recvBuffer, bytesReceived);

        // Guard against a client flooding without a newline (unbounded memory).
        if (acc.find('\n') == std::string::npos && acc.size() > SV_MAX_LINE) {
            std::cerr << "[Server] " << username << " sent oversized line; dropping." << std::endl;
            break;
        }

        size_t nlpos;
        while (!disconnect && (nlpos = acc.find('\n')) != std::string::npos) {
            // A rejoin from this address took our name (stage 7.5). Whatever is
            // still buffered belongs to a session that no longer exists; acting on
            // it would relay and remember positions under the new session's name.
            if (myOut && myOut->evicted.load(std::memory_order_relaxed)) { disconnect = true; break; }
            std::string message = acc.substr(0, nlpos);
            acc.erase(0, nlpos + 1);
            if (message.empty()) continue;

            auto parts = parseMessage(message);
            if (parts.empty()) continue;

            const std::string& command = parts[0];

            // --- pre-JOIN admission gate (stage 7.17) ------------------------
            // One check, here, for every verb. `JOIN` is the only authentication
            // a --password server has, and five verbs used to skip it: ACTION
            // (world edits, persisted and relayed), MSG (chat as `Player<n>`),
            // and POS/VEL/POSVEL (a phantom player that never gets a `has left`
            // line, plus a row in the player store). The four verbs that *did*
            // check — REGION, SIGNQ, SIGNP and the `/`-command path — now rely on
            // this gate instead of repeating it; see ewb::verb_allowed_before_join
            // for why the rule is an allow-list.
            //
            // A pre-JOIN line is dropped, not fatal: a client that races its own
            // JOIN loses one packet rather than its session. (Nothing observed in
            // any capture sends anything before JOIN — this is the cautious
            // reading, not a known case.)
            if (!joined && !ewb::verb_allowed_before_join(command)) {
                if (!preJoinWarned) {
                    // Once per connection: this is an auth-bypass attempt worth
                    // seeing without --verbose, but a per-line log would be its own
                    // flood. The verb is untrusted input — sanitise and bound it.
                    std::cerr << "[Server] client #" << clientId << " (" << clientIP
                              << ") sent " << ewb::sanitize_text(command, 16)
                              << " before JOIN; ignored." << std::endl;
                    preJoinWarned = true;
                }
                continue;
            }

            // POSVEL:px:py:pz:vx:vy:vz
            // Dispatch order below is frequency-tuned (stage 7.13: POSVEL is by far
            // the most frequent verb on the wire, then POS/VEL/ACTION/MSG) — do not
            // "tidy" it back into protocol/alphabetical order.
            if (command == "POSVEL" && parts.size() >= 7) {
                // Movement budget (stage 7.14): sized far above a real client's
                // observed rate, so a refusal here means a scripted flood, not a
                // laggy player. Dropped silently — see SV_MOVE_RATE above.
                if (SV_MOVE_RATE > 0.0 && !moveBucket.allow(monoSeconds())) {
                    if (!moveWarned) {
                        std::cerr << "[Server] " << username
                                  << " exceeded the movement rate limit; dropping updates."
                                  << std::endl;
                        moveWarned = true;
                    }
                    continue;
                }
                // Validate at ingest (stage 7.16). `std::stof` used to be the only
                // filter, so `nan`/`inf`/`1e38` reached playerInfoMap, g_playerPos,
                // eden_players.txt and — a restart later — `SPAWN`'s formatter.
                float px, py, pz, vx, vy, vz;
                if (!ewb::parse_move_pos(parts[1], parts[2], parts[3], px, py, pz) ||
                    !ewb::parse_move_vel(parts[4], parts[5], parts[6], vx, vy, vz)) {
                    if (invalidMsgNotice.allow(monoSeconds()))
                        std::cout << "[Server] Invalid POSVEL message from " << username << std::endl;
                    continue;
                }
                {
                    std::lock_guard<std::mutex> lock(clientsMutex);
                    auto pit = playerInfoMap.find(clientSocket);
                    if (pit != playerInfoMap.end()) {
                        auto& info = pit->second;
                        info.posX = px; info.posY = py; info.posZ = pz;
                        info.velX = vx; info.velY = vy; info.velZ = vz;
                    }
                }
                rememberPos(username, px, py, pz);
                // Re-formatted from the parsed floats, not echoed (stage 7.18): the
                // relay's size must not be the sender's to choose.
                std::string broadcastMsg = "POSVEL:" + username + ":" + std::to_string(characterType) + ":" +
                                           ewb::format_move_triplet(px, py, pz) + ":" +
                                           ewb::format_move_triplet(vx, vy, vz) + "\n";
                broadcastMessage(broadcastMsg, clientSocket);
            }
            // POS:x:y:z
            else if (command == "POS" && parts.size() >= 4) {
                if (SV_MOVE_RATE > 0.0 && !moveBucket.allow(monoSeconds())) {
                    if (!moveWarned) {
                        std::cerr << "[Server] " << username
                                  << " exceeded the movement rate limit; dropping updates."
                                  << std::endl;
                        moveWarned = true;
                    }
                    continue;
                }
                float px, py, pz;
                if (!ewb::parse_move_pos(parts[1], parts[2], parts[3], px, py, pz)) {   // stage 7.16
                    if (invalidMsgNotice.allow(monoSeconds()))
                        std::cout << "[Server] Invalid POS message from " << username << std::endl;
                    continue;
                }
                {
                    std::lock_guard<std::mutex> lock(clientsMutex);
                    auto pit = playerInfoMap.find(clientSocket);
                    if (pit != playerInfoMap.end()) {
                        pit->second.posX = px;
                        pit->second.posY = py;
                        pit->second.posZ = pz;
                    }
                }
                rememberPos(username, px, py, pz);
                std::string broadcastMsg = "POS:" + username + ":" + std::to_string(characterType) + ":" +
                                           ewb::format_move_triplet(px, py, pz) + "\n";   // stage 7.18
                broadcastMessage(broadcastMsg, clientSocket);
            }
            // VEL:x:y:z
            else if (command == "VEL" && parts.size() >= 4) {
                if (SV_MOVE_RATE > 0.0 && !moveBucket.allow(monoSeconds())) {
                    if (!moveWarned) {
                        std::cerr << "[Server] " << username
                                  << " exceeded the movement rate limit; dropping updates."
                                  << std::endl;
                        moveWarned = true;
                    }
                    continue;
                }
                float vx, vy, vz;
                if (!ewb::parse_move_vel(parts[1], parts[2], parts[3], vx, vy, vz)) {   // stage 7.16
                    if (invalidMsgNotice.allow(monoSeconds()))
                        std::cout << "[Server] Invalid VEL message from " << username << std::endl;
                    continue;
                }
                {
                    std::lock_guard<std::mutex> lock(clientsMutex);
                    auto pit = playerInfoMap.find(clientSocket);
                    if (pit != playerInfoMap.end()) {
                        pit->second.velX = vx;
                        pit->second.velY = vy;
                        pit->second.velZ = vz;
                    }
                }
                std::string broadcastMsg = "VEL:" + username + ":" + std::to_string(characterType) + ":" +
                                           ewb::format_move_triplet(vx, vy, vz) + "\n";   // stage 7.18
                broadcastMessage(broadcastMsg, clientSocket);
            }
            // ACTION:x:y:z:mode[:typeOrColor]   mode 0=build 1=mine 2=burn 3=paint
            else if (command == "ACTION" && parts.size() >= 5) {
                try {
                    int x = std::stoi(parts[1]);
                    int y = std::stoi(parts[2]);
                    int z = std::stoi(parts[3]);
                    int mode = std::stoi(parts[4]);

                    // Reject out-of-range coordinates so a malformed/malicious
                    // client can't corrupt the model or grow it without bound.
                    // y is a hard world height; x/z are the 24-bit key range.
                    if(y < 0 || y >= SV_WORLD_HEIGHT ||
                       x < 0 || z < 0 || x > 0xFFFFFF || z > 0xFFFFFF){
                        if(g_verbose) std::cout << "[Server] rejected out-of-range ACTION ("
                                                << x << "," << y << "," << z << ") from "
                                                << username << std::endl;
                        continue;
                    }

                    // Validate the payload *at ingest* (stage 1.7). Nothing checked
                    // this before, so any byte could land in Cell::type/Cell::color,
                    // be persisted, and be broadcast verbatim to every peer.
                    // `emit_cell_records` guards the SNAPZ wire; this guards the
                    // model and the relay. In particular a paint of 255 would put the
                    // painted-base sentinel's own value into Cell::color.
                    const int rawExtra = (parts.size() >= 6) ? std::stoi(parts[5]) : 0;
                    if (!ewb::action_extra_valid(mode, rawExtra)) {
                        if (g_verbose) std::cout << "[Server] rejected ACTION mode " << mode
                                                 << " extra " << rawExtra << " from "
                                                 << username << std::endl;
                        continue;
                    }

                    // Per-connection edit budget. A refused edit is dropped, not
                    // queued and not fatal — a laggy burst from a real player should
                    // cost them a block, not their session.
                    const double cost = (mode == 2) ? SV_ACTION_COST_BURN : 1.0;
                    if (SV_ACTION_RATE > 0.0 && !actionBucket.allow(monoSeconds(), cost)) {
                        if (!actionWarned) {
                            std::cerr << "[Server] " << username
                                      << " exceeded the ACTION rate limit; dropping edits."
                                      << std::endl;
                            actionWarned = true;
                        }
                        continue;
                    }

                    std::string broadcastMsg;
                    std::string editSuffix;   // "x:y:z:mode[:extra]" relayed to peers
                    std::string coords = std::to_string(x) + ":" + std::to_string(y) + ":" + std::to_string(z);
                    std::string who = username + ":" + std::to_string(characterType);
                    int extra = 0;            // block type (build) / color (paint)
                    // ⚠️ The peer-relay shape `ACTION:<user>:<type>:x:y:z:mode[:extra]`
                    // is **unconfirmed**: every capture to date was single-client, so
                    // no relay has ever been observed. The `server`-sender form
                    // (`ACTION:server:0:...`) is separately corroborated by the modded
                    // server. A two-client session (stage 1.8 rung 5) settles this.
                    switch (mode) {
                        case 0: { // BUILD
                            extra = rawExtra;
                            editSuffix   = coords + ":0:" + std::to_string(extra);
                            broadcastMsg = "ACTION:" + who + ":" + editSuffix + "\n";
                            if(g_verbose) std::cout << "[" << username << "] BUILD at (" << x << "," << y << "," << z << ") type=" << extra << std::endl;
                            break;
                        }
                        case 1: { // MINE
                            editSuffix   = coords + ":1";
                            broadcastMsg = "ACTION:" + who + ":" + editSuffix + "\n";
                            if(g_verbose) std::cout << "[" << username << "] MINE at (" << x << "," << y << "," << z << ")" << std::endl;
                            break;
                        }
                        case 2: { // BURN
                            editSuffix   = coords + ":2";
                            broadcastMsg = "ACTION:" + who + ":" + editSuffix + "\n";
                            if(g_verbose) std::cout << "[" << username << "] BURN at (" << x << "," << y << "," << z << ")" << std::endl;
                            break;
                        }
                        case 3: { // PAINT
                            extra = rawExtra;
                            editSuffix   = coords + ":3:" + std::to_string(extra);
                            broadcastMsg = "ACTION:" + who + ":" + editSuffix + "\n";
                            if(g_verbose) std::cout << "[" << username << "] PAINT at (" << x << "," << y << "," << z << ") color=" << extra << std::endl;
                            break;
                        }
                        default:
                            std::cout << "[" << username << "] Unknown action mode: " << mode << std::endl;
                            continue;
                    }
                    // Protected zones (stage 8.2). Here — after the edit budget, so a
                    // refused edit still spends its tokens and hammering a protected
                    // wall is paced like any other editing; before simAction, so the
                    // model is never touched. Not relayed: no peer ever sees it. The
                    // sender already drew it, so they get the cell put back.
                    const ZoneCheck zc = zoneCheck(clientSocket);
                    if (const ewb::Zone* zn = zoneDenies(zc, x, y, z)) {
                        static const char* const kVerb[] = {"build", "mine", "burn", "paint"};
                        ewb::ZoneCellSet back;
                        back.add(x, y, z);
                        // A burn on a protected TNT or firework: the sender's client set it
                        // off — and its sphere with it — before asking. Put that sphere back
                        // too. (Links it chained from there are not modelled; see
                        // docs/protocol.md "Protected zones".)
                        if (mode == 2) {
                            Cell c;
                            bool explosive;
                            { std::lock_guard<std::mutex> lk(g_worldMtx);
                              explosive = worldGet(x, y, z, c) && (c.type == SV_TNT || c.type == SV_FIREWORK); }
                            if (explosive)
                                ewb::explode_for_each_blast_cell(x, y, z, 0, SV_WORLD_HEIGHT,
                                    [&](int bx, int by, int bz) {
                                        if (ewb::ws_in_world(bx, by, bz)) back.add(bx, by, bz);
                                    });
                        }
                        scheduleRestore(myOut, back.cells());
                        zoneRefused(clientSocket, zoneNotice, username, zn->name, kVerb[mode], x, y, z);
                        continue;
                    }
                    // A burn anywhere else can still reach into a zone: the chain is
                    // told which zones are within its reach, and collects what it hit.
                    ZoneBlast blast;
                    if (mode == 2)
                        blast.near = zoneCheckNear(zc, x - ewb::EXPLODE_REACH, y - ewb::EXPLODE_REACH,
                                                   z - ewb::EXPLODE_REACH, x + ewb::EXPLODE_REACH,
                                                   y + ewb::EXPLODE_REACH, z + ewb::EXPLODE_REACH);

                    // Simulate the action into the authoritative world model so the
                    // server always has an accurate picture (handles TNT/paint
                    // explosions and burning too).
                    const ewb::ExplodeResult act = simAction(mode, x, y, z, extra,
                                                             blast.near.zones ? &blast : nullptr);
                    const size_t refused = act.refused;
                    // A chain is charged for the blasts it actually ran, over the flat
                    // SV_ACTION_COST_BURN paid upfront (stage 7.19): the upfront cost
                    // can't know how much of the world one BURN would touch, and
                    // without this a player may hold g_worldMtx for a budgeted chain
                    // eight times a second, indefinitely.
                    if (SV_ACTION_RATE > 0.0 && act.explosions > (size_t)SV_ACTION_COST_BURN)
                        actionBucket.charge(monoSeconds(),
                                            (double)act.explosions - SV_ACTION_COST_BURN);
                    // A mine or a blast that took a sign's block took the sign. The client
                    // sends nothing for the sign itself (LIVE-FINDINGS 2026-09-11).
                    drainSignRemovals();
                    if (refused && mode != 2) {
                        // The world is at its cell cap and this edit did not land.
                        // Keep it off the peers — they would draw a block no REGION
                        // will ever send back — and tell the player, whose client
                        // has already drawn it. Otherwise the first anyone hears of
                        // it is the build being gone on their next join.
                        if (mode == 0) {
                            // Take the block back out of the sender's world. The cell
                            // was absent from the model, so as far as the server knows
                            // it is untouched terrain, and a client only builds into
                            // air. ⚠️ The `ACTION:server:0` shape the command relay uses.
                            std::string undo;
                            emitEditWire(undo, x, y, z, SV_AIR, 0);
                            sendWorldTo(clientSocket, undo);
                        }
                        if (capNotice.allow(monoSeconds()))
                            weSay(clientSocket, "This world is full, so that edit was not saved."
                                                " Please tell the server operator.");
                        continue;
                    }
                    // Relay the ORIGINAL action to everyone EXCEPT the sender (who
                    // already applied it locally). Peers re-simulate it themselves;
                    // the model above is what late joiners are snapshotted from.
                    broadcastWorld(std::move(broadcastMsg), clientSocket);
                    // The blast reached into a zone. Every client — the sender and each
                    // peer the relay just reached — runs the whole blast itself, zones
                    // unknown to it, so every one of them gets the protected cells put
                    // back, queued behind the relay. A protected TNT did not go off here
                    // but will on every client, so its sphere is restored as well.
                    if (!blast.hit.empty()) {
                        for (const ewb::RevertCell& t : blast.explosives)
                            ewb::explode_for_each_blast_cell(t.x, t.y, t.z, 0, SV_WORLD_HEIGHT,
                                [&](int bx, int by, int bz) {
                                    if (ewb::ws_in_world(bx, by, bz)) blast.hit.add(bx, by, bz);
                                });
                        scheduleRestore(nullptr, blast.hit.cells());
                        zoneRefused(clientSocket, zoneNotice, username, blast.zone, "blast",
                                    blast.fx, blast.fy, blast.fz);
                        // Past ZONE_BURN_RESTORE_MAX the rest is intact on the server but
                        // not redrawn; a rejoin shows it. Say so rather than leave a crater
                        // that looks real.
                        if (blast.hit.overflow() && zoneTruncNotice.allow(monoSeconds())) {
                            sendLine(clientSocket, "[Server] That explosion hit a protected area. It is"
                                                   " intact, but some of it may look damaged until you rejoin.\n");
                            std::cerr << "[Server] BURN from " << username << " reached zone " << blast.zone
                                      << ": restore capped at " << ewb::ZONE_BURN_RESTORE_MAX
                                      << " cells; the rest is intact on the server but not redrawn." << std::endl;
                        }
                    }
                    // A burn is relayed even when part of its blast was refused or the
                    // chain was truncated: every client simulates the explosion itself
                    // regardless, so the relay is what keeps the *other* players in step
                    // with the sender. What the server keeps can differ — see below.
                    if (refused && capNotice.allow(monoSeconds()))
                        weSay(clientSocket, "This world is full, so part of that explosion was"
                                            " not saved. Please tell the server operator.");
                    // Truncation is not a full world: the chain hit --burn-max-cells,
                    // so the server stopped detonating while the clients did not. The
                    // TNT the server did not reach is still in the saved world and will
                    // be there again on the next join, which is worth saying plainly.
                    if (act.truncated) {
                        if (burnTruncNotice.allow(monoSeconds()))
                            weSay(clientSocket, "That explosion chain was too big to finish;"
                                                " some of the TNT is still there on the server.");
                        if (burnTruncLog.allow(monoSeconds()))
                            std::cerr << "[Server] BURN chain from " << username << " hit --burn-max-cells "
                                      << g_burnMaxCells << " (" << act.explosions << " blasts, "
                                      << act.visits << " cells read); " << act.truncated
                                      << " link(s) dropped. Raise it if players report TNT"
                                         " reappearing." << std::endl;
                    }
                } catch (...) {
                    if (invalidMsgNotice.allow(monoSeconds()))
                        std::cout << "[Server] Invalid ACTION message from " << username << std::endl;
                }
            }
            // MSG:text
            //
            // ⚠️ There is deliberately **no "exit"/"quit" kill-switch** here (stage
            // 1.6). The reference server disconnected anyone who typed either word
            // in chat — a player discussing quitting got kicked for it. Closing the
            // game is how you disconnect; nothing replaces it.
            else if (command == "MSG" && parts.size() >= 2) {
                // Bound the line and strip control bytes: chat is relayed verbatim
                // to every peer, and the username charset rule (which stops `]` from
                // forging `[name (Tn)]` structure) is only half the guard.
                std::string msgContent = ewb::sanitize_text(message.substr(4), SV_MAX_CHAT);
                if (msgContent.empty()) continue;

                // A '/'-prefixed line is a Tier 2 command, not chat (stage 3.3).
                // It is answered on this socket and never relayed: broadcasting
                // it would leak one player's `/msg` text — and their command
                // history — to the whole server. WorldEdit's own `cells`/`cmds`
                // buckets (stage 3.3) already pace commands, so the chat budget
                // below applies only to actual broadcast chat, not commands.
                // (Reaching here at all means JOIN succeeded — stage 7.17's gate.)
                if (msgContent[0] == '/') {
                    // /login first, and before the WorldEdit switch: identity is not
                    // WorldEdit, and this keeps the PIN away from every path that logs.
                    if (msgContent.compare(0, 6, "/login") == 0 &&
                        (msgContent.size() == 6 || msgContent[6] == ' ' || msgContent[6] == '\t')) {
                        handleLogin(clientSocket, username, clientIP, msgContent, loginAttempts);
                        continue;
                    }
                    if (!g_weEnabled) { weSay(clientSocket, "Commands are disabled on this server."); continue; }
                    handleWorldEditLine(clientSocket, username, msgContent, we, regionLimiter);
                    continue;
                }

                // Chat budget (stage 7.14): unlike movement, an over-budget line is a
                // griefing vector, not a lag artifact, so it is told to the sender
                // rather than silently eaten (the notice is itself rate-limited, so
                // a spammer can't turn the notice into its own flood).
                if (SV_CHAT_RATE > 0.0 && !chatBucket.allow(monoSeconds())) {
                    if (chatThrottleNotice.allow(monoSeconds()))
                        weSay(clientSocket, "You're sending chat too fast; that message was dropped.");
                    continue;
                }

                std::string broadcastMsg = "[" + username + " (T" + std::to_string(characterType) + ")] " + msgContent + "\n";
                std::cout << broadcastMsg;
                broadcastMessage(broadcastMsg, clientSocket);
            }
            // JOIN:username:characterType[:password[:clientTag]]
            // The native client sends five fields (`JOIN:Player6835:17:EDEN6:zr`);
            // `clientTag` is a constant `zr` we neither require nor interpret.
            else if (command == "JOIN" && parts.size() >= 3) {
                // One JOIN per connection. A second one would let a client rename
                // itself past the duplicate check below and out from under whatever
                // its first name is rate-limited or logged as.
                if (joined) {
                    if (g_verbose) std::cout << "[Server] duplicate JOIN from " << username
                                             << "; ignored." << std::endl;
                    continue;
                }

                const std::string wanted = parts[1];
                const ewb::NameVerdict verdict = ewb::validate_username(wanted);
                if (verdict != ewb::NameVerdict::Ok) {
                    std::string deny = std::string("[Server] Invalid name (") +
                                       ewb::name_verdict_text(verdict) + ").\n";
                    sendLine(clientSocket, deny);
                    std::cout << "[Server] Rejected client #" << clientId << " ("
                              << ewb::name_verdict_text(verdict) << ")." << std::endl;
                    disconnect = true;
                    continue;
                }

                // Ban check (stage 3.2). IP bans are also enforced at accept()
                // before a thread spawns; a name ban can only be checked here,
                // once we know the name.
                {
                    std::lock_guard<std::mutex> lk(g_banMtx);
                    if (g_bans.banned(wanted, clientIP)) {
                        std::string deny = "[Server] You are banned from this server.\n";
                        sendLine(clientSocket, deny);
                        std::cout << "[Server] Rejected " << wanted << " (banned)." << std::endl;
                        disconnect = true;
                        continue;
                    }
                }

                // ⚠️ charType: accept 0..255 and echo what was stored. The native
                // client sent 17 and the real server's welcome said 0 — either it
                // clamps (reading i) or field 2 is not a plain charType index at all
                // and the welcome's number is profile-restored (reading ii; the
                // modded server's seven-entry charNames[] makes this the likelier
                // one). Plan §1.6 has the capture that settles it. Until then,
                // widening is strictly safer than the old 0..10-or-zero clamp, which
                // silently rewrote a legitimate 17 to 0. Do NOT inherit the modded
                // server's 0..6 narrowing.
                try {
                    characterType = std::stoi(parts[2]);
                    if (characterType < 0 || characterType > 255) characterType = 0;
                } catch (...) {
                    characterType = 0;
                }

                // Password check (if this server is protected).
                std::string providedPw = (parts.size() >= 4) ? parts[3] : "";
                if (!g_password.empty() && !ewb::const_time_eq(providedPw, g_password)) {
                    size_t inWindow = 0;
                    {
                        std::lock_guard<std::mutex> lk(g_authFailMtx);
                        inWindow = g_authFail.record_failure(clientIP, monoSeconds());
                    }
                    std::string deny = "[Server] Wrong password.\n";
                    sendLine(clientSocket, deny);
                    std::cout << "[Server] Rejected " << wanted << " from " << clientIP
                              << " (wrong password";
                    if (inWindow) std::cout << ", " << inWindow << " in window";
                    std::cout << ")." << std::endl;
                    disconnect = true;
                    continue;
                }

                // A name already connected. g_playerPos is keyed by username, so two
                // players sharing one would share (and clobber) one saved-position
                // slot; comparison is exact, matching that key. Stage 7.5 — two ways
                // out instead of a refusal:
                //   * the same address, its old socket silent past --stale-session-secs:
                //     that is this player's own dead session (a dropped mobile link the
                //     server has not noticed yet). Evict it; they keep the name and the
                //     saved position.
                //   * anyone else: the next free `name-2`, `name-3`, ...; the welcome
                //     line already echoes the assigned name.
                std::string assigned;
                {
                    std::lock_guard<std::mutex> lock(clientsMutex);
                    std::set<std::string> taken;
                    SOCKET dupSock = INVALID_SOCKET;
                    std::string dupIp;
                    for (const auto& kv : playerInfoMap) {
                        taken.insert(kv.second.username);
                        if (kv.second.username == wanted) { dupSock = kv.first; dupIp = kv.second.ip; }
                    }
                    if (dupSock != INVALID_SOCKET) {
                        const auto old = outFor(dupSock);
                        const double silent = old ? monoSeconds() - old->lastRecv.load(std::memory_order_relaxed)
                                                  : 1e9;   // no writer left: nothing is listening
                        if (ewb::dup_name_action(dupIp == clientIP, silent, g_staleSessionSecs) ==
                            ewb::DupNameAction::Evict) {
                            // Taken out of the roster here rather than waiting for the old
                            // thread to get there: its disconnect path saves the whole world
                            // first, which on a big world is seconds. The fd stays open until
                            // that thread closes it — it cannot be recycled under us — and
                            // shutdown() is what wakes its recv().
                            if (old) old->evicted.store(true, std::memory_order_relaxed);
                            playerInfoMap.erase(dupSock);
                            taken.erase(wanted);
                            shutdown(dupSock, SHUT_RDWR);
                            std::cout << "[Server] Evicted the stale session of " << wanted << " ("
                                      << clientIP << ", silent " << static_cast<int>(silent)
                                      << "s); the rejoin keeps the name." << std::endl;
                        }
                    }
                    assigned = ewb::next_free_username(wanted, taken);
                    username = assigned;
                    playerInfoMap[clientSocket] =
                        PlayerInfo{clientSocket, username, characterType, 0, 0, 0, 0, 0, 0, clientIP};
                }
                const bool renamed = (assigned != wanted);
                if (renamed)
                    std::cout << "[Server] " << wanted << " is in use; admitted " << clientIP
                              << " as " << assigned << "." << std::endl;
                joined = true;
                t_editActor = "player:" + username;   // who the audit names when this player's edit removes a sign

                // Handshake complete — relax the read timeout from the short
                // pre-JOIN window to the post-JOIN idle ceiling (or clear it if
                // --idle-timeout-conn is disabled). Stage 1.10.
                if (g_handshakeTimeout > 0)
                    setRecvTimeout(g_idleConnTimeout > 0 ? g_idleConnTimeout : 0);

                // --- join sequence, in the native pcap's order (stage 1.3) --------
                // 1. welcome
                nameOut(clientSocket, username);   // the writer's log lines get a name
                std::string welcome = "[Server] Welcome, " + username + "! (Character Type: " + std::to_string(characterType) + ")\n";
                sendLine(clientSocket, welcome);
                if (renamed)   // ordinary chat, so the player learns why their name changed
                    sendLine(clientSocket, "[Server] The name " + wanted + " is already in use; you are " +
                                           username + ".\n");

                // 2. the operator's welcome message, if this world has one
                //    (eden_motd.txt / --motd-file). Ordinary `[Server]` chat
                //    lines, so no client has to learn anything new; a world with
                //    no MOTD sends nothing and the sequence is unchanged.
                {
                    std::vector<std::string> motd;
                    { std::lock_guard<std::mutex> lk(g_motdMtx); motd = g_motdLines; }
                    for (const std::string& l : motd) sendLine(clientSocket, l);
                }
                // A PIN-protected name (stage 8.6): say how to claim it. Ordinary chat,
                // like the MOTD, so the join sequence's wire shape is unchanged.
                if (nameHasPin(username))
                    sendLine(clientSocket, "[Server] The name " + username + " is protected. Type "
                                           "/login <pin> to use its permissions.\n");

                // 3. capability advertisement. This is what tells the client to ask
                //    for terrain with REGION instead of expecting a push; the reference
                //    test client will not send a REGION until it sees this line.
                sendLine(clientSocket, "CAPS:region\n");

                // 4. SPAWN — this name's saved position if it has one, otherwise
                //    the world's default spawn (eden_spawn.txt / --spawn, stage
                //    5.3). A returning player's own row always wins.
                {
                    std::lock_guard<std::mutex> lk(g_posMtx);
                    const SavedPos* sp = g_playerPos.find(username);
                    if (sp) {
                        sendLine(clientSocket, spawnLine(sp->x, sp->y, sp->z));
                        std::cout << "[Server] Restored " << username << " to ("
                                  << sp->x << "," << sp->y << "," << sp->z << ")\n";
                    } else if (g_haveWorldSpawn) {
                        sendLine(clientSocket, spawnLine(g_worldSpawn.x, g_worldSpawn.y, g_worldSpawn.z));
                        std::cout << "[Server] Spawned " << username << " at world spawn ("
                                  << g_worldSpawn.x << "," << g_worldSpawn.y << "," << g_worldSpawn.z << ")\n";
                    }
                }

                // SIGNP and SNAPZ follow — but as *answers* to the client's own
                // SIGNQ and REGION, which are already queued behind this JOIN on the
                // same TCP stream. Nothing is pushed here.
                if (g_legacySnapshot) sendWorldSnapshot(clientSocket);

                std::string joinMsg = "[Server] " + username + " (Type " + std::to_string(characterType) + ") has joined.\n";
                std::cout << joinMsg;
                broadcastMessage(joinMsg, clientSocket);
            }
            // REGION:x:z — the client asks for the world around a point; we answer
            // with a SNAPZ burst. See serveRegion().
            else if (command == "REGION" && parts.size() >= 3) {
                // ⚠️ Gated on JOIN — by the stage 7.17 gate at the top of the
                // dispatch, not here. Without it an unauthenticated peer on a
                // passworded server could pull the entire world without ever
                // supplying the password, and REGION is the amplification vector.
                try {
                    serveRegion(clientSocket, username,
                                std::stoi(parts[1]), std::stoi(parts[2]), regionLimiter);
                } catch (const std::exception&) {
                    // stoi threw: a malformed point. Say nothing, cost nothing.
                    if (g_verbose) std::cout << "[Server] Invalid REGION message from "
                                             << username << std::endl;
                }
            }
            // SIGNQ — the client asks for the world's signs; we answer with a burst
            // of SIGNP lines and no terminator. See serveSigns() / sign_store.h.
            else if (command == "SIGNQ") {
                // Gated on JOIN (stage 7.17's gate) for the same reason REGION is:
                // a 6-byte request answered with the entire sign file is an
                // amplification vector, and on a passworded server it would leak
                // world content to a peer that never supplied the password.
                serveSigns(clientSocket, username, signLimiter);
            }
            // SIGNP:x:y:z:a:b:c:text — a player placed or edited a sign. Gated on JOIN
            // by stage 7.17's gate (a sign is world content, and a passworded server
            // must not take it from a peer that never authenticated) and paced per
            // connection, since each write rebuilds the burst every SIGNQ is
            // answered from.
            else if (command == "SIGNP") {
                if (!signWriteBucket.allow(monoSeconds())) {
                    if (!signWarned) {
                        std::cerr << "[Server] " << username
                                  << " exceeded the sign write rate limit; dropping sign writes."
                                  << std::endl;
                        signWarned = true;
                    }
                    continue;
                }
                handleSignWrite(clientSocket, username, message, zoneNotice);
            }
            // PING -> PONG. Bare line, bare reply, no arguments echoed — this is what
            // the capture shows and it is what gives a client a real RTT.
            // ⚠️ Not the same thing as the *outbound* matchmaker `PING:<count>`
            // heartbeat in matchmakerThread(). Deliberately unlogged even under
            // --verbose: it is per-client and frequent (the reference test client
            // sends one every 10 s), and a log line per ping drowns everything else.
            else if (command == "PING") {
                // Short enough to live in the std::string's own storage, so the
                // per-ping cost is the same as the deleted (SOCKET, char*, size_t)
                // overload's was.
                sendLine(clientSocket, "PONG\n");
            }
            // Anything else — log it once per verb so a real client's un-modelled
            // messages surface instead of being silently swallowed by this else-if
            // chain. (This is how the client's `SIGNP` sign write was found.)
            // Only reachable from a joined player since stage 7.17: an unknown verb
            // from a peer that never handshook is dropped by the gate above, which
            // keeps `seen` (a set keyed on untrusted bytes, capped by stage 7.24) out
            // of an anonymous peer's reach.
            else if (g_verbose && !command.empty()) {
                static std::mutex seenMtx;
                static std::set<std::string> seen;
                std::lock_guard<std::mutex> lk(seenMtx);
                // Both ends bounded (stage 7.24): the key is attacker-chosen bytes and
                // a diagnostic needs only a verb's prefix, and a few hundred distinct
                // verbs is well past its usefulness. Past the cap it goes quiet.
                static const size_t SEEN_MAX = 256, SEEN_KEY_MAX = 16;
                if (seen.size() < SEEN_MAX && seen.insert(command.substr(0, SEEN_KEY_MAX)).second) {
                    std::cout << "[Server] unrecognised line from " << username
                              << ": " << message.substr(0, 64)
                              << (message.size() > 64 ? "..." : "") << std::endl;
                }
            }
        }   // end inner while (per-line processing)
    }       // end outer while (recv loop)

    // Only announce a departure for someone who actually arrived. A connection
    // refused at JOIN (bad password, bad or duplicate name) or one that never sent a
    // JOIN at all never appeared in anyone's roster, and broadcasting a leave line
    // for it both confuses clients and leaks the attempted name to every player.
    // An evicted session (stage 7.5) is silent: the same player is already back under
    // this name, and a "has left" now would tell every client they had gone.
    const bool evicted = myOut && myOut->evicted.load(std::memory_order_relaxed);
    if (joined && evicted) {
        std::cout << "[Server] " << username << " (stale session) closed." << std::endl;
    } else if (joined) {
        std::cout << "[Server] " << username << " disconnected." << std::endl;
        {   // /r state is keyed by the recipient: drop it with them (stage 7.23)
            std::lock_guard<std::mutex> lk(g_whisperMtx);
            g_lastWhisper.erase(username);
        }
        std::string leaveMsg = "[Server] " + username + " has left.\n";
        broadcastMessage(leaveMsg, clientSocket);
    } else if (g_verbose) {
        std::cout << "[Server] client #" << clientId << " closed without joining." << std::endl;
    }

    savePlayerPos();   // persist positions when someone leaves
    saveWorld();       // and persist the world model
    saveSigns();       // and any signs placed this session
    removeClient(clientSocket);   // out of the roster: no new broadcast finds us
    // Then flush and retire the writer, and only then close the fd. The writer
    // never closes it, so a recycled descriptor can never be written by a thread
    // that thinks it still belongs to this player (stage 7.3).
    closeOut(clientSocket);
    close(clientSocket);
}

int main(int argc, char* argv[]) {
    std::cout << std::unitbuf;   // auto-flush so logs appear live under systemd/journald
    int port = DEFAULT_PORT;

    // Args: [port] and/or flags:
    //   --port N  --name "My World"  --password PASS | --password-file FILE  --world FILE  --signs FILE
    //   --world-format edmb|text   (what a save writes; loading accepts both)
    //   --spawn x:y:z  --spawn-file FILE  --motd-file FILE  --players-file FILE
    //   --max-world-cells N  --max-saved-positions N
    //   --matchmaker HOST[:PORT]
    //   --region-radius N  --no-region-sort  --no-region-empty-frame
    //   --action-rate N  --action-burst N   (0 = unlimited)
    //   --burn-max-cells N  (cells one TNT chain may read before it is truncated)
    //   --move-rate N  --move-burst N   --chat-rate N  --chat-burst N   (0 = unlimited)
    //   --legacy-snapshot  --connect-limit N  --tcp-nodelay 0|1  (1 = default, disables Nagle)
    //   --auth-fail-limit N  --handshake-timeout N  --idle-timeout-conn N  (0 = off)
    //   --stale-session-secs N  (0 = never evict a same-address duplicate name)
    //   --control-rate N  --control-burst N  --control-max-conns N  --audit-file FILE
    //   --zones-file FILE  --zone-revert-delay-ms N  (0 = restore refused edits at once)
    //   --auth-file FILE   (login PINs; default <worlddir>/eden_auth.txt)
    // A bare leading number is still accepted as the port (back-compat).
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* def)->std::string{ return (i+1<argc) ? std::string(argv[++i]) : std::string(def); };
        if      (a == "--port")       port = std::atoi(next("27015").c_str());
        else if (a == "--name")       g_serverName = next("Eden Server");
        else if (a == "--password") {
            const bool hasValue = i + 1 < argc;
            g_password = next("");
            // Blank the secret in argv so /proc/<pid>/cmdline and `ps` stop showing it. This
            // narrows the window (the process was already exec'd with it) but does not close it:
            // prefer --password-file or EDEN_PASSWORD (stage 7.28). The length of the argv slot
            // is unchanged, so a same-length blank leaks only how long the password was.
            if (hasValue) std::memset(argv[i], ' ', std::strlen(argv[i]));
        }
        else if (a == "--password-file") g_passwordFile = next("");
        else if (a == "--world")      g_worldFile  = next("eden_world.model");
        else if (a == "--world-format") {        // stage 7.6: edmb (default) | text
            const std::string v = next("edmb");
            if (v == "text") g_saveText = true;
            else if (v == "edmb") g_saveText = false;
            else { std::cerr << "[Server] --world-format must be 'edmb' or 'text'" << std::endl; return 1; }
        }
        else if (a == "--signs")      g_signFile   = next("eden_signs.txt");
        else if (a == "--spawn-file") g_spawnFile  = next("eden_spawn.txt");
        else if (a == "--motd-file")  g_motdFile   = next("eden_motd.txt");
        else if (a == "--players-file") g_posFile  = next("eden_players.txt");
        else if (a == "--spawn") {
            // Inline default spawn; overrides (and skips) eden_spawn.txt.
            const std::string v = next("x:y:z");
            ewb::Spawn s;
            if (!ewb::parse_spawn_line(v, s))
                std::cerr << "[Server] --spawn " << v << " is not x:y:z; ignored." << std::endl;
            else if (!ewb::move_pos_valid(s.x, s.y, s.z))   // same bound as a player's own position (7.16)
                std::cerr << "[Server] --spawn " << v << " is outside the world; ignored." << std::endl;
            else { g_worldSpawn = s; g_haveWorldSpawn = true; }
        }
        else if (a == "--max-saved-positions") {
            const long long v = std::atoll(next("10000").c_str());
            g_maxSavedPos = v >= 1 ? (size_t)v : 0;   // 0 -> reset with a warning below
        }
        else if (a == "--max-world-cells") {
            const long long v = std::atoll(next("4000000").c_str());
            g_maxWorldCells = v >= 1 ? (size_t)v : 0;   // 0 -> reset with a warning below
        }
        else if (a == "--verbose")    g_verbose    = true;
        // Push the plaintext ACTION world dump on JOIN (pre-1.3 behaviour). Off by
        // default: the real client asks for terrain with REGION and is answered with
        // SNAPZ, and this dump's wire shape has never been captured. Kept as a
        // bring-up fallback — see sendWorldSnapshot().
        else if (a == "--legacy-snapshot") g_legacySnapshot = true;
        else if (a == "--connect-limit") g_connectLimit = std::atoi(next("10").c_str());
        else if (a == "--tcp-nodelay") g_tcpNodelay = std::atoi(next("1").c_str()) != 0;
        // Connection-lifecycle hardening (stage 1.10).
        else if (a == "--auth-fail-limit")   g_authFailLimit    = std::atoi(next("5").c_str());
        else if (a == "--handshake-timeout") g_handshakeTimeout = std::atoi(next("15").c_str());
        else if (a == "--idle-timeout-conn") g_idleConnTimeout  = std::atoi(next("300").c_str());
        else if (a == "--stale-session-secs") g_staleSessionSecs = std::atoi(next("15").c_str());
        else if (a == "--idle-timeout") g_idleTimeout = std::atoi(next("0").c_str());
        // Tier 1 operator control socket (stage 3.2). Defaults on, at
        // <worlddir>/edenserver.sock; --control-socket overrides the path,
        // --no-control-socket disables it entirely.
        else if (a == "--control-socket")    g_controlSocket  = next("");
        else if (a == "--no-control-socket") g_controlEnabled = false;
        else if (a == "--ban-file")          g_banFile        = next("eden_bans.txt");
        else if (a == "--ops-file")          g_opsFile        = next("eden_ops.txt");
        else if (a == "--default-level")     g_defaultLevel   = std::atoi(next("0").c_str());
        // Control-socket flood guard (stage 3.4).
        else if (a == "--control-rate")      g_ctlCmdRate     = std::atof(next("16").c_str());
        else if (a == "--control-burst")     g_ctlCmdBurst    = std::atof(next("64").c_str());
        else if (a == "--control-max-conns") g_ctlMaxConns    = std::atoi(next("8").c_str());
        // Audit log (stage 3.4). stdout always; this is an extra copy.
        else if (a == "--audit-file")        g_auditFile      = next("");
        // Tier 2 player command surface (stage 3.3). On by default; the bounds
        // exist so an operator can tighten them, not so they can be removed —
        // --we-rate 0 disables the cell budget but the per-command cap stands.
        else if (a == "--no-worldedit")      g_weEnabled      = false;
        else if (a == "--we-max-cells")      g_weMaxCells     = std::atoll(next("131072").c_str());
        else if (a == "--burn-max-cells")    g_burnMaxCells   = (size_t)std::atoll(next("1048576").c_str());
        // Protected zones (stage 8.2).
        else if (a == "--zones-file")        g_zonesFile      = next("");
        // Player identity (stage 8.6).
        else if (a == "--auth-file")         g_authFile       = next("");
        else if (a == "--zone-revert-delay-ms") g_zoneRevertDelayMs = std::atoi(next("1000").c_str());
        else if (a == "--we-undo-budget")    g_weUndoBudget   = (size_t)std::atoll(next("2097152").c_str());
        else if (a == "--we-rate")           g_weCellRate     = std::atof(next("32768").c_str());
        else if (a == "--we-burst")          g_weCellBurst    = std::atof(next("262144").c_str());
        // REGION tuning. --region-radius exists because hosting our own server is
        // the only place the derived R = 224 can be varied experimentally without
        // burning someone else's CPU (plan §1.1); leave it alone for normal hosting,
        // since the reference test client's coverage lattice is built around 224.
        else if (a == "--region-radius") g_regionRadius = std::atoi(next("224").c_str());
        else if (a == "--no-region-sort") g_regionSort = false;
        // Empty regions are answered with a well-formed `SNAPZ:0:` by default (1.8
        // rung 2/3: the reference test client counts a real frame as "answered" and resets its
        // consecutive-empty abort; with silence it hangs on "waiting for the world
        // snapshot" and a 9-region sweep aborts three points in). --no-region-empty-frame
        // restores pre-1.8 silence for A/B testing against the real client (1.9).
        else if (a == "--region-empty-frame") g_regionEmptyFrame = true;   // back-compat no-op (now the default)
        else if (a == "--no-region-empty-frame") g_regionEmptyFrame = false;
        // Per-client output queue bounds (stage 7.3). Defaults are sized in
        // out_queue.h's header comment and docs/configuration.md; an operator on a
        // small VPS mostly wants --client-world-max and --region-pending-records.
        else if (a == "--client-outbox-max")     g_outboxMax     = (size_t)std::atoll(next("1048576").c_str());
        else if (a == "--client-world-max")      g_worldboxMax   = (size_t)std::atoll(next("16777216").c_str());
        else if (a == "--client-region-queue")   g_regionQueue   = (size_t)std::atoll(next("2").c_str());
        else if (a == "--region-pending-records") g_regionPending = (uint64_t)std::atoll(next("16000000").c_str());
        else if (a == "--client-write-timeout")  g_writeTimeout  = std::atoi(next("60").c_str());
        else if (a == "--action-rate")  SV_ACTION_RATE  = std::atof(next("512").c_str());
        else if (a == "--action-burst") SV_ACTION_BURST = std::atof(next("1024").c_str());
        else if (a == "--move-rate")    SV_MOVE_RATE    = std::atof(next("40").c_str());
        else if (a == "--move-burst")   SV_MOVE_BURST   = std::atof(next("80").c_str());
        else if (a == "--chat-rate")    SV_CHAT_RATE    = std::atof(next("0.5").c_str());
        else if (a == "--chat-burst")   SV_CHAT_BURST   = std::atof(next("5").c_str());
        else if (a == "--matchmaker") {
            std::string mm = next("");
            size_t c = mm.find(':');
            if (c != std::string::npos) { g_matchHost = mm.substr(0,c); g_matchPort = std::atoi(mm.substr(c+1).c_str()); }
            else                        { g_matchHost = mm; }
        }
        else if (a == "--advertise")  g_advertiseIP = next("");   // IP clients use to reach us
        else if (!a.empty() && a[0] != '-') port = std::atoi(a.c_str());  // bare port
    }
    g_port = port;

    // Resolve the world password (stage 7.28): --password, then --password-file, then the
    // EDEN_PASSWORD environment variable. A file the operator asked for that cannot be read,
    // or holds nothing, is a hard error: carrying on would silently start an OPEN server.
    {
        std::string filePw;
        const bool wantFile = !g_passwordFile.empty();
        if (wantFile) {
            struct stat st{};
            std::ifstream pf(g_passwordFile, std::ios::binary);
            if (!pf || stat(g_passwordFile.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
                std::cerr << "[Server] --password-file " << g_passwordFile
                          << ": cannot read it (or it is not a regular file). Refusing to start an open server."
                          << std::endl;
                return 2;
            }
            if (st.st_mode & (S_IRWXG | S_IRWXO))
                std::cerr << "[Server] warning: --password-file " << g_passwordFile
                          << " is readable by other users; chmod 600 it." << std::endl;
            char pwbuf[4096];                            // only the first line is used; bound the read
            pf.read(pwbuf, sizeof pwbuf);
            filePw = ewb::password_from_file_text(std::string(pwbuf, (size_t)pf.gcount()));
            if (filePw.empty()) {
                std::cerr << "[Server] --password-file " << g_passwordFile
                          << " is empty. Refusing to start an open server (omit the flag for one)."
                          << std::endl;
                return 2;
            }
        }
        const ewb::PasswordChoice pc =
            ewb::choose_password(g_password, wantFile ? &filePw : nullptr, std::getenv("EDEN_PASSWORD"));
        g_password = pc.value;
        switch (pc.source) {
            case ewb::PasswordSource::Argv:
                std::cout << "[Server] Password: from --password (visible in the process list; "
                             "prefer --password-file or EDEN_PASSWORD)." << std::endl; break;
            case ewb::PasswordSource::File:
                std::cout << "[Server] Password: from --password-file." << std::endl; break;
            case ewb::PasswordSource::Env:
                std::cout << "[Server] Password: from EDEN_PASSWORD." << std::endl; break;
            case ewb::PasswordSource::None: break;
        }
    }

    // Clamp an operator typo before it becomes a wrapped box or a whole-world scan.
    if (g_regionRadius < 16 || g_regionRadius > 4096) {
        std::cerr << "[Server] --region-radius " << g_regionRadius
                  << " out of range (16..4096); using " << ewb::REGION_RADIUS << "." << std::endl;
        g_regionRadius = ewb::REGION_RADIUS;
    }
    if (g_regionRadius != ewb::REGION_RADIUS)
        std::cout << "[Server] REGION radius " << g_regionRadius
                  << " (non-default — the reference test client's coverage lattice assumes "
                  << ewb::REGION_RADIUS << ")" << std::endl;

    // Runtime edited-cell ceiling (stage 5.3). A value below 1 is a typo, not a
    // configuration — reset it. There is no hard upper bound: an operator hosting
    // a world eden_import only passed with a raised --max-world-cells has to be
    // able to match that number here, and their RAM is the real limit.
    if (g_maxWorldCells < 1) {
        std::cerr << "[Server] --max-world-cells must be >= 1; using "
                  << SV_MAX_WORLD_CELLS_DEFAULT << "." << std::endl;
        g_maxWorldCells = SV_MAX_WORLD_CELLS_DEFAULT;
    }
    // Saved-position table (stage 7.23): same shape — below 1 is a typo. Applied before
    // loadPlayerPos() so an oversized file is trimmed to the newest rows on load.
    if (g_maxSavedPos < 1) {
        std::cerr << "[Server] --max-saved-positions must be >= 1; using "
                  << SV_MAX_SAVED_POS_DEFAULT << "." << std::endl;
        g_maxSavedPos = SV_MAX_SAVED_POS_DEFAULT;
    }
    g_playerPos.set_capacity(g_maxSavedPos);
    if (g_maxSavedPos != SV_MAX_SAVED_POS_DEFAULT)
        std::cout << "[Server] Saved-position cap " << g_maxSavedPos
                  << " (default " << SV_MAX_SAVED_POS_DEFAULT << ")" << std::endl;
    if (g_maxWorldCells != SV_MAX_WORLD_CELLS_DEFAULT)
        std::cout << "[Server] Edited-cell cap " << g_maxWorldCells
                  << " (default " << SV_MAX_WORLD_CELLS_DEFAULT << ")" << std::endl;

    // Per-client output bounds (stage 7.3). A zero here is not a configuration:
    // it would make every line an instant overflow, i.e. disconnect-on-first-word.
    // --client-write-timeout 0 *is* a configuration ("wait forever"), and
    // --region-pending-records 0 disables the global backlog guard.
    if (g_outboxMax < 64u * 1024u) {
        std::cerr << "[Server] --client-outbox-max " << g_outboxMax
                  << " is too small to hold a burst of movement updates; using 65536." << std::endl;
        g_outboxMax = 64u * 1024u;
    }
    if (g_regionQueue < 1) g_regionQueue = 1;
    if (g_writeTimeout < 0) g_writeTimeout = 0;

    if (!ewb::ctl_level_valid(g_defaultLevel)) {
        std::cerr << "[Server] --default-level " << g_defaultLevel << " out of range (0..2); using 0." << std::endl;
        g_defaultLevel = 0;
    }

    // Tier 2 bounds. A cap of 0 would make every command a no-op and a negative
    // one would make the volume check pass everything, so clamp rather than
    // trust the command line. The ceiling is g_maxWorldCells: no single
    // command may be allowed to fill the whole world.
    if (g_weMaxCells < 1 || g_weMaxCells > (long long)g_maxWorldCells) {
        std::cerr << "[Server] --we-max-cells " << g_weMaxCells << " out of range (1.."
                  << g_maxWorldCells << "); using "
                  << std::min<long long>(ewb::WE_MAX_EDIT_CELLS, (long long)g_maxWorldCells)
                  << "." << std::endl;
        g_weMaxCells = std::min<long long>(ewb::WE_MAX_EDIT_CELLS, (long long)g_maxWorldCells);
    }
    // An undo budget below one full-cap batch would evict a player's newest edit
    // the moment they made it, which reads as "undo is broken" rather than as a
    // tuning choice.
    const size_t weMinBudget = (size_t)g_weMaxCells * sizeof(ewb::WeEdit);
    if (g_weUndoBudget < weMinBudget) {
        std::cerr << "[Server] --we-undo-budget raised to " << weMinBudget
                  << " B, one full --we-max-cells batch." << std::endl;
        g_weUndoBudget = weMinBudget;
    }
    if (g_weCellRate < 0.0) g_weCellRate = 0.0;
    if (g_weCellBurst < (double)g_weMaxCells) g_weCellBurst = (double)g_weMaxCells;

    // Tier 1's `fill` cap is derived from Tier 2's, not typed in separately
    // (stage 3.4): they bound the same cost — one pass over the world holding
    // g_worldMtx — and having had two independently-chosen numbers for it was
    // how they drifted apart in the first place.
    g_ctlFillCap = ewb::ctl_fill_cap(g_weMaxCells, (long long)g_maxWorldCells);

    {
        // The memory an all-clients-stalled worst case implies. Not a limit — the
        // operator's RAM is — but it is the number that turns "why did the box run
        // out of memory" into something visible at startup, in the style of the
        // cell-cap warning above. The threshold is deliberately well above the
        // default config (~1.1 GB at 64 slots, and only if all 64 stall at once):
        // this fires for someone who has *raised* a limit, not on every start.
        const size_t perClient = g_outboxMax + g_worldboxMax + (size_t)g_ctlFillCap * 105u;
        const double outboxGB  = (double)perClient * SV_MAX_CLIENTS / 1073741824.0;
        const double regionGB  = (double)g_regionPending * sizeof(ewb::SnapRec) / 1073741824.0;
        if (outboxGB + regionGB > 4.0)
            std::cerr << "[Server] note: with every one of " << SV_MAX_CLIENTS
                      << " clients stalled at once the output queues could hold up to "
                      << outboxGB << " GB, plus " << regionGB
                      << " GB of queued REGION records. Lower --client-world-max or"
                         " --region-pending-records if that is more than this host has."
                      << std::endl;
    }

    // Control-socket flood guard. A rate of 0 turns the pacing off (an operator
    // running a bulk import through edenctl may want that); the concurrent
    // connection cap has no off switch, because "unlimited threads" is not a
    // configuration anyone needs.
    if (g_ctlCmdRate < 0.0) g_ctlCmdRate = 0.0;
    if (g_ctlCmdBurst < 1.0) g_ctlCmdBurst = 1.0;
    if (g_ctlMaxConns < 1) g_ctlMaxConns = 1;

    // Default the control socket to <worlddir>/edenserver.sock — the same
    // directory the world/player/sign files live in (systemd's WorkingDirectory).
    if (g_controlSocket.empty()) {
        const size_t slash = g_worldFile.find_last_of('/');
        g_controlSocket = (slash == std::string::npos)
                              ? std::string("edenserver.sock")
                              : g_worldFile.substr(0, slash + 1) + "edenserver.sock";
    }

    // Default the world spawn sidecar to <worlddir>/eden_spawn.txt, the file
    // eden_import writes there (stage 5.3). --spawn-file overrides the path;
    // --spawn already set g_haveWorldSpawn and loadSpawn() will skip the file.
    if (g_spawnFile.empty()) {
        const size_t slash = g_worldFile.find_last_of('/');
        g_spawnFile = (slash == std::string::npos)
                          ? std::string("eden_spawn.txt")
                          : g_worldFile.substr(0, slash + 1) + "eden_spawn.txt";
    }

    // Default the welcome message to <worlddir>/eden_motd.txt, so a world in its
    // own directory picks up its own MOTD with no extra flag (the same rule the
    // spawn sidecar above follows). --motd-file overrides the path.
    if (g_motdFile.empty()) {
        const size_t slash = g_worldFile.find_last_of('/');
        g_motdFile = (slash == std::string::npos)
                         ? std::string("eden_motd.txt")
                         : g_worldFile.substr(0, slash + 1) + "eden_motd.txt";
    }

    // Protected zones (stage 8.2): <worlddir>/eden_zones.txt, the same rule again.
    if (g_zonesFile.empty()) {
        const size_t slash = g_worldFile.find_last_of('/');
        g_zonesFile = (slash == std::string::npos)
                          ? std::string("eden_zones.txt")
                          : g_worldFile.substr(0, slash + 1) + "eden_zones.txt";
    }
    // Login PINs (stage 8.6): <worlddir>/eden_auth.txt, the same rule.
    if (g_authFile.empty()) {
        const size_t slash = g_worldFile.find_last_of('/');
        g_authFile = (slash == std::string::npos)
                         ? std::string("eden_auth.txt")
                         : g_worldFile.substr(0, slash + 1) + "eden_auth.txt";
    }
    if (g_zoneRevertDelayMs < 0 || g_zoneRevertDelayMs > ewb::ZONE_REVERT_MAX_DELAY_MS) {
        std::cerr << "[Server] --zone-revert-delay-ms " << g_zoneRevertDelayMs << " out of range (0.."
                  << ewb::ZONE_REVERT_MAX_DELAY_MS << "); using " << ewb::ZONE_REVERT_DEFAULT_DELAY_MS
                  << "." << std::endl;
        g_zoneRevertDelayMs = ewb::ZONE_REVERT_DEFAULT_DELAY_MS;
    }

    // Default the player-position file to <worlddir>/eden_players.txt, the same
    // directory the world/spawn files live in. 5.3 did this for the spawn sidecar
    // and missed this one, so every world hosted from one cwd shared a single
    // ./eden_players.txt — a stale row from one world silently teleported a
    // returning player into an unrelated one (LIVE-FINDINGS 5.4 #6, stage 7.15).
    // --players-file overrides outright. Otherwise: if a legacy ./eden_players.txt
    // already exists and the derived path is a different file, keep reading the
    // legacy one so nobody's saved position vanishes on upgrade.
    if (g_posFile.empty()) {
        const size_t slash = g_worldFile.find_last_of('/');
        g_posFile = (slash == std::string::npos)
                        ? std::string("eden_players.txt")
                        : g_worldFile.substr(0, slash + 1) + "eden_players.txt";
        if (g_posFile != "eden_players.txt") {
            std::ifstream legacy("eden_players.txt");
            if (legacy.good()) g_posFile = "eden_players.txt";
        }
    }
    std::cout << "[Server] Player positions: " << g_posFile << std::endl;

    // If we're registering with a matchmaker but weren't told which IP to
    // advertise, auto-detect this machine's LAN address so remote devices get a
    // reachable address instead of the matchmaker's view of our peer IP.
    if (!g_matchHost.empty() && g_advertiseIP.empty()) {
        g_advertiseIP = detectLanIP();
        if (!g_advertiseIP.empty())
            std::cout << "[Server] Advertising LAN address " << g_advertiseIP << ":" << g_port << std::endl;
    }

    // SIGPIPE fires when writing to a socket the peer already closed; ignore it
    // so a disconnecting client can't take the whole server down.
    signal(SIGPIPE, SIG_IGN);

    // SIGTERM/SIGINT (systemctl stop, docker stop, Ctrl-C) used to default-terminate
    // the process, losing everything since the last 15s autosave tick (stage 7.9).
    // The wake pipe must exist before the handler is installed: a signal delivered
    // before the accept loop starts polling just leaves its byte buffered there.
    if (pipe(g_wakePipe) != 0) {
        std::cerr << "[Server] wake pipe: " << strerror(errno) << std::endl;
        return 1;
    }
    // Non-blocking read end: the drain loop after poll() wakes reads until empty,
    // and a blocking read() there would hang forever once the one buffered byte
    // is gone (write() stays blocking — it only ever pushes a single byte).
    fcntl(g_wakePipe[0], F_SETFL, O_NONBLOCK);
    {
        struct sigaction sa{};
        sa.sa_handler = handleShutdownSignal;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGTERM, &sa, nullptr);
        sigaction(SIGINT, &sa, nullptr);
    }

    // Load the saved world model + player positions, and periodically persist them.
    loadWorld();
    // A world loaded at (not just over) its cap refuses every new block players
    // place while edits to existing cells still save — the partial, silent loss the
    // first public server shipped with. Say it loudly, with the number to use.
    switch (ewb::cell_cap_state(g_world.size(), g_maxWorldCells)) {
        case ewb::CellCapState::Full:
            std::cerr << "[Server] WARNING: the world has " << g_world.size()
                      << " cells and --max-world-cells is " << g_maxWorldCells
                      << ". Every NEW block players place will be refused and not saved (edits"
                         " to existing cells still save). Restart with --max-world-cells "
                      << ewb::recommended_max_world_cells(g_world.size()) << " or higher." << std::endl;
            break;
        case ewb::CellCapState::Low:
            std::cerr << "[Server] warning: the world has " << g_world.size() << " of "
                      << g_maxWorldCells << " cells; only " << (g_maxWorldCells - g_world.size())
                      << " left for new building. Consider --max-world-cells "
                      << ewb::recommended_max_world_cells(g_world.size()) << "." << std::endl;
            break;
        case ewb::CellCapState::Ok:
            break;
    }
    loadPlayerPos();
    loadSpawn();    // eden_spawn.txt: the default spawn for a player with no saved row (stage 5.3)
    loadSigns();   // sidecar; the control socket's `signs add|rm` writes it (stage 3.2)
    pruneOrphanSigns();   // after both loads: drop signs on blocks stored as air (stage 7.2)
    loadBans();
    loadOps();
    {
        // Before the zones, so a levelled zone can say whether anyone could bypass it.
        std::string err;
        if (!loadAuth(err)) {
            std::cerr << "[Server] " << err << "\n[Server] Refusing to start with login PINs"
                         " unreadable: fix or remove the line (every PIN-protected name's op level would"
                         " otherwise go to whoever claims it)." << std::endl;
            return 2;
        }
    }
    {
        std::string err;
        if (!loadZones(err)) {
            std::cerr << "[Server] " << err << "\n[Server] Refusing to start with protected zones"
                         " unreadable: fix or remove the line (every zone on file would otherwise be"
                         " unprotected)." << std::endl;
            return 2;
        }
    }
    {
        // eden_motd.txt: the welcome message shown on join. Absent is normal and
        // silent (the `--signs` convention); a present one is worth a line,
        // because "why don't players see my MOTD" is otherwise unanswerable.
        const size_t n = loadMotd();
        if (n) std::cout << "[Server] Loaded MOTD (" << n << " line(s)) from "
                         << g_motdFile << "." << std::endl;
    }

    if (g_haveWorldSpawn)
        std::cout << "[Server] World spawn " << g_worldSpawn.x << ":" << g_worldSpawn.y << ":"
                  << g_worldSpawn.z << " (players with a saved position keep it)." << std::endl;
    if (g_legacySnapshot)
        std::cout << "[Server] --legacy-snapshot: pushing the plaintext ACTION world dump on JOIN."
                  << std::endl;
    if (g_connectLimit <= 0)
        std::cout << "[Server] --connect-limit 0: per-IP connect pacing disabled." << std::endl;
    g_authFail.set_threshold((size_t)std::max(g_authFailLimit, 0));
    g_loginFail.set_threshold((size_t)std::max(g_authFailLimit, 0));   // /login, same policy (8.6)
    if (g_authFailLimit <= 0)
        std::cout << "[Server] --auth-fail-limit 0: per-IP wrong-password lockout disabled." << std::endl;
    if (g_handshakeTimeout <= 0)
        std::cout << "[Server] --handshake-timeout 0: pre-JOIN read timeout disabled "
                     "(a connection can hold a slot until TCP keepalive reaps it)." << std::endl;
    else if (g_idleConnTimeout <= 0)
        std::cout << "[Server] --idle-timeout-conn 0: post-JOIN idle read timeout disabled." << std::endl;
    if (!g_regionEmptyFrame)
        std::cout << "[Server] --no-region-empty-frame: unbuilt regions answered with silence." << std::endl;
    if (g_burnMaxCells < ewb::EXPLODE_VISITS_PER_BLAST) {
        std::cerr << "[Server] --burn-max-cells " << g_burnMaxCells << " is below one blast ("
                  << ewb::EXPLODE_VISITS_PER_BLAST << " cells); using that." << std::endl;
        g_burnMaxCells = ewb::EXPLODE_VISITS_PER_BLAST;
    }
    if (SV_ACTION_RATE <= 0.0)
        std::cout << "[Server] --action-rate 0: per-connection ACTION rate limit disabled." << std::endl;
    else
        std::cout << "[Server] ACTION budget: " << SV_ACTION_RATE << "/s, "
                  << SV_ACTION_BURST << " burst (BURN costs " << SV_ACTION_COST_BURN
                  << " upfront, then 1 per blast past that)." << std::endl;
    std::cout << "[Server] BURN chain budget: " << g_burnMaxCells << " cells read (~"
              << g_burnMaxCells / ewb::EXPLODE_VISITS_PER_BLAST << " blasts) per ACTION." << std::endl;
    if (SV_MOVE_RATE <= 0.0)
        std::cout << "[Server] --move-rate 0: per-connection movement rate limit disabled." << std::endl;
    else
        std::cout << "[Server] Movement budget: " << SV_MOVE_RATE << "/s, "
                  << SV_MOVE_BURST << " burst (POS/VEL/POSVEL, over-budget dropped silently)." << std::endl;
    if (SV_CHAT_RATE <= 0.0)
        std::cout << "[Server] --chat-rate 0: per-connection chat rate limit disabled." << std::endl;
    else
        std::cout << "[Server] Chat budget: " << SV_CHAT_RATE << "/s, "
                  << SV_CHAT_BURST << " burst (over-budget chat is told to the sender)." << std::endl;

    // Register with the matchmaker if one was configured.
    if (!g_matchHost.empty()) {
        std::thread(matchmakerThread).detach();
    }

    // Tier 2 player command surface (stage 3.3). The banner states the level an
    // unlisted player gets, because that single number decides whether a public
    // server hands WorldEdit to every visitor.
    if (!g_weEnabled) {
        std::cout << "[Server] --no-worldedit: in-chat commands disabled." << std::endl;
    } else {
        std::cout << "[Server] Player commands on. Default level " << g_defaultLevel
                  << " (" << (g_defaultLevel >= ewb::WE_LEVEL_BUILDER ? "may edit" : "read-only")
                  << "), max " << g_weMaxCells << " cells/command, ";
        if (g_weCellRate <= 0.0) std::cout << "no cell rate limit";
        else                     std::cout << g_weCellRate << " cells/s";
        std::cout << ", " << (g_weUndoBudget >> 10) << " KiB undo/player." << std::endl;
    }

    // Audit channel (stage 3.4). Say where it goes, because a public server that
    // thinks it is keeping a record and is not is worse off than one that knows.
    if (g_auditFile.empty())
        std::cout << "[Server] Audit log: stdout only (grep '[Audit]'). --audit-file keeps a copy."
                  << std::endl;
    else
        std::cout << "[Server] Audit log: stdout and " << g_auditFile << "." << std::endl;

    // Protected zones (stage 8.2). The restore thread only exists when there is a delay
    // to wait out; with 0 a refusal's restore is sent by the thread that refused it.
    {
        const size_t nz = zonesSnapshot()->size();
        if (nz)
            std::cout << "[Server] Protected zones: " << nz << ". Refused edits are put back after "
                      << g_zoneRevertDelayMs << " ms (--zone-revert-delay-ms)." << std::endl;
        if (g_zoneRevertDelayMs > 0) std::thread(revertThread).detach();
    }

    // Tier 1 operator control socket (stage 3.2).
    if (g_controlEnabled) {
        std::thread(controlThread).detach();
    } else {
        std::cout << "[Server] --no-control-socket: no operator control socket." << std::endl;
    }
    std::thread([](){
        int idleAccum = 0;   // seconds spent with zero clients (for --idle-timeout)
        while (serverRunning) {
            std::this_thread::sleep_for(std::chrono::seconds(15));
            saveWorld();
            savePlayerPos();
            saveSigns();
            flushZoneAudit();   // denial counts folded into closed windows (stage 8.2)
            // Self-shutdown for on-demand hosted worlds: exit once we've had no
            // clients for --idle-timeout seconds (covers both "nobody ever joined"
            // and "everyone left"). The matchmaker then drops us from its list.
            if (g_idleTimeout > 0) {
                size_t n; { std::lock_guard<std::mutex> lk(clientsMutex); n = clients.size(); }
                if (n == 0) {
                    idleAccum += 15;
                    if (idleAccum >= g_idleTimeout) {
                        std::cout << "[Server] Idle for " << idleAccum
                                  << "s with no players; shutting down." << std::endl;
                        shutdownAndExit();
                    }
                } else {
                    idleAccum = 0;
                }
            }
        }
    }).detach();

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(port);

    listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET) {
        std::cerr << "Socket creation failed: " << strerror(errno) << std::endl;
        return 1;
    }

    int opt = 1;
    setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(listenSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "Bind failed: " << strerror(errno) << std::endl;
        close(listenSocket);
        return 1;
    }

    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "Listen failed: " << strerror(errno) << std::endl;
        close(listenSocket);
        return 1;
    }

    std::cout << "========================================" << std::endl;
    std::cout << "  Eden Multiplayer Server" << std::endl;
    std::cout << "  Listening on port " << port << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Waiting for clients to connect..." << std::endl;

    int clientIdCounter = 0;
    ewb::ConnectLimiter connectLimiter((size_t)std::max(g_connectLimit, 0), SV_CONNECT_WINDOW_SEC);
    double lastConnectRefusalLog = 0.0;
    double lastAcceptFailLog = 0.0;

    while (serverRunning) {
        // accept() blocks indefinitely with no client waiting, so a bare
        // "while (serverRunning) accept(...)" never notices the flag flip on
        // shutdown. poll() on the listen socket + the signal handler's wake pipe
        // makes shutdown immediate instead of "whenever the next client connects".
        pollfd pfds[2] = {
            { listenSocket, POLLIN, 0 },
            { g_wakePipe[0], POLLIN, 0 },
        };
        const int pr = poll(pfds, 2, -1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[Server] poll failed: " << strerror(errno) << std::endl;
            break;
        }
        if (pfds[1].revents & POLLIN) {
            char drain[16];
            while (read(g_wakePipe[0], drain, sizeof(drain)) > 0) {}
            break;   // serverRunning is already false
        }
        if (!(pfds[0].revents & POLLIN)) continue;

        sockaddr_in clientAddr{};
        socklen_t clientAddrLen = sizeof(clientAddr);
        SOCKET clientSocket = accept(listenSocket, reinterpret_cast<sockaddr*>(&clientAddr), &clientAddrLen);

        if (clientSocket == INVALID_SOCKET) {
            // EINTR/ECONNABORTED are routine (a signal, or a peer that reset before we
            // could accept it) and not worth a log line. EMFILE/ENFILE/ENOBUFS/ENOMEM
            // are resource exhaustion: without a sleep, poll() keeps reporting the
            // listen socket readable and this becomes a tight 100%-CPU loop that also
            // floods the log with one line per iteration.
            const int err = errno;
            const auto act = ewb::classify_accept_error(err);
            if (serverRunning && act != ewb::AcceptErrorAction::Retry) {
                const double now = monoSeconds();
                if (now - lastAcceptFailLog >= 1.0) {
                    std::cerr << "[Server] Accept failed: " << strerror(err) << std::endl;
                    lastAcceptFailLog = now;
                }
                if (act == ewb::AcceptErrorAction::BackoffSleep)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }

        char clientIP[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, INET_ADDRSTRLEN);

        // IP ban check, before a thread is spawned (stage 3.2). A name ban is
        // enforced later, at JOIN, once the name is known.
        {
            std::lock_guard<std::mutex> lk(g_banMtx);
            if (g_bans.ip_banned(clientIP)) {
                close(clientSocket);
                continue;   // a banned host gets no reply
            }
        }

        // Per-IP wrong-password lockout (stage 1.10). An IP that has tripped the
        // failed-auth threshold is dropped here, before a thread spawns and
        // before it can send another JOIN — this is what turns "one guess per
        // connect, forever" into "N guesses then an escalating cooldown".
        if (g_authFail.enabled()) {
            bool locked;
            { std::lock_guard<std::mutex> lk(g_authFailMtx);
              locked = g_authFail.blocked(clientIP, monoSeconds()); }
            if (locked) {
                close(clientSocket);   // no reply: a locked-out guesser does not deserve one
                const double now = monoSeconds();
                if (now - lastConnectRefusalLog >= 1.0) {
                    std::cerr << "[Server] Rejected " << clientIP << " (auth lockout)." << std::endl;
                    lastConnectRefusalLog = now;
                }
                continue;
            }
        }

        // Per-IP connect pacing (stage 1.7). SV_MAX_CLIENTS below bounds how many
        // sessions exist at once; this bounds how fast they can be *created*, since
        // every accept() spawns a thread. The limiter lives in this loop and this
        // loop is single-threaded, so it needs no lock of its own.
        if (g_connectLimit > 0) {
            const double now = monoSeconds();
            if (!connectLimiter.allow(clientIP, now)) {
                close(clientSocket);   // no reply: a flood does not deserve one
                if (now - lastConnectRefusalLog >= 1.0) {   // one line/second, not one per attempt
                    std::cerr << "[Server] Rejected " << clientIP << " (connect rate limit)."
                              << std::endl;
                    lastConnectRefusalLog = now;
                }
                continue;
            }
        }

        // Enforce a maximum number of concurrent clients (thread/memory guard).
        size_t nclients;
        { std::lock_guard<std::mutex> lock(clientsMutex); nclients = clients.size(); }
        if (nclients >= (size_t)SV_MAX_CLIENTS) {
            // ⚠️ The one deliberate direct send() to a player socket (stage 7.3).
            // No ClientOut exists yet and no second writer can, so a queue here
            // would be pure ceremony — and this socket is closed on the next line.
            const char* full = "[Server] Server full.\n";
            send(clientSocket, full, strlen(full), 0);
            close(clientSocket);
            std::cerr << "[Server] Rejected " << clientIP << " (server full)." << std::endl;
            continue;
        }

        // Detect peers that vanish without a FIN so their thread can exit.
        int ka = 1; setsockopt(clientSocket, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof(ka));

        // Nagle holds a small write until the previous segment is ACKed; the
        // writer sends one queue item per send() and the hottest item is a
        // ~60-byte POSVEL broadcast, so without this every other player's
        // movement arrives in RTT-quantised jerks (stage 7.11).
        if (g_tcpNodelay) {
            int nd = 1;
            setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof(nd));
        }

        int clientId = ++clientIdCounter;
        std::cout << "[Server] Client #" << clientId << " connected from " << clientIP << std::endl;

        {
            std::lock_guard<std::mutex> lock(clientsMutex);
            clients.push_back(clientSocket);
        }
        // Start this client's writer before its reader exists, so every line the
        // reader can produce — including a JOIN denial — has somewhere to go.
        openOut(clientSocket, clientId);

        // Detached; handleClient does its own cleanup (removeClient + close) on
        // exit, so no thread handle is retained and nothing accumulates.
        std::thread(handleClient, clientSocket, clientId, std::string(clientIP)).detach();
    }

    close(listenSocket);
    shutdownAndExit();
    return 0;   // unreachable: shutdownAndExit() calls _exit()
}
