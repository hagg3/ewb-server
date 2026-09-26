// matchmaker.h — pure logic for `edenmatch`, the standalone Eden matchmaker.
//
// Eden's retail client cannot type an IP. The way onto a server is the in-game
// Server Browser, which is populated by a matchmaker: game servers hold an open
// TCP connection to it and register; game clients ask it for the list.
//
// The wire protocol here is the one described by Eden's developer (WORKING/
// matchmakerinfo.txt — a lossy paste, superseded on several points) cross-checked
// against the byte-exact browse capture of the *live* matchmaker in the private
// RE tree (CAPTURE-FINDINGS.md "Matchmaker SERVER: grammar — CONFIRMED").
//
// The SERVER: row width has three sources that disagree (stage 2.3):
//   4 fields — the developer's prose sketch;
//   6 fields — a community-supplied matchmaker implementation (…:hasPassword:players);
//   7 fields — the live browse capture (…:locked:players:flag6).
// We emit 7. The capture is the only one of the three taken off the wire of the
// server the retail client actually browses, and its 6th and 7th columns vary
// independently of one another — across rows and between two separate passes the
// count column tracks live joins while the last column stays set for a fixed
// subset of servers — so the 7th field is a real column, not a mis-split or a
// delimiter artifact of a 6-field row. The 6-field implementation is therefore an
// older or divergent branch, not the running build. Both other forms stay
// selectable for an A/B (`RowForm`, `edenmatch --prod-list` / `--short-list`);
// see `format_server_row`.
//
// Newline-framed, ':'-delimited, one message per line. Three kinds of peer:
//
//   game server -> matchmaker
//     REGISTER:name:port:hasPassword[:advertiseIP]   -> REGISTERED
//                                                       | REGISTERFAIL:full|taken|limit
//         advertiseIP must be a dotted IPv4 address and is only honoured as sent
//         when it is the peer's own address or the peer is trusted (stage 7.20,
//         see advertise_policy); otherwise the row lists the peer address.
//         registration lives as long as the TCP connection stays open; the
//         server must send *something* at least every ~45 s (HEARTBEAT_TTL_SEC).
//     PING[:<players>]                                (no reply) resets the timer;
//         a `:<players>` argument is the live join count, clamped to
//         [0, MAX_REPORTED_PLAYERS]; a bare PING leaves the last known count as-is
//
//   game client -> matchmaker (one-shot; connection closed after the reply)
//     LIST  (also accepted: LISTP, the browser-build verb)
//         -> SERVER:<name>:<ip>:<port>:<locked>:<players>:<flag6>   (repeated)
//            END
//     HOST:name[:hasPassword[:password]]
//         -> HOSTED:ip:port | HOSTFAIL:full | HOSTFAIL:spawn | HOSTFAIL:timeout
//         semantics (stage 2.4): a name with no live registration spawns a
//         fresh world (`world_<slug>.model`, one file per name — see
//         world_slug); a name matching an *offline* saved world reloads it;
//         a name matching a *live* registration returns that running server
//         instead of spawning a duplicate that would fight it for the file.
//
// This header is pure and unit-tested by matchmaker_test.cpp. The socket loop,
// the accept thread and the on-demand spawn live in edenmatch.cpp.
#pragma once

#include <arpa/inet.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace edenmatch {

constexpr int    DEFAULT_PORT       = 27020;  // edenmatch default; matches the retail matchmaker's port
constexpr size_t MAX_LINE           = 4096;   // dev: lines are capped at ~4 KB
constexpr int    HEARTBEAT_TTL_SEC  = 45;     // dev: "traffic at least every ~45 s or [dropped]"
constexpr size_t MAX_NAME_LEN       = 48;     // display name bytes; longer is truncated.
                                              // Sized to leave headroom inside the client's
                                              // 64-byte server-name field (stage 2.3).
constexpr size_t MAX_REGISTRATIONS  = 512;    // registry sanity cap (refuse beyond this)
constexpr int    MAX_REPORTED_PLAYERS = 1000; // `PING:<n>` clamp — production's ceiling
constexpr int    MAX_CONNS_GLOBAL   = 256;    // concurrent connections, all peers (stage 2.2)
constexpr int    MAX_CONNS_PER_IP   = 24;     // concurrent connections, single peer IP (stage 2.2)
constexpr size_t MAX_REGISTRATIONS_PER_PEER = 8;  // rows one untrusted peer IP may hold (stage 7.20)

