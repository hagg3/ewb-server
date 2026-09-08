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
//   Server -> Clients (broadcast, '\n' terminated):
//     POS:username:type:x:y:z
//     VEL:username:type:x:y:z
//     POSVEL:username:type:px:py:pz:vx:vy:vz
//     ACTION:username:type:x:y:z:mode[:typeOrColor]
//     [Server] ... / [username (Tn)] chat text

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
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <sstream>
#include <fstream>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <iostream>
#include <vector>
#include <string>
#include <cmath>      // Required for std::round
#include <algorithm>  // Required for containers
#include <mutex>
#include <sys/socket.h>
#include <ctime>

std::vector<std::string> helpCommands = {
    "/help  <page>       - Lists commands",
    "/helpp <page>       - Lists more commands at once (PC Only)",

    "/msg <username> <msg> - Privately sends message",
    "/r, /reply <msg>      - Privately replies to a /msg",

    "/tp <x> <y> <z>   - Teleport absolute",
    "/tp ~x ~y ~z        - Teleport relative",

    "/tp <username>   - Teleport to player",
    "Note: You can mix relative and absolute #'s, not usernames.",

    "Note: WorldEdit only detects player-made blocks.",
    "Note: Blocks and Colors can be an ID or Name ",

    "/searchblocks <name> - finds closest matching valid block name",
    "/searchcolors <name> - finds closest matching valid color name",

    "/id <name> - reveals numeric id of block or color",
    "/resync    - Refresh world",

    "Note: ID's and Names can be mixed in the same command",
    "Note: You can shuffle multiple ID's if they're comma separated",

    "//pos1    - Set first selection point",
    "//pos2    - Set second selection point",

    "//set <block> [color] - Fill selection",
    "//walls <block> [color] - Build walls",

    "//replace <old> <new> [color] [colorFilter] - Swap blocks",
    "//replacenear <radius> <old> <new> [color] [colorFilter] - Swap near",

    "//paint <color>    - Paint selection",
    "//unpaint, //strip - Unpaints selection",

    "//undo     - Redo worldedit actions",
    "//redo     - Undo worldedit actions",

    "//copy     - Copy selection relative to location",
    "//paste    - Paste selection relative to location",

    "//rotate <angle>   - Rotate clipboard",
    "//up <dist>           - Teleport up with a floating support",

    "//sphere <block> <radius> [color]  - Make a sphere",
    "//cyl <block> <radius> [height] [color] - Make a cylinder",

    "//hsphere <block> <radius> [color] - Make a hollow sphere",
    "//hcyl <block> <radius> [height] [color] - Make a hollow cylinder"

};

typedef int SOCKET;
constexpr SOCKET INVALID_SOCKET = -1;
constexpr int SOCKET_ERROR = -1;

constexpr int DEFAULT_PORT = 27015;
constexpr int BUFFER_SIZE = 512;

// Max blocks that can be modified in a single WorldEdit command
const int MAX_WORLDEDIT_BLOCKS = 100000;

// Stores a single block change for Undo/Redo history
struct BlockEdit {
    int x, y, z;
    int oldType, oldColor;
    int newType, newColor;
};

// Stores a copied block relative to the player's position
struct ClipboardBlock {
    int rx, ry, rz;
    int type, color;
};

// Player info structure
struct PlayerInfo {
    SOCKET socket;
    std::string username;
    std::string clientIP;
    int characterType;
    float posX = 0, posY = 0, posZ = 0;
    float velX = 0, velY = 0, velZ = 0;

    std::string lastMessaged;

    // WorldEdit selection & memory
    bool hasPos1 = false, hasPos2 = false;
    int p1x = 0, p1y = 0, p1z = 0;
    int p2x = 0, p2y = 0, p2z = 0;

    std::vector<std::vector<BlockEdit>> undoHistory;
    std::vector<std::vector<BlockEdit>> redoHistory;
    std::vector<ClipboardBlock> clipboard;
};

static const std::unordered_map<std::string, int> BLOCK_MAP = {
    {"air", 0},
    {"bedrock", 1},
    {"adminium", 1},
    {"stone", 2},
    {"dirt", 3},
    {"sand", 4},
    {"leaves", 5},
    {"log", 6},
    {"planks", 7},
    {"grass", 8},
    {"tnt", 9},
    {"darkstone", 10},
    {"clover", 11},
    {"grass2", 12},
    {"brick", 13},
    {"tile", 14},
    {"ice", 15},
    {"gem", 16},
    {"spring", 17},
    {"ladder", 18},
    {"blank", 19},
    {"water", 20},
    {"lattice", 21},
    {"vines", 22},
    {"lava", 23},
    {"stone_slope_n", 24},
    {"stone_slope_e", 25},
    {"stone_slope_s", 26},
    {"stone_slope_w", 27},
    {"planks_slope_n", 28},
    {"planks_slope_e", 29},
    {"planks_slope_s", 30},
    {"planks_slope_w", 31},
    {"stonebrick_slope_n", 32},
    {"stonebrick_slope_e", 33},
    {"stonebrick_slope_s", 34},
    {"stonebrick_slope_w", 35},
    {"ice_slope_n", 36},
    {"ice_slope_e", 37},
    {"ice_slope_s", 38},
    {"ice_slope_w", 39},
    {"stone_slope_nw", 40},
    {"stone_slope_ne", 41},
    {"stone_slope_se", 42},
    {"stone_slope_sw", 43},
    {"planks_slope_nw", 44},
    {"planks_slope_ne", 45},
    {"planks_slope_se", 46},
    {"planks_slope_sw", 47},
    {"stonebrick_slope_nw", 48},
    {"stonebrick_slope_ne", 49},
    {"stonebrick_slope_se", 50},
    {"stonebrick_slope_sw", 51},
    {"ice_slope_nw", 52},
    {"ice_slope_ne", 53},
    {"ice_slope_se", 54},
    {"ice_slope_sw", 55},
    {"stonebrick", 56},
    {"shade", 57},
    {"glass", 58},
    {"water2", 59},
    {"water3", 60},
    {"water4", 61},
    {"lava2", 62},
    {"lava3", 63},
    {"lava4", 64},
    {"firework", 65},
    {"???", 66},
    {"????", 67},
    {"?????", 68},
    {"??????", 69},
    {"door", 70},
    {"gem", 71},
    {"lamp", 72},
    {"flower", 73},
    {"metal", 74},
    {"portal1", 75},
    {"portal2", 76},
    {"portal3", 77},
    {"portal4", 78},
    {"portal5", 79},
    {"petrified_leaves", 80},
    {"exploding_expand", 81},
    {"expand_grass", 82},
    {"expand_darkstone", 83},
    {"expand_stone", 84},
    {"expand_dirt", 85},
    {"expand_sand", 86},
    {"expand_tnt", 87},
    {"expand_planks", 88},
    {"expand_stonebrick", 89},
    {"expand_glass", 90},
    {"expand_shade", 91},
    {"expand_log", 92},
    {"expand_leaves", 93},
    {"expand_brick", 94},
    {"expand_tile", 95},
    {"expand_moss", 96},
    {"expand_ladder", 97},
    {"expand_ice", 98},
    {"expand_gem", 99},
    {"expand_spring", 100},
    {"expand_blank", 101},
    {"expand_stone_curve", 102},
    {"expand_planks_curve", 103},
    {"expand_ice_curve", 104},
    {"expand_stonebrick_curve", 105},
    {"expand_lattice", 106},
    {"expand_water", 107},
    {"expand_lava", 108},
    {"expand_firework", 109},
    {"expand_lamp", 110},
    {"expand_metal", 111}
};
// Translates standard Eden World Builder blocks to their IDs
int getBlockId(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
    auto it = BLOCK_MAP.find(name);
    if (it != BLOCK_MAP.end()) return it->second ;
    try { return std::stoi(name); } catch (...) { return -1; }
}

