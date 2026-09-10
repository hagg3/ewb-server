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
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>
#include <sstream>
#include <fstream>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cmath>          // lroundf — Tier 2 positions are floats on the wire
#include <ctime>          // gmtime_r/strftime — the audit log's UTC stamp (stage 3.4)

#include "region_query.h"   // REGION reply geometry + Cell -> record table (stage 1.1)
#include "snapz_codec.h"    // raw DEFLATE + base64 + SNAPZ framing      (stage 1.2)
#include "sign_store.h"     // eden_signs.txt + SIGNQ -> SIGNP           (stage 1.5)
#include "spawn_store.h"    // eden_spawn.txt: a world's default spawn     (stage 5.3)
#include "hardening.h"      // names, token buckets, ACTION validation   (stage 1.7)
#include "control.h"        // Tier 1 operator control socket             (stage 3.2)
#include "worldedit.h"      // Tier 2 player command surface              (stage 3.3)

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
};

std::vector<SOCKET> clients;
std::map<SOCKET, PlayerInfo> playerInfoMap;
std::mutex clientsMutex;
bool serverRunning = true;
SOCKET listenSocket = INVALID_SOCKET;

// --- Authoritative world state ---
// The base terrain (flatland) is deterministic on every client; the server only
// needs to store the *edits* players make. The edit log is an ordered list of
// "x:y:z:mode[:extra]" suffixes. Replaying it in order reconstructs the current
// map. New players are sent the whole log when they join; live edits are relayed.
// --- Configuration (set from command-line flags in main) ---
std::string g_worldFile  = "eden_world.model";  // world model (block deltas)
std::string g_serverName = "Eden Server";       // shown in the matchmaker list
std::string g_password   = "";                  // empty = open server
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
bool        g_haveWorldSpawn = false;            // a default spawn point is configured (file or --spawn)
ewb::Spawn  g_worldSpawn;                        // the default spawn handed to a player with no saved position
bool        g_legacySnapshot = false;            // push the ACTION dump on JOIN (--legacy-snapshot)
int         g_connectLimit = 10;                 // connects per IP per window; 0 = off (--connect-limit)

// --- Connection-lifecycle hardening (stage 1.10) ---
int         g_authFailLimit    = 5;   // wrong-password attempts per IP per minute before an escalating lockout; 0 = off (--auth-fail-limit)
int         g_handshakeTimeout = 15;  // seconds a fresh connection has to send JOIN before it is dropped; 0 = off (--handshake-timeout)
int         g_idleConnTimeout  = 300; // seconds of post-JOIN socket silence tolerated; 0 = off (--idle-timeout-conn)

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

// REGION service counters (the measurement plan §3.2's `region-stats` asks for).
std::atomic<uint64_t> g_rgnRequests{0};
std::atomic<uint64_t> g_rgnCellsScanned{0};
std::atomic<uint64_t> g_rgnRecords{0};
std::atomic<uint64_t> g_rgnBytesOut{0};
std::atomic<uint64_t> g_rgnMicros{0};

// Safety limits (hardening for public hosting).
static const int    SV_WORLD_HEIGHT   = 256;       // must match the game's T_HEIGHT
static const size_t SV_MAX_WORLD_CELLS_DEFAULT = 4000000;   // default edited-cell ceiling
size_t              g_maxWorldCells   = SV_MAX_WORLD_CELLS_DEFAULT;  // cap edited-cell count (mem/disk guard); --max-world-cells
static const size_t SV_MAX_LINE       = 8192;      // drop a client flooding without '\n'
static const int    SV_MAX_CLIENTS    = 64;        // reject connections beyond this

// REGION is the single most expensive thing a client can ask for and a trivial
// amplification vector (a ~20-byte request producing megabytes of reply), so it is
// paced per connection. 750 ms matches VuencLink's `net::region::REGION_MIN_GAP`
// and the session cap its `MAX_REGIONS_PER_SESSION`. ⚠️ These are *our* limits, not
// client etiquette — VuencLink imposes the same numbers on itself, but a hostile or
// buggy client has no such scruples.
static const long SV_REGION_MIN_GAP_MS = 750;
static const int  SV_MAX_REGIONS_PER_SESSION = 256;

// SIGNQ is a smaller REGION: a 6-byte request answered with the whole sign file.
// At the sign cap below that is a few hundred KB from one line — an amplification
// vector in exactly the same shape, so it gets the same treatment (gated on JOIN,
// paced, session-capped).
static const long SV_SIGNQ_MIN_GAP_MS = 1000;
static const int  SV_MAX_SIGNQ_PER_SESSION = 32;
static const size_t SV_MAX_SIGNS = 20000;      // bound the burst the file can produce

// Per-connection ACTION budget (stage 1.7, retuned by 1.8 rung 2/3).
// ⚠️ The original 8/s + 64 burst was sized for a *human* placing blocks by hand. It
// silently shreds a legitimate bulk edit: VuencLink's "arm interact + fill a box"
// drains its own queue at ~500 lines/s (default `net::queue` rate), so a 1728-cell
// box arrived as ~96 scattered survivors and the rest were dropped at ingest —
// exactly the "most of my box is gone on reconnect" bug 1.8 found. The defaults now
// accommodate VuencLink's default drain rate; a public operator can tighten them
// with --action-rate / --action-burst (0 = unlimited). ⚠️ BURN still costs far more
// because one ACTION can write hundreds of cells server-side: simExplode's radius-6
// sphere, times its chain depth.
static double SV_ACTION_BURST     = 1024.0;
static double SV_ACTION_RATE      = 512.0;  // tokens/second (0 = unlimited)
static const double SV_ACTION_COST_BURN = 64.0;

// Per-IP connect pacing. SV_MAX_CLIENTS bounds concurrency but not churn — a
// connect flood still spawns a thread per attempt. The default 10 in 10 s allows a
// human reconnecting repeatedly while flattening a loop.
// ⚠️ It is **per source address**, so a whole LAN behind one NAT address shares one
// allowance, and so does a test harness on loopback — hence --connect-limit N
// (0 disables), which the stage 1.8 live test uses.
static const double SV_CONNECT_WINDOW_SEC  = 10.0;

// Chat is broadcast verbatim to every peer; cap what one line can cost.
static const size_t SV_MAX_CHAT = 256;

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
struct Cell { unsigned char type; unsigned char color; };
static std::unordered_map<uint64_t, Cell> g_world;
static std::mutex g_worldMtx;
static std::mutex g_saveMtx;         // serializes on-disk writes (world + players)
std::atomic<bool> editsDirty{false};

// Block constants mirrored from the game's Constants.h.
enum { SV_AIR=0, SV_BEDROCK=1, SV_TNT=9, SV_FIREWORK=65, SV_STEEL=74, SV_PAINTED_BASE=255 };
static const int SV_EXPLOSION_RADIUS = 6;

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
// Caller must hold g_worldMtx.
static void worldSet(int x,int y,int z,int type,int color){
    uint64_t k = wkey(x,y,z);
    // Cap the number of distinct edited cells so a malicious/buggy client can't
    // grow the map (and the on-disk save) without bound. Updates to existing
    // cells are always allowed; only brand-new cells are refused past the cap.
    if(g_world.find(k)==g_world.end() && g_world.size()>=g_maxWorldCells){
        static bool warned=false;
        if(!warned){ std::cerr << "[Server] world cell cap reached (" << g_maxWorldCells
                               << "); refusing new cells." << std::endl; warned=true; }
        return;
    }
    g_world[k] = { (unsigned char)type, (unsigned char)color };
    editsDirty = true;
}
static bool worldGet(int x,int y,int z, Cell& out){
    auto it = g_world.find(wkey(x,y,z));
    if(it==g_world.end()) return false;
    out = it->second; return true;
}

