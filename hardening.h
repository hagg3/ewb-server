// hardening.h — the pure, testable half of ROADMAP-SERVER stage 1.7: username
// validation, per-connection token buckets, a per-IP connect limiter, `ACTION`
// payload validation and text sanitisation — plus (stage 7.16) movement-field
// validation and (stage 7.17) the pre-`JOIN` verb gate.
//
// Everything here is a pure function or a small self-contained struct with an
// explicit `now` parameter (seconds, monotonic — the caller supplies the clock),
// so `protocol_test.cpp` can exercise a rate limiter without sleeping and a
// username rule without a socket. `server_posix.cpp` supplies the clock, the
// locks and the policy constants.
//
// Why each rule exists (plan §1.7):
//   * usernames  — chat is broadcast as `[<user> (T<n>)] <text>`, so a `]` in a
//                  name forges message structure; `server` is the reserved sender
//                  token used by `SIGNP` and the legacy snapshot.
//   * ACTION     — nothing validated `extra` before this, so any byte could land
//                  in `Cell::type`/`Cell::color`, be persisted, and be broadcast
//                  verbatim to every peer. `region_query.h`'s `emit_cell_records`
//                  guards the *wire*; this guards ingest.
//   * buckets    — a scripted client can spam edits, each growing the world toward
//                  the cell cap *and* fanning out to every peer.
//   * connects   — `SV_MAX_CLIENTS` bounds concurrency but not churn: a connect
//                  flood still spawns a thread per attempt.
//   * movement   — (stage 7.16) `std::stof` was the only filter on `POS`/`VEL`/
//                  `POSVEL`, so `nan`, `inf` and `1e38` reached the server's
//                  state, the relay, and `SPAWN`'s fixed-size formatter — where
//                  `1e38` over-read the buffer and put stack bytes on the wire.
//   * pre-JOIN   — (stage 7.17) five verbs skipped the handshake check the other
//                  four had, so on a `--password` server the world and the chat
//                  channel were writable by a peer that never sent the password.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <cerrno>
#include <iterator>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

namespace ewb {

// --- text --------------------------------------------------------------------

/// Strip anything that could break the `\n`-delimited, `:`-split framing or a
/// terminal, and bound the length. ASCII control characters (and DEL) go; bytes
/// >= 0x80 stay, so a UTF-8 chat line or sign text survives intact.
///
/// Truncation is by *byte*, which can split a multi-byte UTF-8 sequence — that is
/// accepted: `max_len` is a flood guard, not a display rule, and a torn trailing
/// codepoint renders as one replacement character.
inline std::string sanitize_text(const std::string& in, size_t max_len) {
    std::string out;
    out.reserve(in.size() < max_len ? in.size() : max_len);
    for (unsigned char c : in) {
        if (out.size() >= max_len) break;
        if (c < 0x20 || c == 0x7F) continue;   // control chars, incl. \r \n \t
        out.push_back(static_cast<char>(c));
    }
    return out;
}

// --- usernames ---------------------------------------------------------------

constexpr size_t USERNAME_MAX = 20;

/// The one reserved name: `SIGNP:server:...` and `ACTION:server:0:...` both use
/// `server` as the sender token, so a player called `server` can impersonate the
/// server on any client that trusts that token.
constexpr const char* USERNAME_RESERVED = "server";

enum class NameVerdict { Ok, Empty, TooLong, BadChar, Reserved };

inline const char* name_verdict_text(NameVerdict v) {
    switch (v) {
        case NameVerdict::Ok:       return "ok";
        case NameVerdict::Empty:    return "empty name";
        case NameVerdict::TooLong:  return "name too long";
        case NameVerdict::BadChar:  return "illegal character in name";
        case NameVerdict::Reserved: return "reserved name";
    }
    return "invalid name";
}

/// Printable ASCII only, bounded, no framing or chat-structure characters, no
/// leading/trailing space, and not the reserved sender token.
///
/// ⚠️ Deliberately excludes bytes >= 0x80. A bounded charset is the whole point of
/// the rule; a UTF-8 display name is a feature request, not a Phase 1 regression
/// (every username observed in any capture is plain ASCII: `Player6835`).
inline NameVerdict validate_username(const std::string& name) {
    if (name.empty()) return NameVerdict::Empty;
    if (name.size() > USERNAME_MAX) return NameVerdict::TooLong;
    if (name.front() == ' ' || name.back() == ' ') return NameVerdict::BadChar;
    for (unsigned char c : name) {
        if (c < 0x20 || c > 0x7E) return NameVerdict::BadChar;   // non-printable / non-ASCII
        if (c == ':') return NameVerdict::BadChar;               // protocol delimiter
        if (c == '[' || c == ']') return NameVerdict::BadChar;   // chat structure
    }
    // Case-insensitive: `Server` impersonates just as well as `server`.
    std::string lower;
    lower.reserve(name.size());
    for (unsigned char c : name) lower.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));
    if (lower == USERNAME_RESERVED) return NameVerdict::Reserved;
    return NameVerdict::Ok;
}