// Drop a trailing byte sequence that a byte-wise length cap cut in half, so a
// truncated name is never left ending in a partial UTF-8 code point. Only the
// final sequence is examined — this is a truncation fixup, not a validator.
inline void trim_partial_utf8(std::string& s) {
    if (s.empty()) return;
    size_t i = s.size(), cont = 0;
    while (i > 0 && (static_cast<unsigned char>(s[i - 1]) & 0xC0) == 0x80 && cont < 3) {
        --i;
        ++cont;
    }
    if (i == 0) { s.clear(); return; }              // continuation bytes only
    unsigned char lead = static_cast<unsigned char>(s[i - 1]);
    if (lead < 0x80) { if (cont) s.resize(i); return; }  // ASCII + stray continuations
    size_t need = ((lead & 0xE0) == 0xC0) ? 2
                : ((lead & 0xF0) == 0xE0) ? 3
                : ((lead & 0xF8) == 0xF0) ? 4
                                          : 0;
    if (need == 0 || cont + 1 != need) s.resize(i - 1);
}

// Sanitise a display name for the browse list. The guarantee we need is exactly
// the framing guarantee and nothing more: a name can never carry a ':' (which
// would forge a field in a SERVER: row) or a '\n' (which would forge a whole
// line), nor any other control byte. So: ':' and every control byte are dropped,
// TAB and SPACE become a single space, whitespace runs collapse, the result is
// trimmed and capped at MAX_NAME_LEN bytes.
//
// Everything else printable survives, including punctuation and non-ASCII —
// this matches how the production matchmaker is known to behave, and it is why
// a name like "Ari's Server" now lists intact instead of as "Aris Server". The
// older alnum-only rule bought no safety the above does not already buy.
//
// This is *not* the function to use for anything that becomes a path; see
// world_slug below.
inline std::string sanitize_name(const std::string& in) {
    std::string out;
    bool pendingSpace = false;
    for (char c : in) {
        unsigned char u = static_cast<unsigned char>(c);
        if (c == ':') continue;                       // field delimiter
        if (u == ' ' || u == '\t') { pendingSpace = true; continue; }
        if (u < 0x20 || u == 0x7f) continue;          // '\n', '\r', every other control byte
        if (pendingSpace && !out.empty() && out.size() < MAX_NAME_LEN) out += ' ';
        pendingSpace = false;
        if (out.size() < MAX_NAME_LEN) out += c;
    }
    trim_partial_utf8(out);
    return out;
}

// Turn a HOST display name into a filesystem-safe slug for its per-name world
// file (stage 2.4): keep [a-z0-9] (case-folded), collapse every other byte
// into a single '_' separator, trim leading/trailing '_', cap length, empty
// result -> "world". This is a different job from sanitize_name (a path
// component, not a wire-protocol display string) so it is not reused: the
// output alphabet is narrow enough ([a-z0-9_]) that a path-traversal attempt
// like "../../etc/passwd" slugs to something inert ("etc_passwd") rather than
// needing special-case rejection.
constexpr size_t MAX_SLUG_LEN = 40;

inline std::string world_slug(const std::string& in) {
    std::string out;
    bool pendingSep = false;
    for (char c : in) {
        unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) {
            if (pendingSep && !out.empty() && out.size() < MAX_SLUG_LEN) out += '_';
            pendingSep = false;
            if (out.size() < MAX_SLUG_LEN) out += static_cast<char>(std::tolower(u));
        } else {
            pendingSep = true;
        }
    }
    return out.empty() ? "world" : out;
}

// Validate a dotted-quad IPv4 address and return it in canonical inet_ntop form
// (stage 7.20). Before this the REGISTER advertise field was copied verbatim, so
// any byte but ':' and '\n' — control bytes included — reached clients inside a
// SERVER: row, and the string never had to be an address at all.
inline bool canonical_ipv4(const std::string& s, std::string& out) {
    in_addr a{};
    if (s.empty() || inet_pton(AF_INET, s.c_str(), &a) != 1) return false;
    char buf[INET_ADDRSTRLEN] = {0};
    if (!inet_ntop(AF_INET, &a, buf, sizeof(buf))) return false;
    out = buf;
    return true;
}