// Simulate a TNT / paint explosion centred on (x,y,z) — mirrors Terrain::explode:
// a spherical radius; color!=0 paints the sphere, color==0 destroys it. TNT hit by
// the blast chains. Caller holds g_worldMtx.
static void simExplode(int cx,int cy,int cz,int depth){
    if(depth>6) return;                        // guard runaway chains
    Cell center; bool haveCenter = worldGet(cx,cy,cz,center);
    int color = haveCenter ? center.color : 0;
    bool painting = (color != 0);
    worldSet(cx,cy,cz, SV_AIR, 0);   // the TNT itself is consumed by its own blast
    struct P3{int x,y,z;};
    std::vector<P3> chain;
    const int R = SV_EXPLOSION_RADIUS;
    for(int i=1;i<=R;i++){
        int yy=R-i;
        for(int j=cx-R;j<=cx+R;j++) for(int k=cz-R;k<=cz+R;k++){
            int ox=j-cx, oz=k-cz;
            if(ox*ox+oz*oz+yy*yy > R*R) continue;
            int ys[2]={cy-yy, cy+yy};
            for(int s=0;s<2;s++){
                int sy=ys[s];
                if(sy<0||sy>=1024) continue;
                Cell c; bool have=worldGet(j,sy,k,c);
                if(painting){
                    if(have){
                        if(c.type==SV_AIR) continue;
                        if(c.type==SV_TNT && c.color==0) continue;
                        worldSet(j,sy,k, c.type, color);
                    }else{
                        worldSet(j,sy,k, SV_PAINTED_BASE, color);   // paint a base block
                    }
                }else{
                    if(have){
                        if(c.type==SV_AIR) continue;
                        if(c.type==SV_TNT || c.type==SV_FIREWORK) chain.push_back({j,sy,k});
                        else if(c.type!=SV_BEDROCK && c.type!=SV_STEEL) worldSet(j,sy,k, SV_AIR, 0);
                    }else{
                        worldSet(j,sy,k, SV_AIR, 0);                 // destroy a base block
                    }
                }
            }
        }
    }
    for(const P3& t: chain) simExplode(t.x,t.y,t.z, depth+1);
}

// Apply one terrain action to the model. mode: 0 build 1 mine 2 burn 3 paint.
static void simAction(int mode,int x,int y,int z,int extra){
    std::lock_guard<std::mutex> lk(g_worldMtx);
    switch(mode){
        case 0: worldSet(x,y,z, extra, 0); break;                 // BUILD (extra=type)
        case 1: worldSet(x,y,z, SV_AIR, 0); break;                // MINE
        case 3: { Cell c; bool have=worldGet(x,y,z,c);            // PAINT (extra=color)
                  worldSet(x,y,z, have? c.type : SV_PAINTED_BASE, extra); break; }
        case 2: { Cell c; bool have=worldGet(x,y,z,c);            // BURN
                  if(have && (c.type==SV_TNT || c.type==SV_FIREWORK)) simExplode(x,y,z,0);
                  else if(have && c.type!=SV_AIR) worldSet(x,y,z, SV_AIR, 0);
                  break; }
    }
}

void loadWorld() {
    std::ifstream f(g_worldFile);
    if (!f) return;
    std::string line;
    std::lock_guard<std::mutex> lock(g_worldMtx);
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        int v[5]={0,0,0,0,0}, n=0; std::stringstream ss(line); std::string p;
        while (n<5 && std::getline(ss,p,':')) v[n++]=atoi(p.c_str());
        if (n>=4) g_world[wkey(v[0],v[1],v[2])] = { (unsigned char)v[3], (unsigned char)v[4] };
    }
    std::cout << "[Server] Loaded " << g_world.size() << " world cells from " << g_worldFile << std::endl;
}

void saveWorld() {
    std::unordered_map<uint64_t,Cell> snap;
    {
        std::lock_guard<std::mutex> lock(g_worldMtx);
        if (!editsDirty.exchange(false)) return;   // nothing changed
        snap = g_world;
    }
    // Atomic, serialized write: fill a temp file then rename() over the real one,
    // so a crash/kill mid-write can never leave a truncated world, and two
    // concurrent saves (disconnect + timer) can't interleave.
    std::lock_guard<std::mutex> save(g_saveMtx);
    std::string tmp = g_worldFile + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if(!f){ std::cerr << "[Server] save: cannot open " << tmp << std::endl; editsDirty = true; return; }
        for (const auto& kv : snap) {
            int x,y,z; wunkey(kv.first,x,y,z);
            f << x << ":" << y << ":" << z << ":" << (int)kv.second.type << ":" << (int)kv.second.color << "\n";
        }
        f.flush();
        if(!f){ std::cerr << "[Server] save: write error to " << tmp << std::endl; editsDirty = true; return; }
    }
    if(std::rename(tmp.c_str(), g_worldFile.c_str()) != 0){
        std::cerr << "[Server] save: rename " << tmp << " -> " << g_worldFile << " failed" << std::endl;
        editsDirty = true; return;
    }
    std::cout << "[Server] Saved world (" << snap.size() << " cells)." << std::endl;
}

// --- Player position persistence ----------------------------------------------
// Remember where each player was (by username) so they respawn there on rejoin.
struct SavedPos { float x, y, z; };
static std::map<std::string, SavedPos> g_playerPos;
static std::mutex g_posMtx;
static std::string g_posFile = "eden_players.txt";
static std::atomic<bool> g_posDirty{false};

void loadPlayerPos() {
    std::ifstream f(g_posFile);
    if (!f) return;
    std::string line;
    std::lock_guard<std::mutex> lk(g_posMtx);
    while (std::getline(f, line)) {
        size_t p1 = line.find(':');          // username can't contain ':' (protocol delimiter)
        if (p1 == std::string::npos) continue;
        std::string name = line.substr(0, p1);
        float x,y,z;
        if (sscanf(line.c_str()+p1+1, "%f:%f:%f", &x,&y,&z) == 3)
            g_playerPos[name] = { x,y,z };
    }
    std::cout << "[Server] Loaded " << g_playerPos.size() << " player positions." << std::endl;
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

void savePlayerPos() {
    std::map<std::string,SavedPos> snap;
    {
        std::lock_guard<std::mutex> lk(g_posMtx);
        if (!g_posDirty.exchange(false)) return;
        snap = g_playerPos;
    }
    std::lock_guard<std::mutex> save(g_saveMtx);   // atomic temp+rename, serialized
    std::string tmp = g_posFile + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if(!f){ std::cerr << "[Server] save: cannot open " << tmp << std::endl; g_posDirty = true; return; }
        for (const auto& kv : snap)
            f << kv.first << ":" << kv.second.x << ":" << kv.second.y << ":" << kv.second.z << "\n";
        f.flush();
        if(!f){ std::cerr << "[Server] save: write error to " << tmp << std::endl; g_posDirty = true; return; }
    }
    if(std::rename(tmp.c_str(), g_posFile.c_str()) != 0){
        std::cerr << "[Server] save: rename " << tmp << " -> " << g_posFile << " failed" << std::endl;
        g_posDirty = true; return;
    }
}