// --- duplicate names (stage 7.5) ---------------------------------------------

/// The lowest free `<wanted>-2`, `<wanted>-3`, … given the names in use.
///
/// `wanted` is returned untouched when it is free. Otherwise the base is cut so the
/// suffix always fits USERNAME_MAX (a 20-character name still gets a legal `-2`),
/// and a trailing space left by the cut is dropped, because `validate_username`
/// refuses one. Comparison is exact, matching the `g_playerPos` key the name will
/// be saved under. `wanted` must already have passed `validate_username`; the result
/// then does too.
template <class NameSet>
inline std::string next_free_username(const std::string& wanted, const NameSet& taken) {
    if (taken.find(wanted) == taken.end()) return wanted;
    for (unsigned n = 2;; ++n) {
        const std::string suffix = "-" + std::to_string(n);
        std::string base = wanted.substr(0, USERNAME_MAX - suffix.size());
        while (!base.empty() && base.back() == ' ') base.pop_back();
        std::string candidate = base + suffix;
        if (taken.find(candidate) == taken.end()) return candidate;
    }
}

enum class DupNameAction {
    Evict,    // same address, old socket gone quiet: it is this player's own dead session
    Suffix,   // anyone else: keep the other player, take the next free `name-N`
};

/// What a `JOIN` for a name that is already connected should do.
///
/// A rejoin over a dropped mobile link is the same player from the same address
/// while the server's copy of the old socket is still waiting on TCP to give up. That
/// is worth evicting, so they keep their name *and* their saved position.
///
/// ⚠️ Address alone is not enough. Carrier-grade NAT and shared routers put strangers
/// behind one IP, and "same IP evicts" would let any of them boot a named player by
/// joining under their name. The old socket must also have been silent for at least
/// `staleSecs`. A live retail client cannot satisfy that: it sends `PING` every 10 s
/// and `POS` whenever it moves. `staleSecs <= 0` turns eviction off entirely.
inline DupNameAction dup_name_action(bool sameAddress, double oldSilentSecs, double staleSecs) {
    if (sameAddress && staleSecs > 0.0 && oldSilentSecs >= staleSecs) return DupNameAction::Evict;
    return DupNameAction::Suffix;
}

// --- pre-JOIN admission gate (stage 7.17) ------------------------------------

/// May this verb be acted on by a connection that has not completed its `JOIN`?
///
/// `JOIN` is the *only* authentication a `--password` server has, so every verb
/// that touches the world, the roster, the player store or another player's
/// screen has to sit behind it. Until stage 7.17 only `REGION`, `SIGNQ`, `SIGNP`
/// and the `/`-command path checked; `ACTION`, `MSG`, `POS`, `VEL` and `POSVEL`
/// did not — so an unauthenticated peer could edit and persist the world,
/// broadcast chat as `Player<n>`, and inject a phantom player that never gets a
/// `has left` line.
///
/// Written as one allow-list rather than five per-verb checks so that a verb
/// added later is gated *by default* — the same structural argument
/// `worldedit.h`'s permission table makes. An unknown verb is refused too,
/// which also keeps `--verbose`'s unrecognised-verb set out of a stranger's
/// reach.
///
/// `PING` is allowed through. Its reply is a bare `PONG` that reveals nothing a
/// successful TCP connect has not already revealed, and a real client's 10 s
/// ping timer can fire while its own `JOIN` is still in flight.
inline bool verb_allowed_before_join(const std::string& command) {
    return command == "JOIN" || command == "PING";
}

// --- ACTION payload ----------------------------------------------------------