inline bool ipv4_to_u32(const std::string& s, uint32_t& v) {
    in_addr a{};
    if (inet_pton(AF_INET, s.c_str(), &a) != 1) return false;
    v = ntohl(a.s_addr);
    return true;
}

// Peers whose REGISTER may advertise an address other than their own (stage
// 7.20). Loopback (127.0.0.0/8) is always trusted: it is a process on the
// matchmaker's own host — a HOST-spawned edenserver, or a server co-located with
// the matchmaker advertising its public address — which the operator already
// controls. Anything else must be listed with `edenmatch --trust-advertise`.
class TrustList {
public:
    // "a.b.c.d" or "a.b.c.d/n", comma-separated; whitespace around entries is
    // ignored. Returns false (list unchanged) if any entry is malformed.
    bool parse(const std::string& spec) {
        std::vector<Net> nets;
        size_t start = 0;
        for (size_t i = 0; i <= spec.size(); ++i) {
            if (i != spec.size() && spec[i] != ',') continue;
            std::string e = spec.substr(start, i - start);
            start = i + 1;
            while (!e.empty() && std::isspace(static_cast<unsigned char>(e.front()))) e.erase(0, 1);
            while (!e.empty() && std::isspace(static_cast<unsigned char>(e.back()))) e.pop_back();
            if (e.empty()) continue;
            int bits = 32;
            size_t slash = e.find('/');
            if (slash != std::string::npos) {
                std::string b = e.substr(slash + 1);
                char* end = nullptr;
                long n = std::strtol(b.c_str(), &end, 10);
                if (b.empty() || *end != '\0' || n < 0 || n > 32) return false;
                bits = static_cast<int>(n);
                e.resize(slash);
            }
            uint32_t addr;
            if (!ipv4_to_u32(e, addr)) return false;
            uint32_t mask = bits == 0 ? 0u : ~uint32_t(0) << (32 - bits);
            nets.push_back({addr & mask, mask});
        }
        nets_ = std::move(nets);
        return true;
    }

    bool trusted(const std::string& peerIp) const {
        uint32_t v;
        if (!ipv4_to_u32(peerIp, v)) return false;
        if ((v >> 24) == 127) return true;
        for (const auto& n : nets_)
            if ((v & n.mask) == n.net) return true;
        return false;
    }

private:
    struct Net { uint32_t net, mask; };
    std::vector<Net> nets_;
};

inline bool is_loopback_ipv4(const std::string& ip) {
    uint32_t v;
    return ipv4_to_u32(ip, v) && (v >> 24) == 127;
}

// One live server, keyed while connected by its owning connection id.
struct Registration {
    std::string name;         // already sanitised
    std::string ip;           // advertised address a client should dial
    int         port = 0;
    bool        hasPassword = false;
    int         players = 0;  // not carried by REGISTER; set by `PING:<n>` (see parse_ping)
    int         flag6   = 0;  // opaque mode/PvP bool from the browse grammar; 0 for our servers
    int64_t     lastSeen = 0; // monotonic seconds, supplied by the caller
    uint64_t    conn = 0;     // owning connection id
    std::string peer;         // TCP peer address of the owning connection (stage 7.20);
                              // empty for an entry reloaded from disk. Not persisted.
};