// Write `content` to `path` atomically (temp file + rename), serialized on
// g_saveMtx like the world/player writes so a crash mid-write can't truncate a
// sidecar and two writers can't interleave. Returns false on error.
static bool writeFileAtomic(const std::string& path, const std::string& content) {
    std::lock_guard<std::mutex> save(g_saveMtx);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) { std::cerr << "[Server] write: cannot open " << tmp << std::endl; return false; }
        f << content;
        f.flush();
        if (!f) { std::cerr << "[Server] write: error writing " << tmp << std::endl; return false; }
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::cerr << "[Server] write: rename " << tmp << " -> " << path << " failed" << std::endl;
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

// Record a player's latest position (by username).
static void rememberPos(const std::string& name, float x, float y, float z){
    if(name.empty()) return;
    std::lock_guard<std::mutex> lk(g_posMtx);
    g_playerPos[name] = { x,y,z };
    g_posDirty = true;
}

// --- Signs (stage 1.5, read-only) ---------------------------------------------
// The sidecar is operator-authored and immutable while we run, so the whole
// `SIGNP` burst is formatted once at startup and re-sent verbatim per SIGNQ:
// nothing to rebuild per request, and no lock held while formatting. (Phase 3's
// `signs reload` rebuilds this blob under g_signMtx; the mutex is here for that.)
static std::vector<ewb::Sign> g_signs;
static std::string            g_signBlob;
static std::mutex             g_signMtx;

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
        g_signBlob = std::move(blob);
    }
    std::cout << "[Server] Loaded " << n << " signs from " << g_signFile
              << " (" << bytes << " B burst)." << std::endl;
}

// Keep a persistent registration with the matchmaker for as long as we run.
// The matchmaker treats the open TCP connection as "online"; if we exit it drops
// and we're removed. Reconnects on failure.
//
// Wire (Eden dev, WORKING/matchmakerinfo.txt; own matchmaker in edenmatch.cpp):
//   -> REGISTER:<name>:<port>:<hasPassword>[:<advertiseIP>]
//   <- REGISTERED
//   -> PING          every ~20s — a bare no-op that just resets the TTL (~45s).
// The heartbeat carries no player count: the dev's PING is argument-less. (A
// count channel would be a separate message; edenmatch tolerates `PING:<n>` but
// nothing documented consumes it.)
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

                // Heartbeat: bare PING every ~20s, comfortably inside the ~45s TTL.
                while (serverRunning) {
                    const char ping[] = "PING\n";
                    if (send(s, ping, sizeof(ping) - 1, 0) <= 0) break;
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

// Send the current world (derived from the model) to one newly-joined client.
// Send an entire buffer, looping over partial writes.
static void sendAll(SOCKET s, const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(s, data + off, len - off, 0);
        if (n <= 0) break;
        off += (size_t)n;
    }
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
        for (const auto& kv : g_world) {
            int x,y,z; wunkey(kv.first,x,y,z);
            const Cell& c = kv.second;
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
        }
    }
    sendAll(clientSocket, blob.data(), blob.size());
    std::cout << "[Server] Sent legacy world snapshot (" << cells << " cells, " << blob.size()
              << " bytes) to new player." << std::endl;
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

    std::string blob;
    size_t count;
    {
        std::lock_guard<std::mutex> lk(g_signMtx);
        blob  = g_signBlob;      // copy out; the send happens with no lock held
        count = g_signs.size();
    }
    if (blob.empty()) {
        if (g_verbose) std::cout << "[Server] SIGNQ from " << who << ": no signs." << std::endl;
        return;
    }
    sendAll(clientSocket, blob.data(), blob.size());
    if (g_verbose) std::cout << "[Server] SIGNQ from " << who << ": sent " << count
                             << " signs (" << blob.size() << " B)." << std::endl;
}