/// Highest block id the server will accept in an `ACTION:...:0:<type>`.
///
/// 0–111 are the community-named blocks (`BLOCK_MAP`, plan §0.5.1); 112–127 are the
/// ids the 2026-08 game update added (VuencEdit `CLAUDE.md` "New block types
/// 112–127"). 128+ has never been seen from any client, and 255 in particular is
/// the server model's own painted-base sentinel — letting one in would make a
/// player-placed block indistinguishable from a painted natural cell.
constexpr int MAX_BLOCK_TYPE = 127;

/// Highest paint index. The palette is 55 entries: 0 = the no-paint sentinel
/// (`getColorId` maps `none`/`unpaint`/`base` to it), 1–54 the real colours.
/// Must agree with `region_query.h`'s `CELL_MAX_PAINT`; `server_posix.cpp`
/// static_asserts the pair.
constexpr int MAX_PAINT_INDEX = 54;

/// Validate the trailing `extra` of an `ACTION:x:y:z:<mode>[:<extra>]`.
/// Modes 1 (mine) and 2 (burn) carry no payload, so anything trailing is ignored.
inline bool action_extra_valid(int mode, int extra) {
    if (mode == 0) return extra >= 0 && extra <= MAX_BLOCK_TYPE;
    if (mode == 3) return extra >= 0 && extra <= MAX_PAINT_INDEX;
    return true;
}

// --- movement floats (stage 7.16) --------------------------------------------

/// Horizontal bound: the 24-bit key range `ACTION` already enforces. x/z are
/// centred on 65536, so a playable position is nowhere near either end.
constexpr float MOVE_XZ_MAX = 16777215.0f;   // 0xFFFFFF

/// Vertical bound: the world is 0..255, but a *player's* y is the origin of the
/// avatar, which sits ~0.92 above the block it stands on (ground standing height
/// 33.92 over block 33), and a jump adds a couple more. 272 is the world height
/// plus enough headroom that standing on and jumping from the topmost block is
/// still a legal position; anything past it is not a game state.
constexpr float MOVE_Y_MAX = 272.0f;

/// Velocity bound. Not a physics claim — no capture pins the real terminal
/// speed — just a ceiling far above anything playable, so a relayed velocity
/// stays a bounded number instead of `1e38`.
constexpr float MOVE_VEL_MAX = 4096.0f;

/// Parse one movement field strictly: the whole token must be a finite float.
///
/// `std::stof` is not enough. It skips leading whitespace, stops at the first
/// byte it cannot use and returns without throwing, so `"1\r"`, `"1x"` and
/// `"  1"` all "parse" — and it accepts `nan`, `inf` and `1e38`. Every one of
/// those then reaches the server's state, the relay, and `SPAWN`'s formatter.
inline bool parse_move_float(const std::string& in, float& out) {
    if (in.empty()) return false;
    switch (in[0]) {   // strtof would skip these; the wire grammar has no padding
        case ' ': case '\t': case '\v': case '\f': case '\r': case '\n': return false;
        default: break;
    }
    char* end = nullptr;
    const float v = std::strtof(in.c_str(), &end);
    // Rejects trailing junk, and an embedded NUL (strtof stops short of size()).
    if (end != in.c_str() + in.size()) return false;
    // nan/inf as written, and the HUGE_VALF an out-of-float-range literal returns.
    if (!std::isfinite(v)) return false;
    out = v;
    return true;
}

/// A position the server will store, relay and later hand back as a `SPAWN`.
/// NaN fails every comparison, so it is refused here too even if it somehow
/// reached this without going through `parse_move_float`.
inline bool move_pos_valid(float x, float y, float z) {
    return x >= 0.0f && x <= MOVE_XZ_MAX &&
           z >= 0.0f && z <= MOVE_XZ_MAX &&
           y >= 0.0f && y <= MOVE_Y_MAX;
}

inline bool move_vel_valid(float vx, float vy, float vz) {
    return vx >= -MOVE_VEL_MAX && vx <= MOVE_VEL_MAX &&
           vy >= -MOVE_VEL_MAX && vy <= MOVE_VEL_MAX &&
           vz >= -MOVE_VEL_MAX && vz <= MOVE_VEL_MAX;
}

