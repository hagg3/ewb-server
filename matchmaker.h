// matchmaker.h — pure logic for `edenmatch`, the standalone Eden matchmaker.
//
// Eden's retail client cannot type an IP. The way onto a server is the in-game
// Server Browser, which is populated by a matchmaker: game servers hold an open
// TCP connection to it and register; game clients ask it for the list.
//
// The wire protocol here is the one described by Eden's developer (WORKING/
// matchmakerinfo.txt) cross-checked against the byte-exact browse capture in the
// private RE tree (CAPTURE-FINDINGS.md "Matchmaker SERVER: grammar — CONFIRMED").
// Where the two disagree — the developer sketch lists a short 4-field SERVER row,
// the capture shows 7 fields — the capture wins for what we emit, because that is
// what a real client was observed parsing; see `format_server_row`.
//
// Newline-framed, ':'-delimited, one message per line. Three kinds of peer:
//
//   game server -> matchmaker
//     REGISTER:name:port:hasPassword[:advertiseIP]   -> REGISTERED
//         registration lives as long as the TCP connection stays open; the
//         server must send *something* at least every ~45 s (HEARTBEAT_TTL_SEC).
//     PING                                            (no reply) resets the timer
//
//   game client -> matchmaker (one-shot; connection closed after the reply)
//     LIST  (also accepted: LISTP, the browser-build verb)
//         -> SERVER:<name>:<ip>:<port>:<locked>:<players>:<flag6>   (repeated)
//            END
//     HOST:name[:hasPassword[:password]]
//         -> HOSTED:ip:port | HOSTFAIL:full | HOSTFAIL:spawn | HOSTFAIL:timeout
//
// This header is pure and unit-tested by matchmaker_test.cpp. The socket loop,
// the accept thread and the on-demand spawn live in edenmatch.cpp.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace edenmatch {

constexpr int    DEFAULT_PORT       = 27020;  // edenmatch default; the retail IP is 45.79.193.87
constexpr size_t MAX_LINE           = 4096;   // dev: lines are capped at ~4 KB
constexpr int    HEARTBEAT_TTL_SEC  = 45;     // dev: "traffic at least every ~45 s or [dropped]"
constexpr size_t MAX_NAME_LEN       = 32;     // display name; longer is truncated
constexpr size_t MAX_REGISTRATIONS  = 512;    // registry sanity cap (refuse beyond this)

// Names are sanitised to alphanumerics + single spaces (dev note: "Names are
// sanitized (alnum + s[paces])..."). Everything else is dropped, whitespace runs
// collapse to one space, and the result is trimmed and length-capped. The point
// is a hard guarantee that a name can never carry a ':' or '\n', so neither the
// colon split in a SERVER: row nor the newline framing can be forged from it.
inline std::string sanitize_name(const std::string& in) {
    std::string out;
    bool pendingSpace = false;
    for (char c : in) {
        unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) {
            if (pendingSpace && !out.empty() && out.size() < MAX_NAME_LEN) out += ' ';
            pendingSpace = false;
            if (out.size() < MAX_NAME_LEN) out += c;
        } else if (c == ' ' || c == '\t') {
            pendingSpace = true;
        }
        // any other byte: dropped entirely
    }
    return out;
}

// One live server, keyed while connected by its owning connection id.
struct Registration {
    std::string name;         // already sanitised
    std::string ip;           // advertised address a client should dial
    int         port = 0;
    bool        hasPassword = false;
    int         players = 0;  // not carried by REGISTER; reserved for a future count channel
    int         flag6   = 0;  // opaque mode/PvP bool from the browse grammar; 0 for our servers
    int64_t     lastSeen = 0; // monotonic seconds, supplied by the caller
    uint64_t    conn = 0;     // owning connection id
};

// Parse "REGISTER:name:port:hasPassword[:advertiseIP]".
//   - `name` is sanitised; an empty result after sanitising is rejected.
//   - `port` must be 1..65535.
//   - `hasPassword` is "1"/"0" (anything non-"0" and non-empty reads as 1).
//   - `advertiseIP`, when absent or empty, falls back to `peerIp` (the accepted
//     socket's remote address) — the common case of a server behind the same
//     NAT as, or on the same host as, the matchmaker still gets a usable row for
//     LAN clients, and a public server passes its real address explicitly.
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
    r.ip = adv.empty() ? peerIp : adv;
    if (r.ip.empty()) return false;
    out = std::move(r);
    return true;
}

// A browse-list row. The 7-field form is the capture-confirmed grammar the retail
// client parses:  SERVER:<name>:<ip>:<port>:<locked>:<playerCount>:<flag6>
// `shortForm` emits the developer-sketch 4-field variant instead
// (SERVER:<name>:<ip>:<port>:<hasPassword>) — kept only for an A/B against a
// client that turns out to want it; the default is the observed grammar.
inline std::string format_server_row(const Registration& r, bool shortForm = false) {
    std::string s = "SERVER:" + r.name + ":" + r.ip + ":" + std::to_string(r.port) + ":" +
                    (r.hasPassword ? "1" : "0");
    if (!shortForm) s += ":" + std::to_string(r.players) + ":" + std::to_string(r.flag6);
    return s;
}

// Format a whole LIST reply: every live row, then a bare "END". A client
// distinguishes "no servers" from "not answered" by the terminator, so the
// terminator is always sent even when there are zero rows (the natural reading of
// the grammar; the empty case was never captured — CAPTURE-FINDINGS "Still
// needed" #1).
inline std::string format_list(const std::vector<Registration>& regs, bool shortForm = false) {
    std::string out;
    for (const auto& r : regs) out += format_server_row(r, shortForm) + "\n";
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
    // Add or replace. The dedupe key is (name, ip, port): "Re-registering with
    // the same name:ip:port replaces the old entry." A replaced entry's old
    // connection id is returned via `displaced` (0 if none) so the caller can
    // drop that now-stale connection. Returns false if the registry is full.
    bool add(const Registration& reg, uint64_t conn, int64_t now, uint64_t* displaced = nullptr) {
        std::lock_guard<std::mutex> lk(mu_);
        if (displaced) *displaced = 0;
        for (auto& e : entries_) {
            if (e.name == reg.name && e.ip == reg.ip && e.port == reg.port) {
                if (displaced && e.conn != conn) *displaced = e.conn;
                Registration u = reg;
                u.conn = conn;
                u.lastSeen = now;
                e = u;
                return true;
            }
        }
        if (entries_.size() >= MAX_REGISTRATIONS) return false;
        Registration u = reg;
        u.conn = conn;
        u.lastSeen = now;
        entries_.push_back(std::move(u));
        return true;
    }

    // Any traffic on a registered connection resets its TTL.
    void touch(uint64_t conn, int64_t now) {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& e : entries_)
            if (e.conn == conn) e.lastSeen = now;
    }

    void remove(uint64_t conn) {
        std::lock_guard<std::mutex> lk(mu_);
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                      [conn](const Registration& e) { return e.conn == conn; }),
                       entries_.end());
    }

    // Drop entries not seen within HEARTBEAT_TTL_SEC. Returns the connection ids
    // that were dropped, so the caller can also close those sockets.
    std::vector<uint64_t> sweep(int64_t now) {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<uint64_t> dropped;
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                           [&](const Registration& e) {
                               if (now - e.lastSeen > HEARTBEAT_TTL_SEC) {
                                   dropped.push_back(e.conn);
                                   return true;
                               }
                               return false;
                           }),
                       entries_.end());
        return dropped;
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