// Answer one `REGION:<x>:<z>` with a burst of `SNAPZ` frames.
//
// ⚠️ **The world lock is held for the scan only.** Matching records are copied into
// a local vector under `g_worldMtx`; sorting, deflate, base64 and send() all happen
// after it is released. The real server appears to hold its world locked for the
// full ~1 s a region takes — that is precisely the behaviour VuencLink's
// `net/region.rs` etiquette rules exist to avoid triggering, and reproducing it
// would make every other player's edits queue behind one player's walk.
//
// The scan itself is a filtered pass over the whole `unordered_map` (no spatial
// index). At the 4M-cell cap that is a few tens of ms — already far better than the
// ~1 s/region observed on the real server. Only add a chunk index if the log line
// below says the scan actually exceeds ~100 ms on a realistic world.
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
    size_t scanned = 0, inBox = 0;
    const auto t0 = clock::now();
    {
        std::lock_guard<std::mutex> lock(g_worldMtx);
        scanned = g_world.size();
        for (const auto& kv : g_world) {
            int x, y, z; wunkey(kv.first, x, y, z);
            if (!box.contains(x, z)) continue;
            ++inBox;
            ewb::emit_cell_records(x, y, z, kv.second.type, kv.second.color, recs);
        }
    }   // <-- lock released here; nothing below re-takes it.
    const auto t1 = clock::now();

    if (g_regionSort) ewb::sort_records(recs);

    size_t wireBytes = 0, frames = 0;
    try {
        for (size_t i = 0; i < recs.size(); i += ewb::SNAPZ_FRAME_RECORDS) {
            const size_t n = std::min(ewb::SNAPZ_FRAME_RECORDS, recs.size() - i);
            const std::string line = ewb::encode_snapz(recs.data() + i, n);
            sendAll(clientSocket, line.data(), line.size());
            wireBytes += line.size();
            ++frames;
        }
        // An unbuilt region has no records. We answer with an explicit `SNAPZ:0:`
        // (a well-formed frame that decodes to zero records) — 1.8 rung 2/3 showed
        // VuencLink needs a real frame back: it treats one as "answered" (resetting
        // its ABORT_AFTER_EMPTY counter) where silence leaves it stuck on "waiting
        // for the world snapshot" and aborts a ring sweep after 3 empty points.
        // --no-region-empty-frame restores pre-1.8 silence to A/B test the real
        // client in 1.9 (the real server has never been *observed* answering one).
        if (recs.empty() && g_regionEmptyFrame) {
            const std::string line = ewb::encode_snapz({});
            sendAll(clientSocket, line.data(), line.size());
            wireBytes += line.size();
            ++frames;
        }
    } catch (const std::exception& e) {
        // deflate failure must not take the client thread down (it is detached, so an
        // escaping exception would std::terminate the whole server).
        std::cerr << "[Server] REGION encode failed for " << who << ": " << e.what() << std::endl;
        return;
    }
    const auto t2 = clock::now();

    // The measurement the plan asks for: requests served, cells scanned, ms/region,
    // bytes out. Not gated on --verbose — one line per region is not chatty, and the
    // scan-vs-encode split is what decides whether a spatial index is worth building.
    const long scanMs = (long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    const long encMs  = (long)std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
    const size_t rawBytes = recs.size() * 20;

    // Feed the `region-stats` control command (stage 3.2).
    g_rgnRequests.fetch_add(1, std::memory_order_relaxed);
    g_rgnCellsScanned.fetch_add(scanned, std::memory_order_relaxed);
    g_rgnRecords.fetch_add(recs.size(), std::memory_order_relaxed);
    g_rgnBytesOut.fetch_add(wireBytes, std::memory_order_relaxed);
    g_rgnMicros.fetch_add(
        (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(t2 - t0).count(),
        std::memory_order_relaxed);

    char ratio[32] = "n/a";
    if (wireBytes) snprintf(ratio, sizeof(ratio), "%.1fx", (double)rawBytes / (double)wireBytes);
    std::cout << "[Server] REGION #" << lim.served << " " << who << " (" << cx << "," << cz
              << ") box x[" << box.x0 << ".." << box.x1 << "] z[" << box.z0 << ".." << box.z1
              << "]: " << inBox << "/" << scanned << " cells -> " << recs.size() << " records, "
              << frames << " frame(s), " << wireBytes << " B wire (" << ratio
              << "), scan " << scanMs << " ms, encode " << encMs << " ms" << std::endl;
}

void broadcastMessage(const std::string& message, SOCKET senderSocket) {
    std::lock_guard<std::mutex> lock(clientsMutex);
    for (SOCKET client : clients) {
        if (client != senderSocket) {
            send(client, message.c_str(), message.length(), 0);
        }
    }
}

void removeClient(SOCKET clientSocket) {
    std::lock_guard<std::mutex> lock(clientsMutex);
    clients.erase(std::remove(clients.begin(), clients.end(), clientSocket), clients.end());
    playerInfoMap.erase(clientSocket);
}

// Parse message with format "PREFIX:data1:data2:..."
std::vector<std::string> parseMessage(const std::string& message) {
    std::vector<std::string> parts;
    std::stringstream ss(message);
    std::string part;
    while (std::getline(ss, part, ':')) {
        parts.push_back(part);
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
    std::string msg = "[Server] You were " + reason + ".\n";
    send(target, msg.c_str(), msg.size(), 0);
    // shutdown() (not close()) unblocks the client thread's recv(); it then runs
    // its own cleanup (savePlayerPos / removeClient / close).
    shutdown(target, SHUT_RDWR);
    return ip.empty() ? std::string("?") : ip;
}

// One `x:y:z:type[:color]` edit relayed to every client with the reserved
// `server` sender (mine -> build -> paint, matching the legacy snapshot: `mode 0`
// alone does not overwrite an occupied cell on a peer). Shared by Tier 1
// (setblock/fill) and Tier 2 (every WorldEdit command), so there is one relay
// shape to get right. The caller writes the model; this only builds the wire.
static void emitEditWire(std::string& wire, int x, int y, int z, int type, int color) {
    char line[96];
    const std::string c = std::to_string(x) + ":" + std::to_string(y) + ":" + std::to_string(z);
    wire.append(line, snprintf(line, sizeof(line), "ACTION:server:0:%s:1\n", c.c_str()));   // mine
    if (type != SV_AIR) {
        wire.append(line, snprintf(line, sizeof(line), "ACTION:server:0:%s:0:%d\n", c.c_str(), type));
        if (color != 0 && color <= (int)ewb::CELL_MAX_PAINT)
            wire.append(line, snprintf(line, sizeof(line), "ACTION:server:0:%s:3:%d\n", c.c_str(), color));
    }
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
// the wire burst, release the lock, then broadcast once.
static long long ctlFillBox(int x0, int y0, int z0, int x1, int y1, int z1, int type, int color) {
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    if (z0 > z1) std::swap(z0, z1);
    std::vector<ewb::WeEdit> batch;
    batch.reserve((size_t)ewb::ctl_fill_volume(x0, y0, z0, x1, y1, z1));
    {
        std::lock_guard<std::mutex> lock(g_worldMtx);
        for (int x = x0; x <= x1; ++x)
            for (int z = z0; z <= z1; ++z)
                for (int y = y0; y <= y1; ++y) {
                    worldSet(x, y, z, type == SV_AIR ? SV_AIR : type, color);
                    batch.push_back({x, y, z, 0, 0, (unsigned char)type, (unsigned char)color});
                }
    }
    const std::string wire = emitEditBatch(batch);   // formatting, outside the lock
    if (!wire.empty()) broadcastMessage(wire, INVALID_SOCKET);
    return (long long)batch.size();
}

// Rewrite g_signFile from the current g_signs and rebuild the SIGNQ burst blob.
// Caller holds g_signMtx.
static bool ctlPersistSignsLocked() {
    std::string file;
    for (const ewb::Sign& s : g_signs) {
        char h[64];
        file.append(h, snprintf(h, sizeof(h), "%d:%d:%d:%d:%d:%d:", s.x, s.y, s.z, s.a, s.b, s.c));
        file += ewb::sanitize_text(s.text, ewb::SIGN_TEXT_MAX);
        file += '\n';
    }
    g_signBlob = ewb::format_sign_burst(g_signs);
    return writeFileAtomic(g_signFile, file);
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
        std::lock_guard<std::mutex> lock(clientsMutex);
        std::ostringstream ss;
        ss << playerInfoMap.size() << " player(s):\n";
        for (const auto& kv : playerInfoMap) {
            const PlayerInfo& p = kv.second;
            int lvl; { std::lock_guard<std::mutex> ol(g_opsMtx); lvl = g_ops.level_of(p.username, g_defaultLevel); }
            ss << "  " << p.username << " (T" << p.characterType << ") "
               << p.ip << "  @ " << (int)p.posX << "," << (int)p.posY << "," << (int)p.posZ
               << "  level " << lvl << "\n";
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
        ctlFillBox(x, y, z, x, y, z, type, color);
        auditLog("control", "setblock " + std::to_string(x) + "," + std::to_string(y) + "," +
                            std::to_string(z) + " = " + std::to_string(type) +
                            (color ? " color " + std::to_string(color) : ""));
        reply = "ok: set 1 block";
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
        const long long changed = ctlFillBox(v[0], v[1], v[2], v[3], v[4], v[5], type, color);
        auditLog("control", "fill " + std::to_string(changed) + " cells = " + std::to_string(type) +
                            (color ? " color " + std::to_string(color) : "") + " @ " +
                            std::to_string(v[0]) + "," + std::to_string(v[1]) + "," + std::to_string(v[2]) +
                            ".." + std::to_string(v[3]) + "," + std::to_string(v[4]) + "," + std::to_string(v[5]));
        reply = "ok: filled " + std::to_string(changed) + " cells";
        return;
    }

    if (verb == "signs") {
        const auto f = ewb::ctl_fields(rest, 0);
        const std::string sub = f.empty() ? "" : f[0];
        if (sub == "reload") {
            loadSigns();
            std::lock_guard<std::mutex> lk(g_signMtx);
            auditLog("control", "signs reload (" + std::to_string(g_signs.size()) + ")");
            reply = "ok: reloaded " + std::to_string(g_signs.size()) + " signs";
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
            std::lock_guard<std::mutex> lk(g_signMtx);
            g_signs.push_back(s);
            ctlPersistSignsLocked();
            auditLog("control", "signs add " + std::to_string(s.x) + "," + std::to_string(s.y) +
                                "," + std::to_string(s.z));
            reply = "ok: added sign at " + std::to_string(s.x) + "," + std::to_string(s.y) + "," + std::to_string(s.z) +
                    " (" + std::to_string(g_signs.size()) + " total)";
            return;
        }
        if (sub == "rm") {
            if (f.size() < 4) { reply = "usage: signs:rm:<x>:<y>:<z>"; return; }
            int x, y, z;
            try { x = std::stoi(f[1]); y = std::stoi(f[2]); z = std::stoi(f[3]); }
            catch (...) { reply = "error: non-numeric coordinate"; return; }
            std::lock_guard<std::mutex> lk(g_signMtx);
            const size_t before = g_signs.size();
            g_signs.erase(std::remove_if(g_signs.begin(), g_signs.end(),
                          [&](const ewb::Sign& s){ return s.x == x && s.y == y && s.z == z; }),
                          g_signs.end());
            const size_t removed = before - g_signs.size();
            if (removed) ctlPersistSignsLocked();
            auditLog("control", "signs rm " + std::to_string(x) + "," + std::to_string(y) + "," +
                                std::to_string(z) + " (" + std::to_string(removed) + " removed)");
            reply = removed ? "ok: removed " + std::to_string(removed) + " sign(s)"
                            : "error: no sign at " + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(z);
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
           << "  cells scanned   : " << g_rgnCellsScanned.load(std::memory_order_relaxed) << "\n"
           << "  records emitted : " << g_rgnRecords.load(std::memory_order_relaxed) << "\n"
           << "  bytes out       : " << g_rgnBytesOut.load(std::memory_order_relaxed) << "\n"
           << "  total time      : " << (us / 1000) << " ms\n"
           << "  mean per region : " << (reqs ? (double)us / reqs / 1000.0 : 0.0) << " ms\n";
        reply = ss.str();
        return;
    }

    reply = "error: command '" + verb + "' recognised but not implemented";
}

// One control connection: read '\n'-framed lines, answer each, until the peer
// closes. `stop` sets serverRunning=false and exits the process after a flush.
static void handleControlClient(int fd) {
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
                send(fd, reply.c_str(), reply.size(), 0);
            }
            if (stopServer) break;
        }
    }
    close(fd);
    g_ctlConns.fetch_sub(1, std::memory_order_relaxed);
    if (stopServer) {
        saveWorld();
        savePlayerPos();
        if (!g_controlSocket.empty()) unlink(g_controlSocket.c_str());
        std::cout << "Server terminated (control: stop)." << std::endl;
        std::exit(0);
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

    while (serverRunning) {
        const int c = accept(s, nullptr, nullptr);
        if (c < 0) { if (!serverRunning) break; continue; }
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

// Everything else a connection remembers between commands. One instance per
// `handleClient` frame; freed with the connection, no cleanup path to forget.
struct WeSession {
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
    const std::string line = "[Server] " + text + "\n";
    sendAll(s, line.data(), line.size());
}

// A player's permission level right now — re-read per command, so a `deop` from
// the control socket takes effect on the next line, not the next session.
static int weLevel(const std::string& name) {
    std::lock_guard<std::mutex> lk(g_opsMtx);
    return g_ops.level_of(name, g_defaultLevel);
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

// Write a decided set of cells into the model and relay them.
//
// `edits` supplies x/y/z and the new type/colour; the old values are filled in
// here, under the same lock as the write, so an undo record can never disagree
// with what was actually overwritten. No-ops and out-of-range cells are dropped.
// The lock covers deciding and writing the batch and nothing else — formatting
// the relay (the larger half, see `emitEditBatch`) and sending it both happen
// after it is released, so a large batch never blocks another player's REGION
// behind string building or a socket write.
static std::vector<ewb::WeEdit> weCommit(const std::vector<ewb::WeEdit>& edits, SOCKET s) {
    std::vector<ewb::WeEdit> batch;
    batch.reserve(edits.size());
    {
        std::lock_guard<std::mutex> lock(g_worldMtx);
        // Refuse rather than truncate: a partially applied edit is worse than a
        // refused one, and the player can see why.
        if (g_world.size() + edits.size() > g_maxWorldCells) {
            weSay(s, "The world is at its edited-cell limit; that edit was refused.");
            return batch;
        }
        for (const ewb::WeEdit& e : edits) {
            if (!weCellInRange(e.x, e.y, e.z)) continue;
            Cell cur{0, 0};
            const bool have = worldGet(e.x, e.y, e.z, cur);
            const unsigned char oldType  = have ? cur.type  : (unsigned char)SV_AIR;
            const unsigned char oldColor = have ? cur.color : 0;
            if (have && oldType == e.newType && oldColor == e.newColor) continue;
            worldSet(e.x, e.y, e.z, e.newType, e.newColor);
            batch.push_back({e.x, e.y, e.z, oldType, oldColor, e.newType, e.newColor});
        }
    }
    const std::string wire = emitEditBatch(batch);   // formatting, outside the lock
    if (!wire.empty()) broadcastMessage(wire, INVALID_SOCKET);
    return batch;
}

// Scan an inclusive box, ask `want` what each cell should become, and commit the
// answer. `want(x, y, z, present, curType, curColor, newType, newColor) -> bool`
// returns false to leave a cell alone.
//
// `present` distinguishes "a player carved this to air" (`type 0`) from "nobody
// has touched this cell" (absent — deterministic base terrain the server never
// stored). The patch this replaces conflated the two, so `//replace 0 <block>`
// silently filled every untouched cell in the box with a placed block. Its own
// help text promised the opposite ("WorldEdit only detects player-made blocks").
//
// The caller has already volume-checked the box and charged the budget.
template <typename F>
static std::vector<ewb::WeEdit> weEditBox(const ewb::WeBox& box, F&& want, SOCKET s) {
    std::vector<ewb::WeEdit> batch;
    {
        std::lock_guard<std::mutex> lock(g_worldMtx);
        const long long vol = ewb::we_box_volume(box);
        if (g_world.size() + (size_t)vol > g_maxWorldCells) {
            weSay(s, "The world is at its edited-cell limit; that edit was refused.");
            return batch;
        }
        for (int x = box.x0; x <= box.x1; ++x)
            for (int z = box.z0; z <= box.z1; ++z)
                for (int y = box.y0; y <= box.y1; ++y) {
                    if (!weCellInRange(x, y, z)) continue;
                    Cell cur{0, 0};
                    const bool have = worldGet(x, y, z, cur);
                    const int curType  = have ? cur.type  : SV_AIR;
                    const int curColor = have ? cur.color : 0;
                    int newType = curType, newColor = curColor;
                    if (!want(x, y, z, have, curType, curColor, newType, newColor)) continue;
                    if (have && newType == curType && newColor == curColor) continue;
                    worldSet(x, y, z, newType, newColor);
                    batch.push_back({x, y, z, (unsigned char)curType, (unsigned char)curColor,
                                     (unsigned char)newType, (unsigned char)newColor});
                }
    }
    const std::string wire = emitEditBatch(batch);   // formatting, outside the lock
    if (!wire.empty()) broadcastMessage(wire, INVALID_SOCKET);
    return batch;
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
    }
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
    char sp[96];
    const int n = snprintf(sp, sizeof(sp), "SPAWN:%.2f:%.2f:%.2f\n", x, y, z);
    sendAll(s, sp, (size_t)n);
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

    if (!we.cmds.allow(monoSeconds())) return;   // command spam; silent, costs nothing

    const ewb::WeSpec* spec = ewb::we_find(verb);
    if (!spec) {
        weSay(s, "Unknown command '" + verb + "'. Try /help.");
        return;
    }

    // ⚠️ The permission gate. It is here, once, before dispatch — not in each
    // handler, which is how the patch this replaces came to have none at all.
    const int level = weLevel(username);
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
        sendAll(dest, toThem.data(), toThem.size());
        sendAll(s, toUs.data(), toUs.size());
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

    // --- selection -----------------------------------------------------------

    if (verb == "//pos1" || verb == "//pos2") {
        int x, y, z;
        if (!weFeet(s, x, y, z)) { weSay(s, "The server does not have your position yet."); return; }
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
        }, s);
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
        // ACTION paint path and the SNAPZ encoder already agree on.
        auto batch = weEditBox(box, [&](int, int, int, bool have, int ct, int, int& nt, int& nc) {
            if (have && ct == SV_AIR) return false;      // don't paint carved air
            nt = have ? ct : SV_PAINTED_BASE;
            nc = color;
            return true;
        }, s);
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
        }, s);
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
        }, s);
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
        }, s);
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
                        clip.push_back({x - fx, y - fy, z - fz, c.type, c.color});
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
        weFinish(we, s, username, verb, weCommit(want, s));
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
        std::vector<ewb::WeEdit> batch;
        if (!(undo ? we.hist.take_undo(batch) : we.hist.take_redo(batch))) {
            weSay(s, undo ? "Nothing to undo." : "Nothing to redo.");
            return;
        }
        if (!weCharge(we, s, (long long)batch.size())) return;
        // Undo replays the batch backwards. A cell nobody had touched before the
        // edit is restored to `type 0` (air), not to an erased entry: `erase`
        // means "fall back to base terrain" in the model but there is no wire
        // message that says that, so erasing would desync every client that saw
        // the edit — plan §0.5.7 defect 4, in reverse.
        const std::vector<ewb::WeEdit> apply = undo ? ewb::we_invert(batch) : batch;
        std::vector<ewb::WeEdit> want;
        want.reserve(apply.size());
        for (const ewb::WeEdit& e : apply)
            want.push_back({e.x, e.y, e.z, 0, 0, e.newType, e.newColor});
        const auto done = weCommit(want, s);
        if (!done.empty())
            auditLog("player:" + username, verb + ": " + std::to_string(done.size()) + " cell(s)");
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
        auto batch = weCommit(want, s);
        if (!batch.empty()) {
            we.hist.record(std::move(batch));
            editsDirty = true;
            auditLog("player:" + username, "//up: 1 cell at " + std::to_string(fx) + "," +
                                           std::to_string(destY - 1) + "," + std::to_string(fz));
        }
        weTeleport(s, username, (float)fx, (float)(destY + 1), (float)fz);
        weSay(s, "Whoosh.");
        return;
    }

    weSay(s, "'" + verb + "' is not available on this server.");
}