/// `POS:x:y:z` (and the position half of `POSVEL`): parse all three fields and
/// bound them together, so a caller cannot forget half of the rule.
inline bool parse_move_pos(const std::string& sx, const std::string& sy, const std::string& sz,
                           float& x, float& y, float& z) {
    return parse_move_float(sx, x) && parse_move_float(sy, y) && parse_move_float(sz, z) &&
           move_pos_valid(x, y, z);
}

/// `VEL:x:y:z` (and the velocity half of `POSVEL`).
inline bool parse_move_vel(const std::string& sx, const std::string& sy, const std::string& sz,
                           float& x, float& y, float& z) {
    return parse_move_float(sx, x) && parse_move_float(sy, y) && parse_move_float(sz, z) &&
           move_vel_valid(x, y, z);
}

/// One movement number as it goes back out: two decimals, the precision `SPAWN` and
/// `eden_players.txt` already use (stage 7.18).
///
/// The relay used to echo the client's own token. `parse_move_float` bounds its
/// *value*, not its *length*, so `"0"` x 1300 + `"1"` is in range, parses, and was
/// relayed at ~8 KB a line to every peer. Formatting from the parsed float makes
/// the size a property of the number, not of what the sender chose to type.
/// A magnitude under 0.005 would print as `-0.00`; that is `0.00`.
inline std::string format_move_float(float v) {
    char buf[32];   // |v| <= 16777215 (MOVE_XZ_MAX) or 4096: 11 chars at most, with room
    std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(v));
    return std::string(buf) == "-0.00" ? std::string("0.00") : std::string(buf);
}

/// `x:y:z`, each through format_move_float.
inline std::string format_move_triplet(float x, float y, float z) {
    return format_move_float(x) + ":" + format_move_float(y) + ":" + format_move_float(z);
}

// --- world cell cap ----------------------------------------------------------

/// The least room for *new* cells a hosted world should have above what it loads.
///
/// ⚠️ A world loaded at its `--max-world-cells` refuses every brand-new cell while
/// still accepting edits to cells it already holds. From a player's seat that is
/// silent, partial data loss: a block placed in open air, or a natural block mined
/// or painted, is a new cell and vanishes on the next join; a block placed where
/// the import already stored a cell saves. Setting the cap to exactly an import's
/// cell count — which the tooling used to suggest — produces precisely that.
constexpr size_t WORLD_CELL_HEADROOM_MIN = 1000000;

/// A `--max-world-cells` that leaves a world of `cells` room to grow: a quarter
/// again, never less than WORLD_CELL_HEADROOM_MIN, rounded up to a whole 100,000
/// so it reads cleanly in a config file. RAM is still the operator's real limit.
inline size_t recommended_max_world_cells(size_t cells) {
    const size_t want = cells + std::max(WORLD_CELL_HEADROOM_MIN, cells / 4);
    return (want + 99999) / 100000 * 100000;
}

enum class CellCapState { Ok, Low, Full };

/// Where a loaded world sits against its cap: `Full` refuses every new cell,
/// `Low` has less than a tenth of the cap left for players to build into.
inline CellCapState cell_cap_state(size_t cells, size_t cap) {
    if (cells >= cap) return CellCapState::Full;
    if (cap - cells < cap / 10) return CellCapState::Low;
    return CellCapState::Ok;
}

// --- token bucket ------------------------------------------------------------

/// Classic leaky bucket over a caller-supplied monotonic clock (seconds).
/// `capacity` is the burst a client may spend at once; `refill_per_sec` the
/// sustained rate. One instance per connection, touched by one thread only.
struct TokenBucket {
    double capacity;
    double refill_per_sec;
    double tokens;
    double last = 0.0;
    bool primed = false;

    TokenBucket(double cap, double rate) : capacity(cap), refill_per_sec(rate), tokens(cap) {}

    /// Spend `cost` if available. Returns false (and spends nothing) when the
    /// bucket is dry, so a refused request costs the client its whole allowance
    /// rather than draining it partially.
    bool allow(double now, double cost = 1.0) {
        refill(now);
        if (tokens < cost) return false;
        tokens -= cost;
        return true;
    }

    /// Spend `cost` after the fact, even past empty — for work whose size is only
    /// known once it has run (a BURN chain, stage 7.19). The debt is repaid by
    /// refill before the next allow() succeeds, and bounded at one burst's worth
    /// so a single charge can never lock a client out for longer than two bursts'
    /// refill time.
    void charge(double now, double cost) {
        refill(now);
        tokens -= cost;
        if (tokens < -capacity) tokens = -capacity;
    }

