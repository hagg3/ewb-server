// hardening.h — the pure, testable half of ROADMAP-SERVER stage 1.7: username
// validation, per-connection token buckets, a per-IP connect limiter, `ACTION`
// payload validation and text sanitisation.
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

#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>
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
        if (!primed) {
            last = now;
            primed = true;
        } else if (now > last) {
            tokens += (now - last) * refill_per_sec;
            if (tokens > capacity) tokens = capacity;
            last = now;
        }
        if (tokens < cost) return false;
        tokens -= cost;
        return true;
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

}  // namespace ewb