static const std::unordered_map<std::string, int> COLOR_MAP = { // this list is off by 1
    { "none", -1  }, { "unpaint", -1  }, { "base", -1  }, { "original", -1  }, { "scrape", -1  },
    // Pale (Top)
    {"pale_red", 0}, {"pale_orange", 1}, {"pale_yellow", 2}, {"pale_green", 3},
    {"pale_cyan", 4}, {"pale_blue", 5}, {"pale_purple", 6}, {"pale_pink", 7}, {"white", 8},
    // Light
    {"light_red", 9}, {"light_orange", 10}, {"light_yellow", 11}, {"light_green", 12},
    {"light_cyan", 13}, {"light_blue", 14}, {"light_purple", 15}, {"light_pink", 16}, {"light_grey", 17},
    // Base (Middle)
    {"red", 18}, {"orange", 19}, {"yellow", 20}, {"green", 21},
    {"cyan", 22}, {"blue", 23}, {"purple", 24}, {"pink", 25}, {"grey", 26},
    // Dark
    {"dark_red", 27}, {"dark_orange", 28}, {"dark_yellow", 29}, {"dark_green", 30},
    {"dark_cyan", 31}, {"dark_blue", 32}, {"dark_purple", 33}, {"dark_pink", 34}, {"dark_grey", 35},
    // Deep
    {"deep_red", 36}, {"deep_orange", 37}, {"deep_yellow", 38}, {"deep_green", 39},
    {"deep_cyan", 40}, {"deep_blue", 41}, {"deep_purple", 42}, {"deep_pink", 43}, {"deep_grey", 44},
    // Darkest (Lowest)
    {"darkest_red", 45}, {"darkest_orange", 46}, {"darkest_yellow", 47}, {"darkest_green", 48},
    {"darkest_cyan", 49}, {"darkest_blue", 50}, {"darkest_purple", 51}, {"darkest_pink", 52}, {"black", 53}
};

const char* charNames[] = {
    "Moof",
    "Batty",
    "Green",
    "Nergle",
    "Stumpy",
    "Charger",
    "Stalker"
};

// Translates basic color names to Eden color IDs (Base 0-54 palette)
int getColorId(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
    // Eden Color Palette: String-to-ID Mapping
    // Useful for command parsing (e.g., //paint red)
    auto it = COLOR_MAP.find(name);
    if (it != COLOR_MAP.end()) return it->second + 1;
    try { return std::stoi(name); } catch (...) { return -1; }
}

// Helper to search map keys for a partial string
std::string searchMap(const std::unordered_map<std::string, int>& map, const std::string& query) {
    std::string results;
    int count = 0;
    for (const auto& pair : map) {
        if (pair.first.find(query) != std::string::npos) {
            if (!results.empty()) results += ", ";
            results += pair.first;
            count++;
            // Limit results to prevent massive chat spam
            if (count >= 10) { results += "... (too many)"; break; }
        }
    }
    return results.empty() ? "No matches found." : results;
}

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

// Safety limits (hardening for public hosting).
static const int    SV_WORLD_HEIGHT   = 256;       // must match the game's T_HEIGHT
static const size_t SV_MAX_WORLD_CELLS= 4000000;   // cap edited-cell count (mem/disk guard)
static const size_t SV_MAX_LINE       = 8192;      // drop a client flooding without '\n'
static const int    SV_MAX_CLIENTS    = 64;        // reject connections beyond this

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