  private:
    void refill(double now) {
        if (!primed) {
            last = now;
            primed = true;
        } else if (now > last) {
            tokens += (now - last) * refill_per_sec;
            if (tokens > capacity) tokens = capacity;
            last = now;
        }
    }
};

// --- per-IP connect limiter --------------------------------------------------

/// Sliding-window connect counter, keyed by peer IP string. Lives in the accept
/// loop (single-threaded), so it carries no lock of its own.
///
/// ⚠️ **Fails open when its own tracking table is full.** A flood from thousands of
/// distinct source addresses would otherwise turn this guard into the memory
/// exhaustion it exists to prevent; `SV_MAX_CLIENTS` still bounds concurrency in
/// that case. `max_tracked` is sized so the normal case never reaches it.
class ConnectLimiter {
  public:
    ConnectLimiter(size_t max_per_window, double window_sec, size_t max_tracked = 4096)
        : max_per_window_(max_per_window), window_(window_sec), max_tracked_(max_tracked) {}

    bool allow(const std::string& ip, double now) {
        auto it = hits_.find(ip);
        if (it == hits_.end()) {
            if (hits_.size() >= max_tracked_) {
                sweep(now);
                if (hits_.size() >= max_tracked_) return true;   // fail open — see above
            }
            it = hits_.emplace(ip, std::vector<double>{}).first;
        }
        prune(it->second, now);
        if (it->second.size() >= max_per_window_) return false;
        it->second.push_back(now);
        return true;
    }

    size_t tracked() const { return hits_.size(); }

    /// Drop every IP with no hits left inside the window.
    void sweep(double now) {
        for (auto it = hits_.begin(); it != hits_.end();) {
            prune(it->second, now);
            it = it->second.empty() ? hits_.erase(it) : std::next(it);
        }
    }

  private:
    void prune(std::vector<double>& v, double now) {
        const double cutoff = now - window_;
        size_t keep = 0;
        while (keep < v.size() && v[keep] <= cutoff) ++keep;
        if (keep) v.erase(v.begin(), v.begin() + static_cast<long>(keep));
    }

    size_t max_per_window_;
    double window_;
    size_t max_tracked_;
    std::unordered_map<std::string, std::vector<double>> hits_;
};

// --- constant-time password compare ----------------------------------------

/// Length-independent byte compare for the `JOIN` password check (stage 1.10).
/// Not a crypto primitive — network jitter dwarfs any timing signal a LAN peer
/// could measure — but it removes `a != b`'s data-dependent early-out for free
/// and is trivial to get right. Runs over the longer of the two lengths, so a
/// length mismatch costs the same as a content mismatch and folds the size
/// difference into the result.
inline bool const_time_eq(const std::string& a, const std::string& b) {
    const size_t n = a.size() > b.size() ? a.size() : b.size();
    unsigned char diff = static_cast<unsigned char>((a.size() ^ b.size()) != 0);
    for (size_t i = 0; i < n; ++i) {
        const unsigned char ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        const unsigned char cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        diff |= static_cast<unsigned char>(ca ^ cb);
    }
    return diff == 0;
}

// --- where the world password comes from (stage 7.28) ----------------------
//
// `--password` puts the secret in argv, which /proc/<pid>/cmdline shows to every
// local user (and to `ps`, a monitoring agent, a crash reporter). So the server
// also takes it from a file (`--password-file`, the mode-0600 file an operator
// already keeps) or from the `EDEN_PASSWORD` environment variable (which the
// systemd units already load from their EnvironmentFile, and which
// /proc/<pid>/environ shows to the owner and root only). `--password` stays for
// interactive use.

enum class PasswordSource { None, Argv, File, Env };

struct PasswordChoice {
    PasswordSource source = PasswordSource::None;
    std::string value;
};

