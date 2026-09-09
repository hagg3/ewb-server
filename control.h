// control.h — the pure, testable half of ROADMAP-SERVER stage 3.2: the Tier 1
// operator control surface.
//
// Tier 1 is **out of band**. It never touches the client wire, so it invents no
// protocol a client has to understand; the transport is a `0600` unix domain
// socket at `<worlddir>/edenserver.sock` and filesystem permissions *are* the
// authentication (plan §3.1 — `JOIN` carries a shared world password, not a
// per-user credential, so there is no identity to build admin auth on).
//
// Everything here is a pure function or a small self-contained struct that reads
// and writes plain text. `server_posix.cpp` owns the socket, the locks, the world
// model and the broadcast path; this header owns:
//   * the command table + `help` text (one definition, so `edenctl` and the
//     server can never disagree about what exists),
//   * the persisted ban list (`eden_bans.txt`) and op-level file (`eden_ops.txt`),
//     both plain line-based sidecars in the same style as `eden_world.model`,
//   * `fill` volume arithmetic and the cap it shares with the player tier,
//   * the per-connection flood guard (stage 3.4).
//
// The line grammar is `verb[:rest]` — the verb up to the first ':', then a single
// remainder the handler splits as it needs (so a `say` message or a sign's text
// keeps its own ':' and spaces). `edenctl` joins its arguments with ':' to build
// that line; a human with `nc -U` types it directly.

#pragma once

#include <cstdint>
#include <cstdlib>
#include <istream>
#include <ostream>
#include <set>
#include <string>
#include <vector>

#include "hardening.h"   // sanitize_text

namespace ewb {

// --- command table ----------------------------------------------------------

/// One control verb. `min_args` / `max_args` count the ':'-separated fields of
/// `rest` (not the verb); `max_args < 0` means "unbounded" (`say`, whose rest is
/// one free-text field, uses min 1 / max 1 because it is never split).
struct CtlSpec {
    const char* name;
    int         min_args;
    int         max_args;
    const char* usage;
};

inline const std::vector<CtlSpec>& ctl_specs() {
    static const std::vector<CtlSpec> specs = {
        {"help",         0,  0, "help                             — this text"},
        {"who",          0,  0, "who                              — connected players"},
        {"say",          1,  1, "say:<text>                       — broadcast a [Server] line"},
        {"kick",         1,  2, "kick:<name>[:<reason>]           — disconnect a player"},
        {"ban",          1,  1, "ban:<name|ip>                    — ban and disconnect"},
        {"unban",        1,  1, "unban:<name|ip>                  — lift a ban"},
        {"banlist",      0,  0, "banlist                          — show the ban list"},
        {"save",         0,  0, "save                             — flush world + players to disk"},
        {"stop",         0,  0, "stop                             — save and shut the server down"},
        {"op",           2,  2, "op:<name>:<0..2>                 — set a player's op level"},
        {"deop",         1,  1, "deop:<name>                      — clear a player's op level"},
        {"setblock",     4,  5, "setblock:<x>:<y>:<z>:<type>[:<color>] — place one block"},
        {"fill",         7,  8, "fill:<x0>:<y0>:<z0>:<x1>:<y1>:<z1>:<type>[:<color>] — fill a box"},
        {"signs",        1, -1, "signs:reload | signs:add:<x>:<y>:<z>:<a>:<b>:<c>:<text> | signs:rm:<x>:<y>:<z>"},
        {"region-stats", 0,  0, "region-stats                     — REGION counters since start"},
    };
    return specs;
}

inline const CtlSpec* ctl_find(const std::string& verb) {
    for (const CtlSpec& s : ctl_specs())
        if (verb == s.name) return &s;
    return nullptr;
}

/// Full `help` body, one line per verb, terminating '\n' included.
inline std::string ctl_help_text() {
    std::string out = "edenserver control socket — Tier 1 operator commands:\n";
    for (const CtlSpec& s : ctl_specs()) {
        out += "  ";
        out += s.usage;
        out += '\n';
    }
    return out;
}

/// Split a control line into `verb` and `rest` (everything after the first ':').
/// A trailing '\r'/'\n' is stripped. Returns false for an empty line.
inline bool ctl_split(const std::string& line, std::string& verb, std::string& rest) {
    size_t end = line.size();
    while (end > 0 && (line[end - 1] == '\r' || line[end - 1] == '\n')) --end;
    if (end == 0) return false;
    const size_t colon = line.find(':');
    if (colon == std::string::npos || colon >= end) {
        verb = line.substr(0, end);
        rest.clear();
    } else {
        verb = line.substr(0, colon);
        rest = line.substr(colon + 1, end - colon - 1);
    }
    return !verb.empty();
}

/// Split `rest` on ':' into at most `limit` fields (the last field keeps any
/// remaining ':'). `limit <= 0` means "split everything".
inline std::vector<std::string> ctl_fields(const std::string& rest, int limit = 0) {
    std::vector<std::string> out;
    if (rest.empty()) return out;
    size_t pos = 0;
    while (true) {
        if (limit > 0 && (int)out.size() == limit - 1) { out.push_back(rest.substr(pos)); break; }
        const size_t colon = rest.find(':', pos);
        if (colon == std::string::npos) { out.push_back(rest.substr(pos)); break; }
        out.push_back(rest.substr(pos, colon - pos));
        pos = colon + 1;
    }
    return out;
}

// --- ban list (eden_bans.txt) ----------------------------------------------

/// A token is treated as an IP if it is only digits, dots and colons (covers
/// IPv4 and the textual IPv6 forms). Everything else is a username.
inline bool ctl_looks_like_ip(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || c == '.' || c == ':')) return false;
    return true;
}

