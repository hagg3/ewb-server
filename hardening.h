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

}  // namespace ewb