void handleClient(SOCKET clientSocket, int clientId, std::string clientIP) {
    char recvBuffer[BUFFER_SIZE];
    std::string username = "Player" + std::to_string(clientId);
    int characterType = 0;
    bool joined = false;          // a successful JOIN gates REGION/SIGNQ (see below)
    BurstLimiter regionLimiter;   // per-connection REGION pacing
    BurstLimiter signLimiter;     // ...and SIGNQ pacing
    ewb::TokenBucket actionBucket(SV_ACTION_BURST, SV_ACTION_RATE);   // stage 1.7
    bool actionWarned = false;    // log the first refusal per connection, not each one
    WeSession we;                 // Tier 2 selection / clipboard / undo (stage 3.3)

    // Client->server messages are newline-framed: accumulate bytes and process
    // one complete '\n'-terminated line at a time. This makes parsing robust to
    // TCP coalescing (e.g. a POSVEL and an ACTION arriving in one recv()).
    std::string acc;
    bool disconnect = false;

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
        acc.append(recvBuffer, bytesReceived);

        // Guard against a client flooding without a newline (unbounded memory).
        if (acc.find('\n') == std::string::npos && acc.size() > SV_MAX_LINE) {
            std::cerr << "[Server] " << username << " sent oversized line; dropping." << std::endl;
            break;
        }

        size_t nlpos;
        while (!disconnect && (nlpos = acc.find('\n')) != std::string::npos) {
            std::string message = acc.substr(0, nlpos);
            acc.erase(0, nlpos + 1);
            if (message.empty()) continue;

            auto parts = parseMessage(message);
            if (parts.empty()) continue;

            std::string command = parts[0];

            // JOIN:username:characterType[:password[:clientTag]]
            // The native client sends five fields (`JOIN:Player6835:17:EDEN6:zr`);
            // `clientTag` is a constant `zr` we neither require nor interpret.
            if (command == "JOIN" && parts.size() >= 3) {
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
                    send(clientSocket, deny.c_str(), deny.length(), 0);
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
                        send(clientSocket, deny.c_str(), deny.length(), 0);
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
                    send(clientSocket, deny.c_str(), deny.length(), 0);
                    std::cout << "[Server] Rejected " << wanted << " from " << clientIP
                              << " (wrong password";
                    if (inWindow) std::cout << ", " << inWindow << " in window";
                    std::cout << ")." << std::endl;
                    disconnect = true;
                    continue;
                }

                // Reject a name already connected. g_playerPos is keyed by username,
                // so two players sharing one would also share (and clobber) a single
                // saved-position slot. Comparison is exact, matching that key.
                {
                    std::lock_guard<std::mutex> lock(clientsMutex);
                    bool taken = false;
                    for (const auto& kv : playerInfoMap)
                        if (kv.second.username == wanted) { taken = true; break; }
                    if (taken) {
                        // ⚠️ Our wording, not captured evidence — no real server has
                        // been observed refusing a duplicate name.
                        std::string deny = "[Server] Name already in use.\n";
                        send(clientSocket, deny.c_str(), deny.length(), 0);
                        std::cout << "[Server] Rejected " << wanted << " (name in use)." << std::endl;
                        disconnect = true;
                        continue;
                    }
                    username = wanted;
                    PlayerInfo pi{clientSocket, username, characterType};
                    pi.ip = clientIP;
                    playerInfoMap[clientSocket] = pi;
                }
                joined = true;

                // Handshake complete — relax the read timeout from the short
                // pre-JOIN window to the post-JOIN idle ceiling (or clear it if
                // --idle-timeout-conn is disabled). Stage 1.10.
                if (g_handshakeTimeout > 0)
                    setRecvTimeout(g_idleConnTimeout > 0 ? g_idleConnTimeout : 0);

                // --- join sequence, in the native pcap's order (stage 1.3) --------
                // 1. welcome
                std::string welcome = "[Server] Welcome, " + username + "! (Character Type: " + std::to_string(characterType) + ")\n";
                send(clientSocket, welcome.c_str(), welcome.length(), 0);

                // 2. capability advertisement. This is what tells the client to ask
                //    for terrain with REGION instead of expecting a push; VuencLink
                //    will not send a REGION until it sees this line.
                static const char* kCaps = "CAPS:region\n";
                send(clientSocket, kCaps, strlen(kCaps), 0);

                // 3. SPAWN — this name's saved position if it has one, otherwise
                //    the world's default spawn (eden_spawn.txt / --spawn, stage
                //    5.3). A returning player's own row always wins.
                {
                    std::lock_guard<std::mutex> lk(g_posMtx);
                    auto it = g_playerPos.find(username);
                    if (it != g_playerPos.end()) {
                        char sp[96];
                        int n = snprintf(sp, sizeof(sp), "SPAWN:%.2f:%.2f:%.2f\n",
                                         it->second.x, it->second.y, it->second.z);
                        send(clientSocket, sp, n, 0);
                        std::cout << "[Server] Restored " << username << " to ("
                                  << it->second.x << "," << it->second.y << "," << it->second.z << ")\n";
                    } else if (g_haveWorldSpawn) {
                        char sp[96];
                        int n = snprintf(sp, sizeof(sp), "SPAWN:%.2f:%.2f:%.2f\n",
                                         g_worldSpawn.x, g_worldSpawn.y, g_worldSpawn.z);
                        send(clientSocket, sp, n, 0);
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
                // history — to the whole server.
                if (msgContent[0] == '/') {
                    if (!joined) continue;
                    if (!g_weEnabled) { weSay(clientSocket, "Commands are disabled on this server."); continue; }
                    handleWorldEditLine(clientSocket, username, msgContent, we, regionLimiter);
                    continue;
                }

                std::string broadcastMsg = "[" + username + " (T" + std::to_string(characterType) + ")] " + msgContent + "\n";
                std::cout << broadcastMsg;
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
                    // Simulate the action into the authoritative world model so the
                    // server always has an accurate picture (handles TNT/paint
                    // explosions and burning too).
                    simAction(mode, x, y, z, extra);
                    // Relay the ORIGINAL action to everyone EXCEPT the sender (who
                    // already applied it locally). Peers re-simulate it themselves;
                    // the model above is what late joiners are snapshotted from.
                    broadcastMessage(broadcastMsg, clientSocket);
                } catch (...) {
                    std::cout << "[Server] Invalid ACTION message from " << username << std::endl;
                }
            }
            // POS:x:y:z
            else if (command == "POS" && parts.size() >= 4) {
                try {
                    float px = std::stof(parts[1]);
                    float py = std::stof(parts[2]);
                    float pz = std::stof(parts[3]);
                    {
                        std::lock_guard<std::mutex> lock(clientsMutex);
                        if (playerInfoMap.count(clientSocket)) {
                            playerInfoMap[clientSocket].posX = px;
                            playerInfoMap[clientSocket].posY = py;
                            playerInfoMap[clientSocket].posZ = pz;
                        }
                    }
                    rememberPos(username, px, py, pz);
                    std::string broadcastMsg = "POS:" + username + ":" + std::to_string(characterType) + ":" +
                                               parts[1] + ":" + parts[2] + ":" + parts[3] + "\n";
                    broadcastMessage(broadcastMsg, clientSocket);
                } catch (...) {
                    std::cout << "[Server] Invalid POS message from " << username << std::endl;
                }
            }
            // VEL:x:y:z
            else if (command == "VEL" && parts.size() >= 4) {
                try {
                    float vx = std::stof(parts[1]);
                    float vy = std::stof(parts[2]);
                    float vz = std::stof(parts[3]);
                    {
                        std::lock_guard<std::mutex> lock(clientsMutex);
                        if (playerInfoMap.count(clientSocket)) {
                            playerInfoMap[clientSocket].velX = vx;
                            playerInfoMap[clientSocket].velY = vy;
                            playerInfoMap[clientSocket].velZ = vz;
                        }
                    }
                    std::string broadcastMsg = "VEL:" + username + ":" + std::to_string(characterType) + ":" +
                                               parts[1] + ":" + parts[2] + ":" + parts[3] + "\n";
                    broadcastMessage(broadcastMsg, clientSocket);
                } catch (...) {
                    std::cout << "[Server] Invalid VEL message from " << username << std::endl;
                }
            }
            // POSVEL:px:py:pz:vx:vy:vz
            else if (command == "POSVEL" && parts.size() >= 7) {
                try {
                    float px = std::stof(parts[1]), py = std::stof(parts[2]), pz = std::stof(parts[3]);
                    float vx = std::stof(parts[4]), vy = std::stof(parts[5]), vz = std::stof(parts[6]);
                    {
                        std::lock_guard<std::mutex> lock(clientsMutex);
                        if (playerInfoMap.count(clientSocket)) {
                            auto& info = playerInfoMap[clientSocket];
                            info.posX = px; info.posY = py; info.posZ = pz;
                            info.velX = vx; info.velY = vy; info.velZ = vz;
                        }
                    }
                    rememberPos(username, px, py, pz);
                    std::string broadcastMsg = "POSVEL:" + username + ":" + std::to_string(characterType) + ":" +
                                               parts[1] + ":" + parts[2] + ":" + parts[3] + ":" +
                                               parts[4] + ":" + parts[5] + ":" + parts[6] + "\n";
                    broadcastMessage(broadcastMsg, clientSocket);
                } catch (...) {
                    std::cout << "[Server] Invalid POSVEL message from " << username << std::endl;
                }
            }
            // REGION:x:z — the client asks for the world around a point; we answer
            // with a SNAPZ burst. See serveRegion().
            else if (command == "REGION" && parts.size() >= 3) {
                // ⚠️ Gated on JOIN. Without this, an unauthenticated peer on a
                // passworded server could pull the entire world without ever
                // supplying the password — and REGION is the amplification vector,
                // so the cheapest place to require a handshake is here.
                if (!joined) {
                    if (g_verbose) std::cout << "[Server] REGION before JOIN from client "
                                             << clientId << "; ignored." << std::endl;
                    continue;
                }
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
                // Gated on JOIN for the same reason REGION is: a 6-byte request
                // answered with the entire sign file is an amplification vector, and
                // on a passworded server it would leak world content to a peer that
                // never supplied the password.
                if (!joined) {
                    if (g_verbose) std::cout << "[Server] SIGNQ before JOIN from client "
                                             << clientId << "; ignored." << std::endl;
                    continue;
                }
                serveSigns(clientSocket, username, signLimiter);
            }
            // PING -> PONG. Bare line, bare reply, no arguments echoed — this is what
            // the capture shows and it is what gives a client a real RTT.
            // ⚠️ Not the same thing as the *outbound* matchmaker `PING:<count>`
            // heartbeat in matchmakerThread(). Deliberately unlogged even under
            // --verbose: it is per-client and frequent (VuencLink sends one every
            // 10 s), and a log line per ping drowns everything else.
            else if (command == "PING") {
                static const char* kPong = "PONG\n";
                send(clientSocket, kPong, strlen(kPong), 0);
            }
            // Anything else — log it once per verb so a real client's un-modelled
            // messages (e.g. a sign-write we don't yet know the shape of — 1.9)
            // surface instead of being silently swallowed by this else-if chain.
            else if (g_verbose && !command.empty()) {
                static std::mutex seenMtx;
                static std::set<std::string> seen;
                std::lock_guard<std::mutex> lk(seenMtx);
                if (seen.insert(command).second) {
                    std::cout << "[Server] unrecognised line from "
                              << (joined ? username : ("client #" + std::to_string(clientId)))
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
    if (joined) {
        std::cout << "[Server] " << username << " disconnected." << std::endl;
        std::string leaveMsg = "[Server] " + username + " has left.\n";
        broadcastMessage(leaveMsg, clientSocket);
    } else if (g_verbose) {
        std::cout << "[Server] client #" << clientId << " closed without joining." << std::endl;
    }

    savePlayerPos();   // persist positions when someone leaves
    saveWorld();       // and persist the world model
    removeClient(clientSocket);
    close(clientSocket);
}

int main(int argc, char* argv[]) {
    std::cout << std::unitbuf;   // auto-flush so logs appear live under systemd/journald
    int port = DEFAULT_PORT;

    // Args: [port] and/or flags:
    //   --port N  --name "My World"  --password PASS  --world FILE  --signs FILE
    //   --spawn x:y:z  --spawn-file FILE  --max-world-cells N
    //   --matchmaker HOST[:PORT]
    //   --region-radius N  --no-region-sort  --no-region-empty-frame
    //   --action-rate N  --action-burst N   (0 = unlimited)
    //   --legacy-snapshot  --connect-limit N
    //   --auth-fail-limit N  --handshake-timeout N  --idle-timeout-conn N  (0 = off)
    //   --control-rate N  --control-burst N  --control-max-conns N  --audit-file FILE
    // A bare leading number is still accepted as the port (back-compat).
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* def)->std::string{ return (i+1<argc) ? std::string(argv[++i]) : std::string(def); };
        if      (a == "--port")       port = std::atoi(next("27015").c_str());
        else if (a == "--name")       g_serverName = next("Eden Server");
        else if (a == "--password")   g_password   = next("");
        else if (a == "--world")      g_worldFile  = next("eden_world.model");
        else if (a == "--signs")      g_signFile   = next("eden_signs.txt");
        else if (a == "--spawn-file") g_spawnFile  = next("eden_spawn.txt");
        else if (a == "--spawn") {
            // Inline default spawn; overrides (and skips) eden_spawn.txt.
            const std::string v = next("x:y:z");
            ewb::Spawn s;
            if (ewb::parse_spawn_line(v, s)) { g_worldSpawn = s; g_haveWorldSpawn = true; }
            else std::cerr << "[Server] --spawn " << v << " is not x:y:z; ignored." << std::endl;
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
        // Connection-lifecycle hardening (stage 1.10).
        else if (a == "--auth-fail-limit")   g_authFailLimit    = std::atoi(next("5").c_str());
        else if (a == "--handshake-timeout") g_handshakeTimeout = std::atoi(next("15").c_str());
        else if (a == "--idle-timeout-conn") g_idleConnTimeout  = std::atoi(next("300").c_str());
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
        else if (a == "--we-undo-budget")    g_weUndoBudget   = (size_t)std::atoll(next("2097152").c_str());
        else if (a == "--we-rate")           g_weCellRate     = std::atof(next("32768").c_str());
        else if (a == "--we-burst")          g_weCellBurst    = std::atof(next("262144").c_str());
        // REGION tuning. --region-radius exists because hosting our own server is
        // the only place the derived R = 224 can be varied experimentally without
        // burning someone else's CPU (plan §1.1); leave it alone for normal hosting,
        // since VuencLink's coverage lattice is built around 224.
        else if (a == "--region-radius") g_regionRadius = std::atoi(next("224").c_str());
        else if (a == "--no-region-sort") g_regionSort = false;
        // Empty regions are answered with a well-formed `SNAPZ:0:` by default (1.8
        // rung 2/3: VuencLink counts a real frame as "answered" and resets its
        // consecutive-empty abort; with silence it hangs on "waiting for the world
        // snapshot" and a 9-region sweep aborts three points in). --no-region-empty-frame
        // restores pre-1.8 silence for A/B testing against the real client (1.9).
        else if (a == "--region-empty-frame") g_regionEmptyFrame = true;   // back-compat no-op (now the default)
        else if (a == "--no-region-empty-frame") g_regionEmptyFrame = false;
        else if (a == "--action-rate")  SV_ACTION_RATE  = std::atof(next("512").c_str());
        else if (a == "--action-burst") SV_ACTION_BURST = std::atof(next("1024").c_str());
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

    // Clamp an operator typo before it becomes a wrapped box or a whole-world scan.
    if (g_regionRadius < 16 || g_regionRadius > 4096) {
        std::cerr << "[Server] --region-radius " << g_regionRadius
                  << " out of range (16..4096); using " << ewb::REGION_RADIUS << "." << std::endl;
        g_regionRadius = ewb::REGION_RADIUS;
    }
    if (g_regionRadius != ewb::REGION_RADIUS)
        std::cout << "[Server] REGION radius " << g_regionRadius
                  << " (non-default — VuencLink's coverage lattice assumes "
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
    if (g_maxWorldCells != SV_MAX_WORLD_CELLS_DEFAULT)
        std::cout << "[Server] Edited-cell cap " << g_maxWorldCells
                  << " (default " << SV_MAX_WORLD_CELLS_DEFAULT << ")" << std::endl;

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

    // Load the saved world model + player positions, and periodically persist them.
    loadWorld();
    if (g_world.size() > g_maxWorldCells)
        std::cerr << "[Server] loaded world has " << g_world.size() << " cells, over the "
                  << g_maxWorldCells << " cap — existing cells are kept, but new ones are"
                     " refused. Raise --max-world-cells to match the import." << std::endl;
    loadPlayerPos();
    loadSpawn();    // eden_spawn.txt: the default spawn for a player with no saved row (stage 5.3)
    loadSigns();   // sidecar; the control socket's `signs add|rm` writes it (stage 3.2)
    loadBans();
    loadOps();

    if (g_haveWorldSpawn)
        std::cout << "[Server] World spawn " << g_worldSpawn.x << ":" << g_worldSpawn.y << ":"
                  << g_worldSpawn.z << " (players with a saved position keep it)." << std::endl;
    if (g_legacySnapshot)
        std::cout << "[Server] --legacy-snapshot: pushing the plaintext ACTION world dump on JOIN."
                  << std::endl;
    if (g_connectLimit <= 0)
        std::cout << "[Server] --connect-limit 0: per-IP connect pacing disabled." << std::endl;
    g_authFail.set_threshold((size_t)std::max(g_authFailLimit, 0));
    if (g_authFailLimit <= 0)
        std::cout << "[Server] --auth-fail-limit 0: per-IP wrong-password lockout disabled." << std::endl;
    if (g_handshakeTimeout <= 0)
        std::cout << "[Server] --handshake-timeout 0: pre-JOIN read timeout disabled "
                     "(a connection can hold a slot until TCP keepalive reaps it)." << std::endl;
    else if (g_idleConnTimeout <= 0)
        std::cout << "[Server] --idle-timeout-conn 0: post-JOIN idle read timeout disabled." << std::endl;
    if (!g_regionEmptyFrame)
        std::cout << "[Server] --no-region-empty-frame: unbuilt regions answered with silence." << std::endl;
    if (SV_ACTION_RATE <= 0.0)
        std::cout << "[Server] --action-rate 0: per-connection ACTION rate limit disabled." << std::endl;
    else
        std::cout << "[Server] ACTION budget: " << SV_ACTION_RATE << "/s, "
                  << SV_ACTION_BURST << " burst (BURN costs " << SV_ACTION_COST_BURN << ")." << std::endl;

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
                        saveWorld();
                        savePlayerPos();
                        std::exit(0);
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

    while (serverRunning) {
        sockaddr_in clientAddr{};
        socklen_t clientAddrLen = sizeof(clientAddr);
        SOCKET clientSocket = accept(listenSocket, reinterpret_cast<sockaddr*>(&clientAddr), &clientAddrLen);

        if (clientSocket == INVALID_SOCKET) {
            if (serverRunning) std::cerr << "Accept failed: " << strerror(errno) << std::endl;
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
            const char* full = "[Server] Server full.\n";
            send(clientSocket, full, strlen(full), 0);
            close(clientSocket);
            std::cerr << "[Server] Rejected " << clientIP << " (server full)." << std::endl;
            continue;
        }

        // Detect peers that vanish without a FIN so their thread can exit.
        int ka = 1; setsockopt(clientSocket, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof(ka));

        int clientId = ++clientIdCounter;
        std::cout << "[Server] Client #" << clientId << " connected from " << clientIP << std::endl;

        {
            std::lock_guard<std::mutex> lock(clientsMutex);
            clients.push_back(clientSocket);
        }

        // Detached; handleClient does its own cleanup (removeClient + close) on
        // exit, so no thread handle is retained and nothing accumulates.
        std::thread(handleClient, clientSocket, clientId, std::string(clientIP)).detach();
    }

    close(listenSocket);
    std::cout << "Server terminated." << std::endl;
    return 0;
}