// Parse "REGISTER:name:port:hasPassword[:advertiseIP]".
//   - `name` is sanitised; an empty result after sanitising is rejected.
//   - `port` must be 1..65535.
//   - `hasPassword` is "1"/"0" (anything non-"0" and non-empty reads as 1).
//   - `advertiseIP`, when absent or empty, falls back to `peerIp` (the accepted
//     socket's remote address) — the common case of a server behind the same
//     NAT as, or on the same host as, the matchmaker still gets a usable row for
//     LAN clients, and a public server passes its real address explicitly.
//     When present it must be a dotted IPv4 address (stored canonicalised); a
//     non-address rejects the whole line. Whether it is *honoured* is a separate
//     question — see advertise_policy.
// Returns false (and leaves `out` untouched) on any malformed line.
inline bool parse_register(const std::string& line, const std::string& peerIp,
                           Registration& out) {
    if (line.rfind("REGISTER:", 0) != 0) return false;
    std::vector<std::string> f;
    size_t start = 0;
    for (size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == ':') {
            f.emplace_back(line.substr(start, i - start));
            start = i + 1;
        }
    }
    // f[0] == "REGISTER"; need at least name, port, hasPassword
    if (f.size() < 4) return false;
    std::string name = sanitize_name(f[1]);
    if (name.empty()) return false;
    char* end = nullptr;
    long port = std::strtol(f[2].c_str(), &end, 10);
    if (end == f[2].c_str() || *end != '\0' || port < 1 || port > 65535) return false;

    Registration r;
    r.name = std::move(name);
    r.port = static_cast<int>(port);
    r.hasPassword = !f[3].empty() && f[3] != "0";
    std::string adv = (f.size() >= 5) ? f[4] : std::string();
    if (adv.empty()) r.ip = peerIp;
    else if (!canonical_ipv4(adv, r.ip)) return false;
    if (r.ip.empty()) return false;
    out = std::move(r);
    return true;
}

// Decide which address a parsed registration is listed under (stage 7.20).
// Before this, whatever REGISTER's fifth field said was listed, and the registry
// dedupes on (ip, port) — so any peer could claim a real server's address and
// take over its row. TCP does not let a peer forge its own source address, so:
//   - `r.ip` is kept if it is the peer's own address, or `fallbackIp` (what an
//     advertise-less REGISTER would get anyway: the peer, or edenmatch's global
//     --advertise-ip), or the peer is trusted (TrustList);
//   - otherwise it is replaced by `fallbackIp`.
// A legitimate server behind NAT that auto-advertises its LAN address therefore
// lists under its NAT's public address — which is the one remote clients need.
// Returns true if the claim was overridden, so the caller can log it.
inline bool advertise_policy(Registration& r, const std::string& peerIp,
                             const std::string& fallbackIp, bool peerTrusted) {
    if (r.ip == peerIp || r.ip == fallbackIp || peerTrusted) return false;
    r.ip = fallbackIp;
    return true;
}

// Parse the heartbeat's optional player-count channel. A bare "PING" (no reply,
// no argument) just resets the TTL and leaves the last known count in place —
// returns false, `players` untouched. "PING:<n>" reports the live count,
// clamped to [0, MAX_REPORTED_PLAYERS]. Anything else after the colon (junk,
// negative, non-numeric) is treated the same as a bare PING: the heartbeat
// itself is never rejected on a malformed count.
inline bool parse_ping(const std::string& line, int& players) {
    if (line.rfind("PING:", 0) != 0) return false;
    std::string arg = line.substr(5);
    if (arg.empty()) return false;
    char* end = nullptr;
    long n = std::strtol(arg.c_str(), &end, 10);
    if (end == arg.c_str() || *end != '\0' || n < 0) return false;
    if (n > MAX_REPORTED_PLAYERS) n = MAX_REPORTED_PLAYERS;
    players = static_cast<int>(n);
    return true;
}

// On-disk persistence for the registry (`eden_registry.txt`, stage 2.2), so a
// matchmaker restart doesn't blank the browser for everyone while every server
// waits out its own reconnect delay. One line per entry, same fields as the
// SERVER: row minus the framing; `conn` and `lastSeen` are never serialized —
// a reloaded entry always comes back as conn=0 (orphaned) and gets its
// liveness re-established by a probe (see Registry::load_stale).
inline std::string serialize_entry(const Registration& r) {
    return r.name + ":" + r.ip + ":" + std::to_string(r.port) + ":" +
           (r.hasPassword ? "1" : "0") + ":" + std::to_string(r.players) + ":" +
           std::to_string(r.flag6);
}

inline std::string serialize_registry(const std::vector<Registration>& regs) {
    std::string out;
    for (const auto& r : regs) out += serialize_entry(r) + "\n";
    return out;
}