/// First line of a password file, without its line ending. A password may contain
/// spaces (and anything else but a newline); only the trailing CR/LF is dropped,
/// so `echo secret > f` and `printf secret > f` mean the same thing.
inline std::string password_from_file_text(const std::string& raw) {
    const size_t nl = raw.find('\n');
    std::string line = nl == std::string::npos ? raw : raw.substr(0, nl);
    while (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

/// Precedence: an explicit non-empty `--password`, then `--password-file`, then
/// `EDEN_PASSWORD`. `--password ""` (which the shipped units used to pass for an
/// open server) counts as "not given", so an old unit file keeps working against
/// a new binary. `file_pw` is null when no file was requested; a requested file
/// that is missing or empty is the caller's error, never an open server.
inline PasswordChoice choose_password(const std::string& argv_pw, const std::string* file_pw,
                                      const char* env_pw) {
    if (!argv_pw.empty()) return {PasswordSource::Argv, argv_pw};
    if (file_pw)          return {PasswordSource::File, *file_pw};
    if (env_pw && *env_pw) return {PasswordSource::Env, env_pw};
    return {};
}

// --- per-IP failed-auth limiter --------------------------------------------

/// Per-IP wrong-password throttle (stage 1.10). Mirrors ConnectLimiter — pure,
/// `now`-injected (seconds, monotonic), sliding window, capped tracking — but
/// with an escalating lockout: once an IP records `threshold` failures inside
/// `window` seconds it is `blocked()` for a `cooldown` that starts at
/// `base_cooldown` and doubles on every further failure up to `max_cooldown`.
/// An IP that stops guessing for a whole `max_cooldown` has its escalation reset.
///
/// ⚠️ Unlike ConnectLimiter this **evicts the oldest entry rather than failing
/// open** when the tracking table is full: an over-full auth table is an attack
/// signal, not normal load, and the cost of a wrong eviction is a brief lockout
/// of one IP, not the memory blow-up a fail-open connect table would risk.
///
/// Per-IP only — a distributed guesser (many IPs, few tries each) still slips
/// through; see ROADMAP-SERVER §1.10 for the layered mitigations (loud global
/// counter, fail2ban jail, and ultimately an identity allow-list).
class AuthFailureLimiter {
  public:
    AuthFailureLimiter(size_t threshold = 5, double window_sec = 60.0,
                       double base_cooldown = 60.0, double max_cooldown = 3600.0,
                       size_t max_tracked = 8192)
        : threshold_(threshold), window_(window_sec),
          base_cooldown_(base_cooldown), max_cooldown_(max_cooldown),
          max_tracked_(max_tracked) {}

    /// 0 threshold = feature off (every call is a no-op / never blocks).
    void set_threshold(size_t t) { threshold_ = t; }
    bool enabled() const { return threshold_ > 0; }

    /// True while `ip` is inside its lockout window.
    bool blocked(const std::string& ip, double now) const {
        if (!enabled()) return false;
        auto it = entries_.find(ip);
        return it != entries_.end() && now < it->second.blocked_until;
    }

    /// Record one wrong-password attempt from `ip`. Returns the number of
    /// failures now counted inside the sliding window (0 when disabled).
    size_t record_failure(const std::string& ip, double now) {
        if (!enabled()) return 0;
        auto it = entries_.find(ip);
        if (it == entries_.end()) {
            if (entries_.size() >= max_tracked_) {
                sweep(now);
                if (entries_.size() >= max_tracked_) evict_oldest();
            }
            it = entries_.emplace(ip, Entry{}).first;
        }
        Entry& e = it->second;
        // Reset escalation for an IP that went quiet for a full max_cooldown.
        if (e.last_failure > 0.0 && now - e.last_failure > max_cooldown_)
            e.cooldown = 0.0;
        prune(e.recent, now);
        e.recent.push_back(now);
        e.last_failure = now;
        if (e.recent.size() >= threshold_) {
            e.cooldown = (e.cooldown <= 0.0)
                             ? base_cooldown_
                             : std::min(e.cooldown * 2.0, max_cooldown_);
            e.blocked_until = now + e.cooldown;
        }
        return e.recent.size();
    }

    size_t tracked() const { return entries_.size(); }

    /// Drop every IP that is neither blocked nor has an in-window failure and has
    /// been idle for a full max_cooldown.
    void sweep(double now) {
        for (auto it = entries_.begin(); it != entries_.end();) {
            prune(it->second.recent, now);
            const Entry& e = it->second;
            const bool idle = e.recent.empty() && now >= e.blocked_until &&
                              now - e.last_failure > max_cooldown_;
            it = idle ? entries_.erase(it) : std::next(it);
        }
    }

  private:
    struct Entry {
        std::vector<double> recent;     // failure timestamps inside the window
        double blocked_until = 0.0;
        double cooldown = 0.0;          // current escalation level (seconds)
        double last_failure = 0.0;
    };

    void prune(std::vector<double>& v, double now) {
        const double cutoff = now - window_;
        size_t keep = 0;
        while (keep < v.size() && v[keep] <= cutoff) ++keep;
        if (keep) v.erase(v.begin(), v.begin() + static_cast<long>(keep));
    }

    void evict_oldest() {
        auto oldest = entries_.begin();
        for (auto it = entries_.begin(); it != entries_.end(); ++it)
            if (it->second.last_failure < oldest->second.last_failure) oldest = it;
        if (oldest != entries_.end()) entries_.erase(oldest);
    }

    size_t threshold_;
    double window_;
    double base_cooldown_;
    double max_cooldown_;
    size_t max_tracked_;
    std::unordered_map<std::string, Entry> entries_;
};

// --- bounded, recency-ordered table (stage 7.23) -------------------------------

/// String-keyed table that never holds more than `capacity` entries: inserting a
/// new key past the cap evicts the least-recently-*written* one, and writing an
/// existing key refreshes it rather than duplicating it. Used for state keyed on a
/// username, which is untrusted input — a client can `JOIN` under a fresh name every
/// few seconds, so an unkeyed-growth `std::map` there is a slow memory and disk leak.
///
/// Recency is list order, not a timestamp, so unlike the limiters above it needs no
/// injected clock; `for_each` walks oldest -> newest so a caller that persists in
/// that order and reloads with `put` in file order gets its recency back across a
/// restart. Not internally locked — the caller holds whatever mutex guards it.
template <typename V>
class LruTable {
  public:
    explicit LruTable(size_t capacity = 10000) : cap_(capacity ? capacity : 1) {}

    /// Insert or refresh `key`. Returns how many entries were evicted to make room.
    size_t put(const std::string& key, const V& value) {
        auto it = index_.find(key);
        if (it != index_.end()) {
            it->second->second = value;
            order_.splice(order_.end(), order_, it->second);   // refresh: now newest
            return 0;
        }
        order_.emplace_back(key, value);
        index_[key] = std::prev(order_.end());
        return trim();
    }

    /// Lookup without refreshing — reading is not a reason to keep a row alive.
    const V* find(const std::string& key) const {
        auto it = index_.find(key);
        return it == index_.end() ? nullptr : &it->second->second;
    }

    /// Change the cap, evicting oldest-first if it now exceeds it. Returns evictions.
    size_t set_capacity(size_t capacity) {
        cap_ = capacity ? capacity : 1;
        return trim();
    }

    size_t size() const { return order_.size(); }
    size_t capacity() const { return cap_; }

    /// Visit every entry, oldest first: `fn(const std::string&, const V&)`.
    template <typename F>
    void for_each(F fn) const {
        for (const auto& kv : order_) fn(kv.first, kv.second);
    }

  private:
    size_t trim() {
        size_t evicted = 0;
        while (order_.size() > cap_) {
            index_.erase(order_.front().first);
            order_.pop_front();
            ++evicted;
        }
        return evicted;
    }

    using Order = std::list<std::pair<std::string, V>>;
    size_t cap_;
    Order order_;
    std::unordered_map<std::string, typename Order::iterator> index_;
};

// --- accept() failure policy (stages 7.10 / 7.25) --------------------------------

enum class AcceptErrorAction {
    Retry,        // routine (EINTR, a peer that reset before accept): loop again, silently
    BackoffSleep, // resource exhaustion: sleep before retrying or the loop pins a core
    LogRetry,     // anything else: retry, but rate-limit the log line
};

/// What an `accept()` loop should do about `err` (an `errno`). Shared by every accept
/// loop — the client listener, the control socket and `edenmatch` — because a
/// persistent `EMFILE` with no sleep is a tight 100 % CPU spin, and it was fixed on
/// one loop at a time.
inline AcceptErrorAction classify_accept_error(int err) {
    if (err == EINTR || err == ECONNABORTED) return AcceptErrorAction::Retry;
    if (err == EMFILE || err == ENFILE || err == ENOBUFS || err == ENOMEM)
        return AcceptErrorAction::BackoffSleep;
    return AcceptErrorAction::LogRetry;
}

}  // namespace ewb