/// Persisted ban list. One token per line; `#` comments and blanks are ignored.
/// Names and IPs share the file — `ctl_looks_like_ip` decides which set a token
/// joins on load and on `add`.
struct BanList {
    std::set<std::string> names;   // exact-match usernames
    std::set<std::string> ips;

    void clear() { names.clear(); ips.clear(); }

    void load(std::istream& in) {
        clear();
        std::string line;
        while (std::getline(in, line)) {
            size_t b = 0, e = line.size();
            while (e > b && (line[e - 1] == '\r' || line[e - 1] == ' ' || line[e - 1] == '\t')) --e;
            while (b < e && (line[b] == ' ' || line[b] == '\t')) ++b;
            if (b >= e || line[b] == '#') continue;
            add(line.substr(b, e - b));
        }
    }

    void serialize(std::ostream& out) const {
        out << "# edenserver ban list — one name or IP per line (stage 3.2).\n";
        for (const std::string& s : ips)   out << s << "\n";
        for (const std::string& s : names) out << s << "\n";
    }

    /// Returns true if the token was newly added.
    bool add(const std::string& token) {
        if (token.empty()) return false;
        auto& set = ctl_looks_like_ip(token) ? ips : names;
        return set.insert(token).second;
    }

    /// Returns true if the token was present and removed.
    bool remove(const std::string& token) {
        return ips.erase(token) + names.erase(token) > 0;
    }

    bool ip_banned(const std::string& ip) const { return ips.count(ip) > 0; }
    bool name_banned(const std::string& name) const { return names.count(name) > 0; }
    bool banned(const std::string& name, const std::string& ip) const {
        return name_banned(name) || ip_banned(ip);
    }

    bool empty() const { return names.empty() && ips.empty(); }
};

// --- op levels (eden_ops.txt) ---------------------------------------------

/// `0` visitor · `1` builder · `2` operator. Stored, but nothing *consumes* the
/// level until Tier 2 (stage 3.3); `op`/`deop`/`who` maintain it now so the file
/// exists and is correct when 3.3 lands.
constexpr int CTL_LEVEL_MIN = 0;
constexpr int CTL_LEVEL_MAX = 2;

inline bool ctl_level_valid(int lvl) { return lvl >= CTL_LEVEL_MIN && lvl <= CTL_LEVEL_MAX; }

struct OpsFile {
    // name -> level. Only non-default levels are stored; a missing name is the
    // server's --default-level (0 unless an operator raised it).
    std::vector<std::pair<std::string, int>> entries;

    void clear() { entries.clear(); }

    int level_of(const std::string& name, int def) const {
        for (const auto& e : entries)
            if (e.first == name) return e.second;
        return def;
    }

    /// Set (or replace) a level. Returns false for an invalid level.
    bool set(const std::string& name, int lvl) {
        if (name.empty() || !ctl_level_valid(lvl)) return false;
        for (auto& e : entries)
            if (e.first == name) { e.second = lvl; return true; }
        entries.emplace_back(name, lvl);
        return true;
    }

    bool erase(const std::string& name) {
        for (auto it = entries.begin(); it != entries.end(); ++it)
            if (it->first == name) { entries.erase(it); return true; }
        return false;
    }

    void load(std::istream& in) {
        clear();
        std::string line;
        while (std::getline(in, line)) {
            size_t e = line.size();
            while (e > 0 && (line[e - 1] == '\r' || line[e - 1] == ' ')) --e;
            if (e == 0 || line[0] == '#') continue;
            const size_t colon = line.rfind(':', e - 1);
            if (colon == std::string::npos) continue;
            const std::string name = line.substr(0, colon);
            const int lvl = std::atoi(line.substr(colon + 1, e - colon - 1).c_str());
            if (!name.empty() && ctl_level_valid(lvl)) set(name, lvl);
        }
    }

    void serialize(std::ostream& out) const {
        out << "# edenserver op levels — <name>:<0..2> per line (stage 3.2).\n";
        for (const auto& e : entries) out << e.first << ":" << e.second << "\n";
    }
};

// --- fill volume ----------------------------------------------------------

