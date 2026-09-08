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
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <unistd.h>
#include <cstring>
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

#include "region_query.h"   // REGION reply geometry + Cell -> record table (stage 1.1)
#include "snapz_codec.h"    // raw DEFLATE + base64 + SNAPZ framing      (stage 1.2)
#include "sign_store.h"     // eden_signs.txt + SIGNQ -> SIGNP           (stage 1.5)
#include "hardening.h"      // names, token buckets, ACTION validation   (stage 1.7)

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
bool        g_legacySnapshot = false;            // push the ACTION dump on JOIN (--legacy-snapshot)
int         g_connectLimit = 10;                 // connects per IP per window; 0 = off (--connect-limit)

// Safety limits (hardening for public hosting).
static const int    SV_WORLD_HEIGHT   = 256;       // must match the game's T_HEIGHT
static const size_t SV_MAX_WORLD_CELLS= 4000000;   // cap edited-cell count (mem/disk guard)
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
    if(g_world.find(k)==g_world.end() && g_world.size()>=SV_MAX_WORLD_CELLS){
        static bool warned=false;
        if(!warned){ std::cerr << "[Server] world cell cap reached (" << SV_MAX_WORLD_CELLS
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
void matchmakerThread() {
    while (serverRunning) {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s >= 0) {
            sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(g_matchPort);
            if (inet_pton(AF_INET, g_matchHost.c_str(), &a.sin_addr) > 0 &&
                connect(s, (sockaddr*)&a, sizeof(a)) == 0) {
                std::string reg = "REGISTER:" + g_serverName + ":" + std::to_string(g_port) + ":" +
                                  (g_password.empty() ? "0" : "1") + ":" + g_advertiseIP + "\n";
                send(s, reg.c_str(), reg.size(), 0);
                std::cout << "[Server] Registered with matchmaker " << g_matchHost << ":" << g_matchPort
                          << " as \"" << g_serverName << "\"" << (g_password.empty()?"":" [locked]") << std::endl;
                // Heartbeat: PING with the current player count so the matchmaker can
                // keep us listed and sort by popularity. Send one immediately, then
                // every ~15s (well inside the matchmaker's TTL).
                while (serverRunning) {
                    size_t n; { std::lock_guard<std::mutex> lock(clientsMutex); n = clients.size(); }
                    std::string ping = "PING:" + std::to_string(n) + "\n";
                    if (send(s, ping.c_str(), ping.size(), 0) <= 0) break;
                    for(int i=0;i<15 && serverRunning;i++) std::this_thread::sleep_for(std::chrono::seconds(1));
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

void handleClient(SOCKET clientSocket, int clientId) {
    char recvBuffer[BUFFER_SIZE];
    std::string username = "Player" + std::to_string(clientId);
    int characterType = 0;
    bool joined = false;          // a successful JOIN gates REGION/SIGNQ (see below)
    BurstLimiter regionLimiter;   // per-connection REGION pacing
    BurstLimiter signLimiter;     // ...and SIGNQ pacing
    ewb::TokenBucket actionBucket(SV_ACTION_BURST, SV_ACTION_RATE);   // stage 1.7
    bool actionWarned = false;    // log the first refusal per connection, not each one

    // Client->server messages are newline-framed: accumulate bytes and process
    // one complete '\n'-terminated line at a time. This makes parsing robust to
    // TCP coalescing (e.g. a POSVEL and an ACTION arriving in one recv()).
    std::string acc;
    bool disconnect = false;
    while (serverRunning && !disconnect) {
        int bytesReceived = recv(clientSocket, recvBuffer, BUFFER_SIZE - 1, 0);
        if (bytesReceived <= 0) break;
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
                if (!g_password.empty() && providedPw != g_password) {
                    std::string deny = "[Server] Wrong password.\n";
                    send(clientSocket, deny.c_str(), deny.length(), 0);
                    std::cout << "[Server] Rejected " << wanted << " (wrong password)." << std::endl;
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
                    playerInfoMap[clientSocket] = { clientSocket, username, characterType };
                }
                joined = true;

                // --- join sequence, in the native pcap's order (stage 1.3) --------
                // 1. welcome
                std::string welcome = "[Server] Welcome, " + username + "! (Character Type: " + std::to_string(characterType) + ")\n";
                send(clientSocket, welcome.c_str(), welcome.length(), 0);

                // 2. capability advertisement. This is what tells the client to ask
                //    for terrain with REGION instead of expecting a push; VuencLink
                //    will not send a REGION until it sees this line.
                static const char* kCaps = "CAPS:region\n";
                send(clientSocket, kCaps, strlen(kCaps), 0);

                // 3. SPAWN — only if this name has a saved position.
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
    //   --matchmaker HOST[:PORT]
    //   --region-radius N  --no-region-sort  --no-region-empty-frame
    //   --action-rate N  --action-burst N   (0 = unlimited)
    //   --legacy-snapshot  --connect-limit N
    // A bare leading number is still accepted as the port (back-compat).
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* def)->std::string{ return (i+1<argc) ? std::string(argv[++i]) : std::string(def); };
        if      (a == "--port")       port = std::atoi(next("27015").c_str());
        else if (a == "--name")       g_serverName = next("Eden Server");
        else if (a == "--password")   g_password   = next("");
        else if (a == "--world")      g_worldFile  = next("eden_world.model");
        else if (a == "--signs")      g_signFile   = next("eden_signs.txt");
        else if (a == "--verbose")    g_verbose    = true;
        // Push the plaintext ACTION world dump on JOIN (pre-1.3 behaviour). Off by
        // default: the real client asks for terrain with REGION and is answered with
        // SNAPZ, and this dump's wire shape has never been captured. Kept as a
        // bring-up fallback — see sendWorldSnapshot().
        else if (a == "--legacy-snapshot") g_legacySnapshot = true;
        else if (a == "--connect-limit") g_connectLimit = std::atoi(next("10").c_str());
        else if (a == "--idle-timeout") g_idleTimeout = std::atoi(next("0").c_str());
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
    loadPlayerPos();
    loadSigns();   // read-only sidecar; nothing writes it in Phase 1

    if (g_legacySnapshot)
        std::cout << "[Server] --legacy-snapshot: pushing the plaintext ACTION world dump on JOIN."
                  << std::endl;
    if (g_connectLimit <= 0)
        std::cout << "[Server] --connect-limit 0: per-IP connect pacing disabled." << std::endl;
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
        std::thread(handleClient, clientSocket, clientId).detach();
    }

    close(listenSocket);
    std::cout << "Server terminated." << std::endl;
    return 0;
}