// Inverse of serialize_entry. Rejects anything that doesn't round-trip cleanly
// (wrong field count, bad port) rather than guessing — a corrupt registry file
// should lose entries, not fabricate broken ones.
inline bool parse_persisted_line(const std::string& line, Registration& out) {
    std::vector<std::string> f;
    size_t start = 0;
    for (size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == ':') {
            f.emplace_back(line.substr(start, i - start));
            start = i + 1;
        }
    }
    if (f.size() != 6) return false;
    if (f[0].empty() || f[1].empty()) return false;
    std::string ip;
    if (!canonical_ipv4(f[1], ip)) return false;   // a hand-edited file gets no bypass (7.20)
    char* end = nullptr;
    long port = std::strtol(f[2].c_str(), &end, 10);
    if (end == f[2].c_str() || *end != '\0' || port < 1 || port > 65535) return false;
    Registration r;
    r.name = sanitize_name(f[0]);
    if (r.name.empty()) return false;
    r.ip = std::move(ip);
    r.port = static_cast<int>(port);
    r.hasPassword = !f[3].empty() && f[3] != "0";
    long players = std::strtol(f[4].c_str(), &end, 10);
    r.players = (end == f[4].c_str()) ? 0 : static_cast<int>(players);
    long flag6 = std::strtol(f[5].c_str(), &end, 10);
    r.flag6 = (end == f[5].c_str()) ? 0 : static_cast<int>(flag6);
    out = std::move(r);
    return true;
}

// Global + per-IP concurrent connection caps (stage 2.2). Pure counting; the
// accept()/thread lifecycle lives in edenmatch.cpp, which acquires a slot right
// after accept() and releases it via RAII when the connection's thread exits,
// covering every exit path (LIST reply, HOST reply, REGISTER drop, error) —
// today's accept loop spawns a detached thread per connection with no cap at
// all.
class ConnCaps {
public:
    bool tryAcquire(const std::string& ip) {
        std::lock_guard<std::mutex> lk(mu_);
        if (total_ >= MAX_CONNS_GLOBAL) return false;
        int& c = perIp_[ip];
        if (c >= MAX_CONNS_PER_IP) return false;
        ++c;
        ++total_;
        return true;
    }
    void release(const std::string& ip) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = perIp_.find(ip);
        if (it != perIp_.end() && --it->second <= 0) perIp_.erase(it);
        if (total_ > 0) --total_;
    }
    int total() const { std::lock_guard<std::mutex> lk(mu_); return total_; }
    int forIp(const std::string& ip) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = perIp_.find(ip);
        return it == perIp_.end() ? 0 : it->second;
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, int> perIp_;
    int total_ = 0;
};

// Case-insensitively find a *live* (non-orphaned) registration by display
// name (stage 2.4's "join-existing": a HOST for a name that's already up
// should hand back the running server rather than spawn a duplicate that
// fights it for the same world file). An orphaned entry (conn == 0) doesn't
// count as live — its process may already be gone.
inline const Registration* find_live_by_name(const std::vector<Registration>& regs,
                                              const std::string& name) {
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    std::string target = lower(name);
    for (const auto& r : regs)
        if (r.conn != 0 && lower(r.name) == target) return &r;
    return nullptr;
}

// Which of the three attested SERVER: row widths to emit (see the header block).
enum class RowForm {
    Capture7 = 0,  // SERVER:name:ip:port:locked:players:flag6  — the live browse capture (default)
    Prod6,         // SERVER:name:ip:port:hasPassword:players   — a community implementation
    Sketch4,       // SERVER:name:ip:port:hasPassword           — the developer's prose sketch
};

// A browse-list row. `Capture7` is the default because it is the grammar the
// retail client is observed browsing every day; the two narrower forms are kept
// only so a live A/B is one flag away if a client ever turns out to want them.
inline std::string format_server_row(const Registration& r, RowForm form = RowForm::Capture7) {
    std::string s = "SERVER:" + r.name + ":" + r.ip + ":" + std::to_string(r.port) + ":" +
                    (r.hasPassword ? "1" : "0");
    if (form != RowForm::Sketch4) s += ":" + std::to_string(r.players);
    if (form == RowForm::Capture7) s += ":" + std::to_string(r.flag6);
    return s;
}