/// How many times the Tier 2 per-command cap (`--we-max-cells`) a single Tier 1
/// `fill` may be.
///
/// Stage 3.2 shipped a flat `CTL_MAX_FILL_CELLS = 262144` and 3.3 then shipped
/// `WE_MAX_EDIT_CELLS = 131072`: two differently-derived numbers for one idea —
/// "the most cells one command may touch in a single pass over the world". The
/// cost being bounded is identical in both tiers, so there is one number, and
/// the tiers differ only by this multiple. It is 2 because the operator is
/// authenticated by filesystem permissions and their own flood is their own
/// fault, so they get headroom the untrusted tier does not; the default pair is
/// unchanged (131072 → 262144).
///
/// Moving `--we-max-cells` moves both.
constexpr long long CTL_FILL_CAP_MULTIPLE = 2;

/// The Tier 1 `fill` cap derived from the Tier 2 one, clamped to the world's
/// edited-cell ceiling (no single command may be permitted to fill the world)
/// and to at least one cell (so a hostile or silly `--we-max-cells` cannot make
/// `setblock` unusable). Pure so `control_test.cpp` can pin the derivation.
inline long long ctl_fill_cap(long long we_max_cells, long long world_ceiling) {
    if (we_max_cells < 1) we_max_cells = 1;
    long long cap = we_max_cells * CTL_FILL_CAP_MULTIPLE;
    if (world_ceiling > 0 && cap > world_ceiling) cap = world_ceiling;
    return cap < 1 ? 1 : cap;
}

/// Inclusive cell count of the box, computed on `long long` so a hostile pair of
/// coordinates can't overflow into a small positive number.
inline long long ctl_fill_volume(long long x0, long long y0, long long z0,
                                 long long x1, long long y1, long long z1) {
    const long long dx = (x0 < x1 ? x1 - x0 : x0 - x1) + 1;
    const long long dy = (y0 < y1 ? y1 - y0 : y0 - y1) + 1;
    const long long dz = (z0 < z1 ? z1 - z0 : z0 - z1) + 1;
    return dx * dy * dz;
}


// --- control-socket flood guard (stage 3.4) --------------------------------
//
// Tier 1 is authenticated by filesystem permissions, which makes it *more*
// privileged than the player tier, not less — and until 3.4 it had exactly one
// bound, `SV_MAX_LINE`. A local script in a retry loop (or a shell `while true;
// do edenctl fill ...; done`) could therefore issue commands as fast as the
// kernel would carry them, each one taking the world lock. Filesystem
// permissions decide *who* may connect; they say nothing about *how fast*.
//
// So: a per-connection command budget, a strike count so a caller that ignores
// the throttle is eventually disconnected rather than throttled forever, and a
// cap on concurrent control connections (the accept loop spawns a thread per
// connection). None of it is a security boundary — it is the guard rail that
// keeps an operator's own runaway script from being indistinguishable from an
// attack.

/// Commands one control connection may issue at once, and the sustained rate it
/// refills at. Deliberately far above interactive use (`edenctl` opens a
/// connection, sends one line and exits) and far below what a busy loop reaches.
constexpr double CTL_CMD_BURST = 64.0;
constexpr double CTL_CMD_RATE  = 16.0;

/// Refusals tolerated on one connection before it is closed. A caller that
/// backs off never reaches this; a busy loop reaches it in well under a second.
constexpr int CTL_MAX_STRIKES = 8;

/// Concurrent control connections. Each one costs a thread, and nothing
/// legitimate needs more than a couple.
constexpr int CTL_MAX_CONNS = 8;

/// Seconds a control connection may sit silent before it is closed, so an
/// abandoned `nc -U` cannot hold one of the slots above indefinitely.
constexpr int CTL_IDLE_TIMEOUT_SEC = 300;

/// Per-connection command pacing. Pure: `server_posix.cpp` supplies the clock
/// and owns the socket, so `control_test.cpp` can drive the whole state machine
/// with a fake one.
struct CtlFlood {
    TokenBucket bucket;
    int strikes = 0;
    int max_strikes = CTL_MAX_STRIKES;

    enum Verdict {
        Allow,       ///< run the command
        Throttle,    ///< refuse this one, keep the connection
        Disconnect   ///< too many refusals; answer and hang up
    };

    CtlFlood(double burst = CTL_CMD_BURST, double rate = CTL_CMD_RATE)
        : bucket(burst, rate) {}

    /// `rate <= 0` disables the guard entirely (`--control-rate 0`), which is
    /// why the check is here and not at the call site: one place to turn off.
    Verdict check(double now) {
        if (bucket.refill_per_sec <= 0.0) return Allow;
        if (bucket.allow(now)) {
            strikes = 0;          // a well-behaved caller never accumulates
            return Allow;
        }
        return ++strikes >= max_strikes ? Disconnect : Throttle;
    }
};

}  // namespace ewb