void sendWorldSnapshot(SOCKET clientSocket) {
    // Build the whole snapshot into ONE buffer and send it in bulk, instead of a
    // send() syscall per cell. For a heavily-edited world that's the difference
    // between tens of thousands of tiny syscalls (slow) and a handful of big writes.
    std::string blob;
    size_t cells = 0;
    {
        std::lock_guard<std::mutex> lock(g_worldMtx);
        cells = g_world.size();
        blob.reserve(cells * 32);   // ~one line per cell
        char line[96];
        for (const auto& kv : g_world) {
            int x,y,z; wunkey(kv.first,x,y,z);
            const Cell& c = kv.second;
            // "server" sender so the client never mistakes these for its own echoes.
            if (c.type == SV_AIR) {
                blob.append(line, snprintf(line, sizeof(line), "ACTION:server:0:%d:%d:%d:1\n", x,y,z));                 // mine
            } else if (c.type == SV_PAINTED_BASE) {
                blob.append(line, snprintf(line, sizeof(line), "ACTION:server:0:%d:%d:%d:3:%d\n", x,y,z,(int)c.color)); // paint base
            } else {
                blob.append(line, snprintf(line, sizeof(line), "ACTION:server:0:%d:%d:%d:0:%d\n", x,y,z,(int)c.type));  // build
                if (c.color != 0)
                    blob.append(line, snprintf(line, sizeof(line), "ACTION:server:0:%d:%d:%d:3:%d\n", x,y,z,(int)c.color)); // then paint
            }
        }
    }
    sendAll(clientSocket, blob.data(), blob.size());
    std::cout << "[Server] Sent world snapshot (" << cells << " cells, " << blob.size()
              << " bytes) to new player." << std::endl;
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


float parseCoord(const std::string& input, float currentPos) {
    if (input.empty()) return currentPos;

    // Check if it starts with '~'
    if (input[0] == '~') {
        // If just "~", return current position (offset 0)
        if (input.length() == 1) return currentPos;
        // Otherwise, add the relative offset
        return currentPos + std::stof(input.substr(1));
    }

    // Fallback to absolute position
    return std::stof(input);
}

// Revised Helper: Use this to broadcast block changes
void applyBlockChanges(const std::vector<BlockEdit>& changes) {
    std::string blob;
    char line[128];
    {
        std::lock_guard<std::mutex> lock(g_worldMtx);
        for (const auto& edit : changes) {
            uint64_t k = wkey(edit.x, edit.y, edit.z);

            // 1. Update Model
            if (edit.newType == 0) g_world.erase(k);
            else g_world[k] = { (unsigned char)edit.newType, (unsigned char)edit.newColor };
            editsDirty = true;

            // 2. Prepare Packets (Always clear first, then rebuild)
            blob.append(line, snprintf(line, sizeof(line), "ACTION:server:0:%d:%d:%d:1\n", edit.x, edit.y, edit.z));
            if (edit.newType != 0) {
                blob.append(line, snprintf(line, sizeof(line), "ACTION:server:0:%d:%d:%d:0:%d\n", edit.x, edit.y, edit.z, edit.newType));
                if (edit.newColor != 0)
                    blob.append(line, snprintf(line, sizeof(line), "ACTION:server:0:%d:%d:%d:3:%d\n", edit.x, edit.y, edit.z, edit.newColor));
            }
        }
    }

    // 3. Thread-safe Broadcast (Do not hold clientsMutex while sending data)
    std::vector<SOCKET> snapshot;
    {
        std::lock_guard<std::mutex> lock(clientsMutex);
        snapshot = clients;
    }
    for (SOCKET c : snapshot) sendAll(c, blob.data(), blob.size());
}

// Rotates a slope block ID 90 degrees clockwise
int rotateSlope(int type) {
    // Cardinal Slopes: 24-27 (Stone), 28-31 (Planks), 32-35 (Stonebrick), 36-39 (Ice)
    // Diagonal Slopes: 40-43 (Stone), 44-47 (Planks), 48-51 (Stonebrick), 52-55 (Ice)

    // Check Cardinal ranges
    for (int start : {24, 28, 32, 36}) {
        if (type >= start && type <= start + 3) {
            return start + ((type - start + 1) % 4);
        }
    }
    // Check Diagonal ranges
    for (int start : {40, 44, 48, 52}) {
        if (type >= start && type <= start + 3) {
            return start + ((type - start + 1) % 4);
        }
    }
    return type; // Not a slope, return original ID
}

void handleClient(SOCKET clientSocket, int clientId) {
    char recvBuffer[BUFFER_SIZE];
    std::string username = "Player" + std::to_string(clientId);
    int characterType = 0;

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

            // JOIN:username:characterType[:password]// JOIN:username:characterType[:password]
            if (command == "JOIN" && parts.size() >= 3) {
                username = parts[1];
                try {
                    characterType = std::stoi(parts[2]);
                    if (characterType < 0 || characterType > 6) characterType = 0;
                } catch (...) {
                    characterType = 0;
                }

                // Password check (if this server is protected).
                std::string providedPw = (parts.size() >= 4) ? parts[3] : "";
                if (!g_password.empty() && providedPw != g_password) {
                    std::string deny = "[Server] Wrong password.\n";
                    send(clientSocket, deny.c_str(), deny.length(), 0);
                    std::cout << "[Server] Rejected " << username << " (wrong password)." << std::endl;
                    disconnect = true;
                    continue;
                }

                {
                    std::lock_guard<std::mutex> lock(clientsMutex);
                    playerInfoMap[clientSocket] = { clientSocket, username, characterType };
                }

                auto sendLine = [&](const std::string& line) {
                    std::string p = "[Server] " + line + "\n";
                    send(clientSocket, p.c_str(), p.size(), 0);
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                };

                // Format the server time (12-hour HH:MM AM/PM)
                auto now = std::chrono::system_clock::now();
                std::time_t now_time = std::chrono::system_clock::to_time_t(now);
                char timeBuf[64];
                std::strftime(timeBuf, sizeof(timeBuf), "%I:%M %p", std::localtime(&now_time));

                // Translate ID to Name
                std::string charName = (characterType >= 0 && characterType <= 6) ? charNames[characterType] : "Unknown";

                // Send expanded welcome to the user (Reordered)
                std::string welcome1 = "-- Server Time: " + std::string(timeBuf) + " --\n";
                sendLine(welcome1);
                std::string welcome2 = "Character: " + charName + "\n";

                sendLine(welcome2);
                // Send the green help prompt last so it stays visible at the bottom of the chat
                std::string welcome3 = "Welcome, " + username + "! Type /help for help\n";
                sendLine(welcome3);

                // Broadcast updated join message to other players using the real name
                std::string joinMsg = "[Server] " + username + " (" + charName + ") has joined.\n";
                std::cout << joinMsg;
                broadcastMessage(joinMsg, clientSocket);
                // -----------------------------------------------

                // Send the current world model. Live edits are relayed from here on.
                sendWorldSnapshot(clientSocket);

                // Restore this player's last position (by username) so they respawn
                // where they left off.
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
            }


            // MSG:text
            else if (command == "MSG" && parts.size() >= 2) {
                std::string msgContent = message.substr(4); // Skip "MSG:"

                // 1. Check for Command Prefix
                if (!msgContent.empty() && msgContent[0] == '/') {
                    std::stringstream ss(msgContent);
                    std::string cmd;
                    ss >> cmd;

                    // --- WorldEdit Core Helper (Updated for History) ---
                    auto doWorldEdit = [&](int x1, int y1, int z1, int x2, int y2, int z2, auto func) {
                        int minX = std::min(x1, x2), maxX = std::max(x1, x2);
                        int minY = std::min(y1, y2), maxY = std::max(y1, y2);
                        int minZ = std::min(z1, z2), maxZ = std::max(z1, z2);

                        if ((maxX - minX + 1LL) * (maxY - minY + 1LL) * (maxZ - minZ + 1LL) > MAX_WORLDEDIT_BLOCKS) {
                            std::string err = "[Server] Area too large (max " + std::to_string(MAX_WORLDEDIT_BLOCKS) + " blocks).\n";
                            send(clientSocket, err.c_str(), err.size(), 0);
                            return;
                        }

                        std::vector<BlockEdit> sessionHistory;

                        {
                            std::lock_guard<std::mutex> lock(g_worldMtx);
                            for (int x = minX; x <= maxX; ++x) {
                                for (int y = minY; y <= maxY; ++y) {
                                    for (int z = minZ; z <= maxZ; ++z) {
                                        if (y < 0 || y >= SV_WORLD_HEIGHT || x < 0 || z < 0 || x > 0xFFFFFF || z > 0xFFFFFF) continue;

                                        uint64_t k = wkey(x, y, z);
                                        int currentType = 0, currentColor = 0;
                                        auto it = g_world.find(k);
                                        if (it != g_world.end()) {
                                            currentType = it->second.type;
                                            currentColor = it->second.color;
                                        }

                                        int newType = currentType;
                                        int newColor = currentColor;

                                        if (func(x, y, z, currentType, currentColor, newType, newColor)) {
                                            if (newType == currentType && newColor == currentColor) continue;

                                            // Save to history vector before applying
                                            sessionHistory.push_back({x, y, z, currentType, currentColor, newType, newColor});
                                        }
                                    }
                                }
                            }
                        }

                        // Push history & trigger application
                        if (!sessionHistory.empty()) {
                            // 1. Save data to a local variable
                            // 2. Perform the history update WITHOUT holding the g_worldMtx or clientsMutex
                            // (Wait to update history until after application to keep scope clean)

                            applyBlockChanges(sessionHistory); // Safe: This locks/unlocks internally now

                            std::lock_guard<std::mutex> lock(clientsMutex);
                            auto& info = playerInfoMap[clientSocket];
                            info.undoHistory.push_back(sessionHistory);
                            if (info.undoHistory.size() > 10) info.undoHistory.erase(info.undoHistory.begin());
                            info.redoHistory.clear();
                        }

                        std::string msg = "[WorldEdit] Updated " + std::to_string(sessionHistory.size()) + " blocks.\n";
                        send(clientSocket, msg.c_str(), msg.size(), 0);
                    };

                    // Selection Helper
                    auto getSelection = [&](int& x1, int& y1, int& z1, int& x2, int& y2, int& z2) -> bool {
                        std::lock_guard<std::mutex> lock(clientsMutex);
                        auto& info = playerInfoMap[clientSocket];
                        if (!info.hasPos1 || !info.hasPos2) {
                            std::string err = "[Server] You must set //pos1 and //pos2 first.\n";
                            send(clientSocket, err.c_str(), err.size(), 0);
                            return false;
                        }
                        x1 = info.p1x; y1 = info.p1y; z1 = info.p1z;
                        x2 = info.p2x; y2 = info.p2y; z2 = info.p2z;
                        return true;
                    };


                    // 2. Commands Implementation
                    std::string arg1, arg2, arg3;

                    if (cmd == "/help" || cmd == "/helpp") {
                        int page = 1;
                        if (ss >> arg1) { try { page = std::stoi(arg1); } catch (...) {} }

                        // Set mode: /help = 1 (3 lines), /helpp = 2 (5 lines)
                        int mode = (cmd == "/help") ? 1 : 2;

                        auto sendLine = [&](const std::string& line) {
                            std::string p = "[Server] " + line + "\n";
                            send(clientSocket, p.c_str(), p.size(), 0);
                            std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        };

                        int linesPerPage = (mode == 1) ? 3 : 5;
                        int itemsPerPage = linesPerPage - 1;

                        // Calculate total pages using integer arithmetic
                        int totalPages = (helpCommands.size() + itemsPerPage - 1) / itemsPerPage;

                        if (page < 1 || page > totalPages) {
                            sendLine("Invalid page.");
                        } else {
                            sendLine("--- Help (" + std::to_string(page) + "/" + std::to_string(totalPages) + ") ---");

                            int startIdx = (page - 1) * itemsPerPage;
                            for (int i = 0; i < itemsPerPage; ++i) {
                                int currentIndex = startIdx + i;
                                if (currentIndex < helpCommands.size()) {
                                    sendLine(helpCommands[currentIndex]);
                                }
                            }
                        }
                    }

                    else if (cmd == "/msg" || cmd == "/r" || cmd == "/reply") {
                        std::string targetUser;
                        std::string privateMsg;

                        if (cmd == "/msg") {
                            if (!(ss >> targetUser)) {
                                send(clientSocket, "[Server] Usage: /msg <username> <message>\n", 42, 0);
                                continue;
                            }
                            std::getline(ss, privateMsg); // captures remaining text
                        } else {
                            // Reply logic
                            {
                                std::lock_guard<std::mutex> lock(clientsMutex);
                                targetUser = playerInfoMap[clientSocket].lastMessaged;
                            }
                            if (targetUser.empty()) {
                                send(clientSocket, "[Server] No one to reply to.\n", 29, 0);
                                continue;
                            }
                            std::getline(ss, privateMsg);
                        }

                        // 1. Trim the leading space left by std::getline
                        if (!privateMsg.empty() && privateMsg[0] == ' ') {
                            privateMsg.erase(0, 1);
                        }

                        SOCKET targetSocket = -1;
                        {
                            std::lock_guard<std::mutex> lock(clientsMutex);
                            // 2. Use auto& (non-const) to update values safely without double map lookups
                            for (auto& [sock, info] : playerInfoMap) {
                                if (info.username == targetUser) {
                                    targetSocket = sock;

                                    // Update conversation history on both ends
                                    info.lastMessaged = username;
                                    playerInfoMap[clientSocket].lastMessaged = targetUser;
                                    break;
                                }
                            }
                        }

                        if (targetSocket != -1) {
                            // 3. Fix the missing spaces and brackets so the client renders it properly
                            std::string out = "[You tell " + targetUser + "] " + privateMsg + "\n";
                            send(clientSocket, out.c_str(), out.size(), 0);

                            std::string in = "[" + username + " whisphers] " + privateMsg + "\n";
                            send(targetSocket, in.c_str(), in.size(), 0);
                        } else {
                            send(clientSocket, "[Server] Player not found.\n", 27, 0);
                        }
                    }

                    else if (cmd == "/tp" && ss >> arg1) {
                        float tx, ty, tz;
                        bool valid = false;
                        bool isCoord = !arg1.empty() && (isdigit(arg1[0]) || arg1[0] == '~' || arg1[0] == '-');

                        {
                            std::lock_guard<std::mutex> lk(g_posMtx);
                            if (!g_playerPos.count(username)) {
                                std::string err = "ERROR: Server lacks your position.\n";
                                send(clientSocket, err.c_str(), err.size(), 0);
                                continue;
                            }
                            if (isCoord && ss >> arg2 >> arg3) {
                                tx = parseCoord(arg1, g_playerPos[username].x);
                                ty = parseCoord(arg2, g_playerPos[username].y);
                                tz = parseCoord(arg3, g_playerPos[username].z);
                                valid = true;
                            } else {
                                if (g_playerPos.count(arg1)) {
                                    tx = g_playerPos[arg1].x; ty = g_playerPos[arg1].y; tz = g_playerPos[arg1].z;
                                    valid = true;
                                } else {
                                    std::string err = "ERROR: Player '" + arg1 + "' not found.\n";
                                    send(clientSocket, err.c_str(), err.size(), 0);
                                }
                            }
                        }

                        if (valid) {
                            // --- ADDED BOUNDARY CHECK ---
                            if (ty < 0 || ty >= SV_WORLD_HEIGHT || tx < 0 || tx > 0xFFFFFF || tz < 0 || tz > 0xFFFFFF) {
                                std::string err = "[Server] Teleport failed: Destination out of bounds.\n";
                                send(clientSocket, err.c_str(), err.size(), 0);
                            } else {
                                rememberPos(username, tx, ty, tz);
                                char sp[96];
                                int n = snprintf(sp, sizeof(sp), "SPAWN:%.2f:%.2f:%.2f\n", tx, ty, tz);
                                send(clientSocket, sp, n, 0);
                            }
                        }
                    }

                    else if (cmd == "//pos1" || cmd == "//pos2") {
                        int px, py, pz;
                        {
                            std::lock_guard<std::mutex> lock(clientsMutex);
                            auto& info = playerInfoMap[clientSocket];
                            px = std::round(info.posX);
                            py = std::round(info.posY - 1.0f); // Lowered to feet level
                            pz = std::round(info.posZ);

                            if (cmd == "//pos1") {
                                info.hasPos1 = true; info.p1x = px; info.p1y = py; info.p1z = pz;
                            } else {
                                info.hasPos2 = true; info.p2x = px; info.p2y = py; info.p2z = pz;
                            }
                        }
                        std::string msg = "[Server] " + cmd.substr(2) + " set to " + std::to_string(px) + ", " + std::to_string(py) + ", " + std::to_string(pz) + ".\n";
                        send(clientSocket, msg.c_str(), msg.size(), 0);
                    }

                    else if (cmd == "//set" && ss >> arg1) {
                        int blockId = getBlockId(arg1);
                        if (blockId == -1) continue;

                        // Capture optional color argument
                        int colorId = 0;
                        if (ss >> arg2) {
                            colorId = getColorId(arg2);
                        }

                        int x1, y1, z1, x2, y2, z2;
                        if (getSelection(x1, y1, z1, x2, y2, z2)) {
                            // Capture blockId and colorId in the lambda
                            doWorldEdit(x1, y1, z1, x2, y2, z2, [blockId, colorId](int, int, int, int, int, int& t, int& c) {
                                t = blockId;
                                c = colorId; // Assign the color to the second reference parameter
                                return true;
                            });
                        }
                    }

                    else if (cmd == "//unpaint" || cmd == "//strip") {
                        int x1, y1, z1, x2, y2, z2;
                        if (!getSelection(x1, y1, z1, x2, y2, z2)) continue;
                        int targetColor = 0;
                        doWorldEdit(x1, y1, z1, x2, y2, z2, [targetColor](int, int, int, int curT, int curC, int& t, int& c) {
                            if (curT == 0) return false;
                            if (curC == targetColor) return false;
                            t = (curT == 0) ? SV_PAINTED_BASE : curT; // Ensure valid type
                            c = targetColor;
                            return true;
                        });
                    }

                    else if (cmd == "//paint") {
                        int x1, y1, z1, x2, y2, z2;
                        if (!getSelection(x1, y1, z1, x2, y2, z2)) continue;

                        int targetColor = 0;
                        if (ss >> arg1) {
                            targetColor = getColorId(arg1);
                            if (targetColor == -1) {
                                std::string err = "[Server] Unknown color: " + arg1 + "\n";
                                send(clientSocket, err.c_str(), err.size(), 0);
                                continue;
                            }
                        } else {
                            // No color specified: fetch the exact color byte of pos1 natively
                            uint64_t k = wkey(x1, y1, z1);
                            std::lock_guard<std::mutex> lock(g_worldMtx);
                            if (g_world.count(k)) targetColor = g_world[k].color;
                        }

                        doWorldEdit(x1, y1, z1, x2, y2, z2, [targetColor](int, int, int, int curT, int curC, int& t, int& c) {
                            if (curT == 0) return false;
                            if (curC == targetColor) return false;
                            t = (curT == 0) ? SV_PAINTED_BASE : curT; // Ensure valid type
                            c = targetColor;
                            return true;
                        });
                    }

                    else if (cmd == "//walls" && ss >> arg1) {
                        int blockId = getBlockId(arg1);
                        int colorId = 0;
                        if (ss >> arg2) colorId = getColorId(arg2);

                        if (blockId == -1) continue;

                        int x1, y1, z1, x2, y2, z2;
                        if (getSelection(x1, y1, z1, x2, y2, z2)) {
                            doWorldEdit(x1, y1, z1, x2, y2, z2, [blockId, colorId, x1, z1, x2, z2](int x, int, int z, int, int, int& t, int& c) {
                                if (x == x1 || x == x2 || z == z1 || z == z2) {
                                    t = blockId;
                                    c = colorId;
                                    return true;
                                }
                                return false;
                            });
                        }
                    }

                    else if (cmd == "//replace" && ss >> arg1 >> arg2) {
                        int targetId = getBlockId(arg1);
                        int replaceId = getBlockId(arg2);

                        int newColor = 0;
                        int filterOldColor = -1; // -1 means "no filter"

                        std::string temp;
                        if (ss >> temp) {
                            newColor = getColorId(temp);
                            if (ss >> temp) {
                                filterOldColor = getColorId(temp);
                            }
                        }

                        if (targetId == -1 || replaceId == -1) continue;

                        int x1, y1, z1, x2, y2, z2;
                        if (getSelection(x1, y1, z1, x2, y2, z2)) {
                            doWorldEdit(x1, y1, z1, x2, y2, z2,
                                        [=](int x, int y, int z, int curT, int curC, int& t, int& c) {
                                            // Check block type AND optional color filter
                                            if (curT == targetId && (filterOldColor == -1 || curC == filterOldColor)) {
                                                t = replaceId;
                                                c = newColor;
                                                return true;
                                            }
                                            return false;
                                        }
                            );
                        }
                    }

                    else if (cmd == "//replacenear" && ss >> arg1 >> arg2 >> arg3) {
                        try {
                            int radius = std::stoi(arg1);
                            int targetId = getBlockId(arg2);
                            int replaceId = getBlockId(arg3);

                            int newColor = 0;
                            int filterOldColor = -1; // -1 means "no filter"

                            std::string temp;
                            if (ss >> temp) {
                                newColor = getColorId(temp);
                                if (ss >> temp) {
                                    filterOldColor = getColorId(temp);
                                }
                            }

                            if (targetId == -1 || replaceId == -1) continue;

                            int px, py, pz;
                            {
                                std::lock_guard<std::mutex> lock(clientsMutex);
                                px = playerInfoMap[clientSocket].posX;
                                py = playerInfoMap[clientSocket].posY;
                                pz = playerInfoMap[clientSocket].posZ;
                            }

                            doWorldEdit(px - radius, py - radius, pz - radius, px + radius, py + radius, pz + radius,
                                        [=](int x, int y, int z, int curT, int curC, int& t, int& c) {
                                            // True Spherical Distance Fix + Type Filter + Optional Color Filter
                                            if ((x-px)*(x-px) + (y-py)*(y-py) + (z-pz)*(z-pz) <= radius*radius) {
                                                if (curT == targetId && (filterOldColor == -1 || curC == filterOldColor)) {
                                                    t = replaceId;
                                                    c = newColor;
                                                    return true;
                                                }
                                            }
                                            return false;
                                        }
                            );
                        } catch (...) {
                            send(clientSocket, "[Server] Syntax: //replacenear <radius> <old> <new> [newColor] [oldColor]\n", 65, 0);
                        }
                    }

                    else if (cmd == "//sphere" && ss >> arg1 >> arg2) {
                        try {
                            int radius = std::stoi(arg1);
                            int blockId = getBlockId(arg2);
                            int colorId = 0;

                            // Correctly looking for the optional 3rd argument
                            if (ss >> arg3) {
                                colorId = getColorId(arg3);
                            }

                            int px, py, pz;
                            {
                                std::lock_guard<std::mutex> lock(clientsMutex);
                                px = playerInfoMap[clientSocket].posX;
                                py = playerInfoMap[clientSocket].posY;
                                pz = playerInfoMap[clientSocket].posZ;
                            }

                            doWorldEdit(px - radius, py - radius, pz - radius, px + radius, py + radius, pz + radius,
                                        [=](int x, int y, int z, int, int, int& t, int& c) {
                                            if ((x-px)*(x-px) + (y-py)*(y-py) + (z-pz)*(z-pz) <= radius*radius) {
                                                t = blockId;
                                                c = colorId;
                                                return true;
                                            }
                                            return false;
                                        }
                            );
                        } catch (...) {
                            send(clientSocket, "[Server] Syntax: //sphere <radius> <block> [color]\n", 50, 0);
                        }
                    }

                    else if (cmd == "//cyl" && ss >> arg1 >> arg2 >> arg3) {
                        try {
                            int radius = std::stoi(arg1);
                            int height = std::stoi(arg2);
                            int blockId = getBlockId(arg3);
                            int colorId = 0;

                            // Capture optional color into a local temporary string
                            std::string colorArg;
                            if (ss >> colorArg) {
                                colorId = getColorId(colorArg);
                            }

                            int px, py, pz;
                            {
                                std::lock_guard<std::mutex> lock(clientsMutex);
                                px = playerInfoMap[clientSocket].posX;
                                py = playerInfoMap[clientSocket].posY;
                                pz = playerInfoMap[clientSocket].posZ;
                            }

                            doWorldEdit(px - radius, py, pz - radius, px + radius, py + height, pz + radius,
                                        [=](int x, int y, int z, int, int, int& t, int& c) {
                                            if ((x-px)*(x-px) + (z-pz)*(z-pz) <= radius*radius) {
                                                t = blockId;
                                                c = colorId;
                                                return true;
                                            }
                                            return false;
                                        }
                            );
                        } catch (...) {
                            send(clientSocket, "[Server] Syntax: //cyl <radius> <height> <block> [color]\n", 55, 0);
                        }
                    }

                    // --- //hsphere <radius> <block> [color] ---
                    else if (cmd == "//hsphere" && ss >> arg1 >> arg2) {
                        try {
                            int radius = std::stoi(arg1);
                            int blockId = getBlockId(arg2);
                            int colorId = 0;
                            if (ss >> arg3) colorId = getColorId(arg3);

                            int px, py, pz;
                            {
                                std::lock_guard<std::mutex> lock(clientsMutex);
                                px = playerInfoMap[clientSocket].posX;
                                py = playerInfoMap[clientSocket].posY;
                                pz = playerInfoMap[clientSocket].posZ;
                            }

                            doWorldEdit(px - radius, py - radius, pz - radius, px + radius, py + radius, pz + radius,
                                        [=](int x, int y, int z, int, int, int& t, int& c) {
                                            int dx = x - px, dy = y - py, dz = z - pz;
                                            int distSq = dx*dx + dy*dy + dz*dz;
                                            // Only place block if it is within the outer shell (dist <= r)
                                            // AND outside the inner void (dist > r-1)
                                            if (distSq <= radius*radius && distSq > (radius-1)*(radius-1)) {
                                                t = blockId;
                                                c = colorId;
                                                return true;
                                            }
                                            return false;
                                        }
                            );
                        } catch (...) {
                            send(clientSocket, "[Server] Syntax: //hsphere <radius> <block> [color]\n", 53, 0);
                        }
                    }

                    // --- //hcyl <radius> <height> <block> [color] ---
                    else if (cmd == "//hcyl" && ss >> arg1 >> arg2 >> arg3) {
                        try {
                            int radius = std::stoi(arg1);
                            int height = std::stoi(arg2);
                            int blockId = getBlockId(arg3);
                            int colorId = 0;

                            std::string colorArg;
                            if (ss >> colorArg) colorId = getColorId(colorArg);

                            int px, py, pz;
                            {
                                std::lock_guard<std::mutex> lock(clientsMutex);
                                px = playerInfoMap[clientSocket].posX;
                                py = playerInfoMap[clientSocket].posY;
                                pz = playerInfoMap[clientSocket].posZ;
                            }

                            doWorldEdit(px - radius, py, pz - radius, px + radius, py + height, pz + radius,
                                        [=](int x, int y, int z, int, int, int& t, int& c) {
                                            int dx = x - px, dz = z - pz;
                                            int radSq = dx*dx + dz*dz;
                                            // Hollow logic: Within outer radius, outside inner radius
                                            if (radSq <= radius*radius && radSq > (radius-1)*(radius-1)) {
                                                t = blockId;
                                                c = colorId;
                                                return true;
                                            }
                                            return false;
                                        }
                            );
                        } catch (...) {
                            send(clientSocket, "[Server] Syntax: //hcyl <radius> <height> <block> [color]\n", 58, 0);
                        }
                    }

                    else if (cmd == "//up" && ss >> arg1) {
                        int dist = 0;
                        try { dist = std::stoi(arg1); } catch (...) {}
                        if (dist <= 0) continue;

                        float px, py, pz;
                        {
                            std::lock_guard<std::mutex> lock(clientsMutex);
                            px = (int)std::round(playerInfoMap[clientSocket].posX);
                            py = (int)std::round(playerInfoMap[clientSocket].posY);
                            pz = (int)std::round(playerInfoMap[clientSocket].posZ);
                        }

                        // Target feet position
                        int targetY = (int)std::round(py) + dist;
                        // Platform block must be 1 below the player's new feet
                        int blockX = (int)std::round(px);
                        int blockZ = (int)std::round(pz);
                        int blockY = targetY - 2;

                        // Create a single glass block
                        std::vector<BlockEdit> sessionHistory;
                        // We record what was there before
                        int curT = 0, curC = 0;
                        uint64_t k = wkey(blockX, blockY, blockZ);
                        {
                            std::lock_guard<std::mutex> lock(g_worldMtx);
                            if (g_world.count(k)) {
                                curT = g_world[k].type;
                                curC = g_world[k].color;
                            }
                        }
                        sessionHistory.push_back({blockX, blockY, blockZ, curT, curC, 58, 0});

                        applyBlockChanges(sessionHistory); // Use existing apply helper

                        // Save history
                        {
                            std::lock_guard<std::mutex> lock(clientsMutex);
                            auto& info = playerInfoMap[clientSocket];
                            info.undoHistory.push_back(sessionHistory);
                            if (info.undoHistory.size() > 20) info.undoHistory.erase(info.undoHistory.begin());
                            info.redoHistory.clear();
                        }

                        // Teleport player feet to exactly targetY
                        char sp[96];
                        int n = snprintf(sp, sizeof(sp), "SPAWN:%.2f:%.2f:%.2f\n", px, (float)targetY, pz);
                        send(clientSocket, sp, n, 0);

                        std::string msg = "[Server] Whoosh!\n";
                        send(clientSocket, msg.c_str(), msg.size(), 0);
                    }

                    else if (cmd == "//undo" || cmd == "//redo") {
                        bool isUndo = (cmd == "//undo");
                        std::vector<BlockEdit> batchToProcess;

                        {
                            std::lock_guard<std::mutex> lock(clientsMutex);
                            auto& info = playerInfoMap[clientSocket];
                            auto& sourceStack = isUndo ? info.undoHistory : info.redoHistory;

                            if (sourceStack.empty()) {
                                std::string err = "[Server] Nothing to " + cmd.substr(2) + ".\n";
                                send(clientSocket, err.c_str(), err.size(), 0);
                                continue;
                            }

                            batchToProcess = sourceStack.back();
                            sourceStack.pop_back();
                        } // <--- Lock is released here. Safe to call applyBlockChanges now.

                        std::vector<BlockEdit> flippedAction;
                        for (const auto& edit : batchToProcess) {
                            if (isUndo) {
                                flippedAction.push_back({edit.x, edit.y, edit.z, edit.newType, edit.newColor, edit.oldType, edit.oldColor});
                            } else {
                                flippedAction.push_back({edit.x, edit.y, edit.z, edit.oldType, edit.oldColor, edit.newType, edit.newColor});
                            }
                        }

                        applyBlockChanges(flippedAction);

                        // Re-lock briefly to update Redo stack
                        {
                            std::lock_guard<std::mutex> lock(clientsMutex);
                            auto& info = playerInfoMap[clientSocket];
                            auto& targetStack = isUndo ? info.redoHistory : info.undoHistory;
                            targetStack.push_back(batchToProcess);
                        }

                        std::string msg = "[Server] " + cmd.substr(2) + " batch complete.\n";
                        send(clientSocket, msg.c_str(), msg.size(), 0);
                    }

                    else if (cmd == "//copy") {
                        int x1, y1, z1, x2, y2, z2;
                        if (!getSelection(x1, y1, z1, x2, y2, z2)) continue;

                        int px, py, pz;
                        std::vector<ClipboardBlock> newClip;
                        {
                            std::lock_guard<std::mutex> lock(clientsMutex);
                            px = std::round(playerInfoMap[clientSocket].posX);
                            py = std::round(playerInfoMap[clientSocket].posY - 1.0f);
                            pz = std::round(playerInfoMap[clientSocket].posZ);
                        }

                        int minX = std::min(x1, x2), maxX = std::max(x1, x2);
                        int minY = std::min(y1, y2), maxY = std::max(y1, y2);
                        int minZ = std::min(z1, z2), maxZ = std::max(z1, z2);

                        {
                            std::lock_guard<std::mutex> lock(g_worldMtx);
                            for (int x = minX; x <= maxX; ++x) {
                                for (int y = minY; y <= maxY; ++y) {
                                    for (int z = minZ; z <= maxZ; ++z) {
                                        uint64_t k = wkey(x, y, z);
                                        if (g_world.count(k)) {
                                            // Only copying non-air blocks to make pasting cleaner
                                            newClip.push_back({x - px, y - py, z - pz, g_world[k].type, g_world[k].color});
                                        }
                                    }
                                }
                            }
                        }

                        {
                            std::lock_guard<std::mutex> lock(clientsMutex);
                            playerInfoMap[clientSocket].clipboard = newClip;
                        }

                        std::string msg = "[Server] Copied " + std::to_string(newClip.size()) + " non-air blocks.\n";
                        send(clientSocket, msg.c_str(), msg.size(), 0);
                    }

                    else if (cmd == "//paste") {
                        int px, py, pz;
                        std::vector<ClipboardBlock> clip;
                        {
                            std::lock_guard<std::mutex> lock(clientsMutex);
                            px = std::round(playerInfoMap[clientSocket].posX);
                            py = std::round(playerInfoMap[clientSocket].posY - 1.0f);
                            pz = std::round(playerInfoMap[clientSocket].posZ);
                            clip = playerInfoMap[clientSocket].clipboard;
                        }

                        if (clip.empty()) {
                            std::string err = "[Server] Clipboard is empty.\n";
                            send(clientSocket, err.c_str(), err.size(), 0);
                            continue;
                        }

                        std::vector<BlockEdit> sessionHistory;
                        {
                            std::lock_guard<std::mutex> lock(g_worldMtx);
                            for (const auto& cb : clip) {
                                int targetX = px + cb.rx;
                                int targetY = py + cb.ry;
                                int targetZ = pz + cb.rz;

                                if (targetY < 0 || targetY >= SV_WORLD_HEIGHT) continue;

                                uint64_t k = wkey(targetX, targetY, targetZ);
                                int curT = 0, curC = 0;
                                if (g_world.count(k)) {
                                    curT = g_world[k].type; curC = g_world[k].color;
                                }
                                sessionHistory.push_back({targetX, targetY, targetZ, curT, curC, cb.type, cb.color});
                            }
                        }

                        applyBlockChanges(sessionHistory);

                        {
                            std::lock_guard<std::mutex> lock(clientsMutex);
                            auto& info = playerInfoMap[clientSocket];
                            info.undoHistory.push_back(sessionHistory);
                            if (info.undoHistory.size() > 10) info.undoHistory.erase(info.undoHistory.begin());
                            info.redoHistory.clear();
                        }

                        std::string msg = "[Server] Pasted " + std::to_string(sessionHistory.size()) + " blocks.\n";
                        send(clientSocket, msg.c_str(), msg.size(), 0);
                    }

                    else if (cmd == "//rotate" && ss >> arg1) {
                        int angle = 0;
                        try { angle = std::stoi(arg1); } catch (...) {}

                        if (angle != 90 && angle != 180 && angle != 270) {
                            std::string err = "[Server] Angle must be 90, 180, or 270.\n";
                            send(clientSocket, err.c_str(), err.size(), 0);
                            continue;
                        }

                        std::lock_guard<std::mutex> lock(clientsMutex);
                        auto& clip = playerInfoMap[clientSocket].clipboard;

                        // Determine how many 90-degree rotations are needed
                        int rotations = angle / 90;

                        for (auto& cb : clip) {
                            int origX = cb.rx;
                            int origZ = cb.rz;

                            // 1. Rotate Position
                            if (rotations == 1)      { cb.rx = -origZ; cb.rz = origX; }
                            else if (rotations == 2) { cb.rx = -origX; cb.rz = -origZ; }
                            else if (rotations == 3) { cb.rx = origZ; cb.rz = -origX; }

                            // 2. Rotate Block Type (if it is a slope)
                            for (int i = 0; i < rotations; ++i) {
                                cb.type = rotateSlope(cb.type);
                            }
                        }
                        std::string msg = "[Server] Clipboard rotated " + std::to_string(angle) + " degrees.\n";
                        send(clientSocket, msg.c_str(), msg.size(), 0);
                    }

                    else if (cmd == "/resync") {
                        int px = 0, py = 0, pz = 0;

                        // Safely fetch coordinates
                        {
                            std::lock_guard<std::mutex> lock(clientsMutex);
                            if (playerInfoMap.count(clientSocket)) {
                                px = static_cast<int>(std::round(playerInfoMap[clientSocket].posX));
                                py = static_cast<int>(std::round(playerInfoMap[clientSocket].posY));
                                pz = static_cast<int>(std::round(playerInfoMap[clientSocket].posZ));
                            }
                        }

                        std::string blob;
                        int radius = 32;

                        // Build the sync packet
                        {
                            std::lock_guard<std::mutex> lock(g_worldMtx);
                            char line[128];

                            for (int x = px - radius; x <= px + radius; ++x) {
                                for (int y = py - radius; y <= py + radius; ++y) {
                                    for (int z = pz - radius; z <= pz + radius; ++z) {
                                        uint64_t k = wkey(x, y, z);
                                        if (g_world.count(k)) {
                                            auto& b = g_world[k];
                                            // Action: Set type
                                            int len = snprintf(line, sizeof(line), "ACTION:server:0:%d:%d:%d:0:%d\n", x, y, z, b.type);
                                            blob.append(line, len);
                                            // Action: Set color
                                            if (b.color != 0) {
                                                len = snprintf(line, sizeof(line), "ACTION:server:0:%d:%d:%d:3:%d\n", x, y, z, b.color);
                                                blob.append(line, len);
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        // Send the blob if it contains data
                        if (!blob.empty()) {
                            sendAll(clientSocket, blob.data(), blob.size());
                            std::string err = "[Server] Synchronized client!\n";
                            send(clientSocket, err.c_str(), err.size(), 0);
                        } else {
                            std::string err = "[Server] Failed to sync ...\n";
                            send(clientSocket, err.c_str(), err.size(), 0);
                        }




                    }

                    else if (cmd == "/id" && ss >> arg1) {
                        int bid = getBlockId(arg1);
                        int cid = getColorId(arg1);
                        std::string reply = "[Server] Lookup for '" + arg1 + "': ";
                        bool found = false;

                        // We check if it returned a valid ID (assuming -1 is the "not found" return in your code)
                        if (bid != -1) { reply += "Block ID: " + std::to_string(bid) + " "; found = true; }
                        if (cid != -1) { reply += "Color ID: " + std::to_string(cid) + " "; found = true; }

                        if (!found) reply = "[Server] No block or color found for '" + arg1 + "'";

                        reply += "\n";
                        send(clientSocket, reply.c_str(), reply.length(), 0);
                    }

                    else if (cmd == "/searchblocks" && ss >> arg1) {
                        // Access your blocks map directly
                        // Note: You may need to move 'blocks' out of getBlockId into global scope
                        // or access it via a public static if defined elsewhere.
                        // Assuming the map is available as 'blocks':
                        std::string matches = searchMap(BLOCK_MAP, arg1);
                        std::string msg = "[Server] Block matches: " + matches + "\n";
                        send(clientSocket, msg.c_str(), msg.length(), 0);
                    }

                    else if (cmd == "/searchcolors" && ss >> arg1) {
                        std::string matches = searchMap(COLOR_MAP, arg1);
                        std::string msg = "[Server] Color matches: " + matches + "\n";
                        send(clientSocket, msg.c_str(), msg.length(), 0);
                    }

                    // else if (cmd == "//debuggrid") {
                    //     int startX, startY, startZ;
                    //
                    //     // 1. Retrieve the client's position safely
                    //     {
                    //         std::lock_guard<std::mutex> lock(clientsMutex);
                    //         auto& info = playerInfoMap[clientSocket];
                    //         startX = std::round(info.posX);
                    //         startY = std::round(info.posY - 1.0f); // Feet level
                    //         startZ = std::round(info.posZ);
                    //     }
                    //
                    //     std::vector<BlockEdit> edits;
                    //
                    //     // 2. Generate 16x16 grid for IDs 0-255
                    //     for (int i = 0; i < 256; ++i) {
                    //         int x = startX + (i % 16);
                    //         int z = startZ + (i / 16);
                    //
                    //         int oldType = 0, oldColor = 0;
                    //         uint64_t k = wkey(x, startY, z);
                    //
                    //         {
                    //             // Lock world mutex to check history
                    //             std::lock_guard<std::mutex> lock(g_worldMtx);
                    //             if (g_world.count(k)) {
                    //                 oldType = g_world[k].type;
                    //                 oldColor = g_world[k].color;
                    //             }
                    //         }
                    //
                    //         edits.push_back({x, startY, z, oldType, oldColor, i, 0});
                    //     }
                    //
                    //     // 3. Apply the changes
                    //     applyBlockChanges(edits);
                    //
                    //     // 4. Confirmation feedback
                    //     std::string msg = "[Server] Debug grid generated at (" +
                    //     std::to_string(startX) + ", " + std::to_string(startY) + ", " +
                    //     std::to_string(startZ) + ").\n";
                    //     send(clientSocket, msg.c_str(), msg.size(), 0);
                    // }

                    else {
                        std::string err = "[Server] Unknown or malformed command: " + cmd + "\n";
                        send(clientSocket, err.c_str(), err.size(), 0);
                    }
                }
                // Existing Chat Logic
                else {
                    if (msgContent == "exit" || msgContent == "quit") {
                        std::cout << "[Server] " << username << " disconnected." << std::endl;
                        disconnect = true;
                        continue;
                    }
                    std::string broadcastMsg = "[" + username + "] " + msgContent + "\n";
                    std::cout << broadcastMsg;
                    broadcastMessage(broadcastMsg, clientSocket);
                }
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

                    std::string broadcastMsg;
                    std::string editSuffix;   // "x:y:z:mode[:extra]" relayed to peers
                    std::string coords = std::to_string(x) + ":" + std::to_string(y) + ":" + std::to_string(z);
                    std::string who = username + ":" + std::to_string(characterType);
                    int extra = 0;            // block type (build) / color (paint)
                    switch (mode) {
                        case 0: { // BUILD
                            extra = (parts.size() >= 6) ? std::stoi(parts[5]) : 0;
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
                            extra = (parts.size() >= 6) ? std::stoi(parts[5]) : 0;
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
        }   // end inner while (per-line processing)
    }       // end outer while (recv loop)

    std::cout << "[Server] " << username << " disconnected." << std::endl;
    std::string leaveMsg = "[Server] " + username + " has left.\n";
    broadcastMessage(leaveMsg, clientSocket);

    savePlayerPos();   // persist positions when someone leaves
    saveWorld();       // and persist the world model
    removeClient(clientSocket);
    close(clientSocket);
}

int main(int argc, char* argv[]) {
    std::cout << std::unitbuf;   // auto-flush so logs appear live under systemd/journald
    int port = DEFAULT_PORT;

    // Args: [port] and/or flags:
    //   --port N  --name "My World"  --password PASS  --world FILE
    //   --matchmaker HOST[:PORT]
    // A bare leading number is still accepted as the port (back-compat).
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* def)->std::string{ return (i+1<argc) ? std::string(argv[++i]) : std::string(def); };
        if      (a == "--port")       port = std::atoi(next("27015").c_str());
        else if (a == "--name")       g_serverName = next("Eden Server");
        else if (a == "--password")   g_password   = next("");
        else if (a == "--world")      g_worldFile  = next("eden_world.model");
        else if (a == "--verbose")    g_verbose    = true;
        else if (a == "--idle-timeout") g_idleTimeout = std::atoi(next("0").c_str());
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