// Format a whole LIST reply: every live row, then a bare "END". A client
// distinguishes "no servers" from "not answered" by the terminator, so the
// terminator is always sent even when there are zero rows (the natural reading of
// the grammar; the empty case was never captured — CAPTURE-FINDINGS "Still
// needed" #1). Rows are sorted players-desc, then name, matching production's
// ordering (the busiest servers surface first in the browser).
inline std::string format_list(const std::vector<Registration>& regs,
                               RowForm form = RowForm::Capture7) {
    std::vector<Registration> sorted = regs;
    std::stable_sort(sorted.begin(), sorted.end(), [](const Registration& a, const Registration& b) {
        if (a.players != b.players) return a.players > b.players;
        return a.name < b.name;
    });
    std::string out;
    for (const auto& r : sorted) out += format_server_row(r, form) + "\n";
    out += "END\n";
    return out;
}

// In-memory registry. Thread-safe; the owning process holds one instance and
// each connection thread calls in. Liveness is primarily the TCP connection
// (remove() on close); `sweep()` is the ~45 s backstop for a wedged peer whose
// socket has not yet errored. All time is monotonic seconds passed by the caller
// so this stays testable without sleeping.
class Registry {
public:
    // Add or replace. The dedupe key is (ip, port), not (name, ip, port): the
    // dev's prose says name:ip:port, but the production matchmaker's own code
    // keys on ip:port (latestref-analysis-2026-09-12.md §5) — code wins, and it
    // matters here because the prose key let a server that renames itself
    // appear twice in the browser for up to the 45 s TTL (stage 2.2).
    //
    // Ownership (stage 7.20). `reg.peer` is the registering connection's TCP peer.
    //   - A live row (conn != 0) owned by a *different* peer is never displaced:
    //     `Taken`. The same peer may displace it — that is a restarted server whose
    //     old socket has not errored yet — and the old connection id is returned
    //     via `displaced` so the caller can close it.
    //   - An orphaned row (conn == 0, incl. one reloaded from disk) may be
    //     reclaimed by any registration that got past advertise_policy.
    //   - A connection owns at most one row: a re-REGISTER that changes port
    //     moves the row rather than adding a second one.
    //   - A new row is refused with `PeerLimit` once `reg.peer` already holds
    //     `maxPerPeer` rows (0 = no limit; the caller passes 0 for trusted peers,
    //     which is how a HOST-enabled matchmaker's loopback spawns fit), and with
    //     `Full` at MAX_REGISTRATIONS.
    enum class AddResult { Ok, Full, Taken, PeerLimit };

    AddResult add(const Registration& reg, uint64_t conn, int64_t now,
                  uint64_t* displaced = nullptr, size_t maxPerPeer = 0) {
        std::lock_guard<std::mutex> lk(mu_);
        if (displaced) *displaced = 0;
        auto same = [&](const Registration& e) { return e.ip == reg.ip && e.port == reg.port; };
        auto owned = [&](const Registration& e) { return conn != 0 && e.conn == conn; };

        Registration u = reg;
        u.conn = conn;
        u.lastSeen = now;

        auto hit = std::find_if(entries_.begin(), entries_.end(), same);
        if (hit != entries_.end()) {
            if (hit->conn != 0 && hit->conn != conn && hit->peer != reg.peer)
                return AddResult::Taken;
            if (displaced && hit->conn != conn) *displaced = hit->conn;
            u.players = hit->players;  // a re-register (e.g. rename) must not flicker
                                       // the row back to 0 (production behaviour)
            *hit = u;
            // this connection's previous row, if it just moved here
            entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                               [&](const Registration& e) { return owned(e) && !same(e); }),
                           entries_.end());
            return AddResult::Ok;
        }

        auto mine = std::find_if(entries_.begin(), entries_.end(), owned);
        if (mine != entries_.end()) {   // same connection, new ip:port: move the row
            u.players = mine->players;
            *mine = u;
            return AddResult::Ok;
        }
        if (maxPerPeer && !reg.peer.empty()) {
            size_t n = std::count_if(entries_.begin(), entries_.end(),
                                     [&](const Registration& e) { return e.peer == reg.peer; });
            if (n >= maxPerPeer) return AddResult::PeerLimit;
        }
        if (entries_.size() >= MAX_REGISTRATIONS) return AddResult::Full;
        entries_.push_back(std::move(u));
        return AddResult::Ok;
    }

    // Any traffic on a registered connection resets its TTL.
    void touch(uint64_t conn, int64_t now) {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& e : entries_)
            if (e.conn == conn) e.lastSeen = now;
    }

    // Update the reported player count for a registered connection (`PING:<n>`).
    // No-op if the connection has no entry (e.g. it raced a sweep/remove).
    void set_players(uint64_t conn, int players) {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& e : entries_)
            if (e.conn == conn) { e.players = players; break; }
    }

    void remove(uint64_t conn) {
        std::lock_guard<std::mutex> lk(mu_);
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                      [conn](const Registration& e) { return e.conn == conn; }),
                       entries_.end());
    }

    // Drop the connection *without* delisting the entry (stage 2.2): a brief
    // hiccup on the registration socket shouldn't remove a server that is
    // plainly still up. The entry survives with conn=0 ("orphaned" — nothing
    // for the caller to close) and its fate is decided by the next probe-aware
    // sweep(). `lastSeen` is untouched, so it still ages out normally if the
    // probe also fails.
    // Returns false if `conn` owned no row (e.g. it was displaced — stage 7.20).
    bool orphan(uint64_t conn) {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& e : entries_)
            if (e.conn == conn) { e.conn = 0; return true; }
        return false;
    }

    // Drop entries not seen within HEARTBEAT_TTL_SEC — unless `probe(ip, port)`
    // says the advertised address is still reachable, in which case the entry
    // is kept and its TTL refreshed instead ("alive ⇒ listed", stage 2.2).
    // `probe` runs with the registry unlocked (candidates are snapshotted
    // first, decisions applied after), so one slow or timing-out probe can't
    // block registrations. Omitting `probe` (or passing nullptr) reproduces the
    // old unconditional-drop behaviour, which is what the offline tests use.
    // Returns the connection ids of dropped entries (0 for an orphaned one —
    // the caller has nothing to close there) so live sockets can also be closed.
    std::vector<uint64_t> sweep(int64_t now,
                                 const std::function<bool(const std::string&, int)>& probe = nullptr) {
        struct Key { std::string ip; int port; uint64_t conn; };
        std::vector<Key> stale;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (const auto& e : entries_)
                if (now - e.lastSeen > HEARTBEAT_TTL_SEC) stale.push_back({e.ip, e.port, e.conn});
        }
        std::vector<Key> toDrop, toRefresh;
        for (const auto& k : stale) {
            if (probe && probe(k.ip, k.port)) toRefresh.push_back(k);
            else toDrop.push_back(k);
        }
        std::vector<uint64_t> droppedConns;
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& k : toRefresh)
            for (auto& e : entries_)
                if (e.ip == k.ip && e.port == k.port) { e.lastSeen = now; break; }
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                           [&](const Registration& e) {
                               for (const auto& k : toDrop) {
                                   if (e.ip == k.ip && e.port == k.port) {
                                       droppedConns.push_back(e.conn);
                                       return true;
                                   }
                               }
                               return false;
                           }),
                       entries_.end());
        return droppedConns;
    }

    // Reload persisted entries at startup, marked stale (conn=0, lastSeen set
    // far enough in the past that the very next sweep() probes them) so a bare
    // matchmaker restart never trusts a server that's actually gone by then —
    // it re-verifies instead of re-listing blind (stage 2.2).
    void load_stale(const std::vector<Registration>& regs, int64_t now) {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& r : regs) {
            if (entries_.size() >= MAX_REGISTRATIONS) break;
            Registration u = r;
            u.conn = 0;
            u.lastSeen = now - HEARTBEAT_TTL_SEC - 1;
            entries_.push_back(std::move(u));
        }
    }

    std::vector<Registration> snapshot() const {
        std::lock_guard<std::mutex> lk(mu_);
        return entries_;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lk(mu_);
        return entries_.size();
    }

private:
    mutable std::mutex mu_;
    std::vector<Registration> entries_;
};

}  // namespace edenmatch
