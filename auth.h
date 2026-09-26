// auth.h — the pure, testable half of ROADMAP-SERVER stage 8.6: player identity
// by operator-issued PIN.
//
// Why this exists. `JOIN` carries a username and a *shared* world password, so a
// name is only a claim. Before this stage `eden_ops.txt` levels were keyed on that
// claim: anyone who joined under an operator's name while the operator was
// offline got their level. A protected zone that honoured a bypass level would
// inherit the same hole. So a name can now carry a PIN:
//
//   * the operator issues it (`edenctl passwd <name>`) — never self-registered,
//     so nobody can squat a name;
//   * the player proves it with `/login <pin>` in chat (a `/`-line reaches only
//     the server; it is never broadcast);
//   * a name **with** an entry gets no more than `--default-level` until it has
//     logged in, and only a logged-in session may bypass a zone. A name without
//     an entry behaves exactly as before.
//
// This header owns the parts with no socket, clock or randomness in them:
// SHA-256, HMAC-SHA-256, PBKDF2-HMAC-SHA-256 (RFC 8018), the PIN rules, and the
// `eden_auth.txt` grammar. The server supplies the random bytes (salt and PIN)
// and the locking. There is no crypto dependency in this project (zlib only), so
// the hash is implemented here and pinned by published test vectors in
// auth_test.cpp.
//
// What a PIN is and is not. The wire is plaintext, so a PIN is exactly as secret
// as the world password already is: it stops a *casual* impersonator, not someone
// who can read the player's traffic. The file stores only a salted PBKDF2 hash,
// so reading `eden_auth.txt` does not reveal a PIN directly — but an 8-digit PIN
// is a small space, so the file is still written `0600` and should be treated like
// the password file.
//
// Grammar, one name per line (`#` comments and blank lines ignored):
//
//   name:pbkdf2-sha256:<iterations>:<salt hex>:<hash hex>
//
// A malformed line fails the whole load: an auth file that half-loaded would
// silently hand the missing names' op levels back to whoever claims them.
//
// Pure and unit-tested by auth_test.cpp.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <functional>
#include <istream>
#include <map>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#include "hardening.h"   // validate_username, const_time_eq

namespace ewb {

// --- SHA-256 (FIPS 180-4) ------------------------------------------------------

class Sha256 {
  public:
    static constexpr size_t DIGEST = 32;
    static constexpr size_t BLOCK  = 64;

    Sha256() { reset(); }

    void reset() {
        static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        std::memcpy(h_, init, sizeof h_);
        len_ = 0;
        fill_ = 0;
    }

    void update(const void* data, size_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        len_ += n;
        if (fill_) {
            const size_t take = n < BLOCK - fill_ ? n : BLOCK - fill_;
            std::memcpy(buf_ + fill_, p, take);
            fill_ += take; p += take; n -= take;
            if (fill_ < BLOCK) return;
            compress(buf_);
            fill_ = 0;
        }
        while (n >= BLOCK) { compress(p); p += BLOCK; n -= BLOCK; }
        if (n) { std::memcpy(buf_, p, n); fill_ = n; }
    }

    void final(uint8_t out[DIGEST]) {
        const uint64_t bits = len_ * 8;
        const uint8_t pad = 0x80;
        update(&pad, 1);
        const uint8_t zero = 0;
        while (fill_ != 56) update(&zero, 1);
        uint8_t lenb[8];
        for (int i = 0; i < 8; ++i) lenb[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
        update(lenb, 8);
        for (int i = 0; i < 8; ++i) {
            out[4 * i]     = static_cast<uint8_t>(h_[i] >> 24);
            out[4 * i + 1] = static_cast<uint8_t>(h_[i] >> 16);
            out[4 * i + 2] = static_cast<uint8_t>(h_[i] >> 8);
            out[4 * i + 3] = static_cast<uint8_t>(h_[i]);
        }
    }

  private:
    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void compress(const uint8_t* b) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t)b[4 * i] << 24 | (uint32_t)b[4 * i + 1] << 16 |
                   (uint32_t)b[4 * i + 2] << 8 | (uint32_t)b[4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h_[0], bb = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = h + S1 + ch + k[i] + w[i];
            const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t mj = (a & bb) ^ (a & c) ^ (bb & c);
            const uint32_t t2 = S0 + mj;
            h = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
        }
        h_[0] += a; h_[1] += bb; h_[2] += c; h_[3] += d;
        h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
    }

    uint32_t h_[8];
    uint64_t len_;
    uint8_t  buf_[BLOCK];
    size_t   fill_;
};

inline std::string sha256(const std::string& data) {
    Sha256 s;
    s.update(data.data(), data.size());
    uint8_t d[Sha256::DIGEST];
    s.final(d);
    return std::string(reinterpret_cast<const char*>(d), sizeof d);
}

// --- HMAC-SHA-256 (RFC 2104) -----------------------------------------------------

/// Keyed with the inner/outer pads precomputed once, because PBKDF2 calls it
/// tens of thousands of times with the same key.
class HmacSha256 {
  public:
    explicit HmacSha256(const std::string& key) {
        uint8_t k[Sha256::BLOCK] = {0};
        if (key.size() > Sha256::BLOCK) {
            const std::string d = sha256(key);
            std::memcpy(k, d.data(), d.size());
        } else {
            std::memcpy(k, key.data(), key.size());
        }
        uint8_t ipad[Sha256::BLOCK], opad[Sha256::BLOCK];
        for (size_t i = 0; i < Sha256::BLOCK; ++i) {
            ipad[i] = static_cast<uint8_t>(k[i] ^ 0x36);
            opad[i] = static_cast<uint8_t>(k[i] ^ 0x5c);
        }
        inner_.update(ipad, sizeof ipad);
        outer_.update(opad, sizeof opad);
        std::memset(k, 0, sizeof k);
    }

    void mac(const void* msg, size_t n, uint8_t out[Sha256::DIGEST]) const {
        Sha256 in = inner_;
        in.update(msg, n);
        uint8_t d[Sha256::DIGEST];
        in.final(d);
        Sha256 ou = outer_;
        ou.update(d, sizeof d);
        ou.final(out);
    }

  private:
    Sha256 inner_, outer_;
};

inline std::string hmac_sha256(const std::string& key, const std::string& msg) {
    uint8_t d[Sha256::DIGEST];
    HmacSha256(key).mac(msg.data(), msg.size(), d);
    return std::string(reinterpret_cast<const char*>(d), sizeof d);
}

// --- PBKDF2-HMAC-SHA-256 (RFC 8018 §5.2) -----------------------------------------

inline std::string pbkdf2_sha256(const std::string& password, const std::string& salt,
                                 uint32_t iterations, size_t dk_len) {
    const HmacSha256 prf(password);
    std::string out;
    out.reserve(dk_len);
    for (uint32_t block = 1; out.size() < dk_len; ++block) {
        std::string s1 = salt;
        s1.push_back(static_cast<char>(block >> 24));
        s1.push_back(static_cast<char>(block >> 16));
        s1.push_back(static_cast<char>(block >> 8));
        s1.push_back(static_cast<char>(block));
        uint8_t u[Sha256::DIGEST], t[Sha256::DIGEST];
        prf.mac(s1.data(), s1.size(), u);
        std::memcpy(t, u, sizeof t);
        for (uint32_t i = 1; i < iterations; ++i) {
            prf.mac(u, sizeof u, u);
            for (size_t j = 0; j < sizeof t; ++j) t[j] ^= u[j];
        }
        const size_t take = dk_len - out.size() < sizeof t ? dk_len - out.size() : sizeof t;
        out.append(reinterpret_cast<const char*>(t), take);
    }
    return out;
}

// --- hex -----------------------------------------------------------------------

inline std::string auth_hex(const std::string& bytes) {
    static const char* d = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) { out.push_back(d[c >> 4]); out.push_back(d[c & 15]); }
    return out;
}

/// Lower- or upper-case hex, even length. False (out untouched) on anything else.
inline bool auth_unhex(const std::string& hex, std::string& out) {
    if (hex.size() % 2) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string r;
    r.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nib(hex[i]), lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        r.push_back(static_cast<char>(hi << 4 | lo));
    }
    out = std::move(r);
    return true;
}

// --- PINs ------------------------------------------------------------------------

/// Digits in an issued PIN. 10^8 guesses against a login that is throttled per
/// connection and per IP (see server_posix.cpp `/login`) is out of reach online;
/// offline, against a stolen `eden_auth.txt`, it is not — which is why the file
/// is `0600` and the hash is slow.
constexpr int AUTH_PIN_DIGITS = 8;

/// PBKDF2 iterations for a newly issued PIN. Stored per record, so raising it
/// later only affects PINs issued after the change. Chosen so one verify costs a
/// few tens of milliseconds on a small VPS — enough to make an offline search of
/// the 10^8 space expensive, cheap enough that a throttled `/login` is no CPU
/// lever. (auth_test.cpp prints the measured cost.)
constexpr uint32_t AUTH_PBKDF2_ITERS = 100000;

/// Bounds a loaded record must respect, so a hand-edited file cannot make one
/// `/login` cost minutes (`iterations`) or pass with a trivially short hash.
constexpr uint32_t AUTH_ITERS_MIN = 1000;
constexpr uint32_t AUTH_ITERS_MAX = 10000000;
constexpr size_t   AUTH_SALT_BYTES = 16;
constexpr size_t   AUTH_HASH_BYTES = 32;

/// Source of random bytes: fills `n` bytes at `p`, returns false on failure.
/// The server passes getentropy(); tests pass a fixed stream.
using AuthRandom = std::function<bool(uint8_t* p, size_t n)>;

/// A uniformly random `AUTH_PIN_DIGITS`-digit string (leading zeros allowed).
/// Each digit is drawn by rejection sampling from a random byte (values 250..255
/// are discarded), so no digit is more likely than another.
inline bool auth_make_pin(const AuthRandom& rnd, std::string& pin) {
    std::string out;
    uint8_t buf[32];
    while ((int)out.size() < AUTH_PIN_DIGITS) {
        if (!rnd(buf, sizeof buf)) return false;
        for (uint8_t b : buf) {
            if (b >= 250) continue;
            out.push_back(static_cast<char>('0' + b % 10));
            if ((int)out.size() == AUTH_PIN_DIGITS) break;
        }
    }
    pin = std::move(out);
    return true;
}

/// Exactly AUTH_PIN_DIGITS ASCII digits. Checked before any hashing, so a
/// malformed guess costs the server nothing.
inline bool auth_pin_wellformed(const std::string& pin) {
    if ((int)pin.size() != AUTH_PIN_DIGITS) return false;
    for (char c : pin)
        if (c < '0' || c > '9') return false;
    return true;
}

// --- records and the file ----------------------------------------------------------

struct AuthRecord {
    uint32_t    iterations = AUTH_PBKDF2_ITERS;
    std::string salt;   // raw bytes
    std::string hash;   // raw bytes, pbkdf2(pin, salt, iterations, AUTH_HASH_BYTES)
};

inline bool auth_make_record(const std::string& pin, const AuthRandom& rnd, AuthRecord& out,
                             uint32_t iterations = AUTH_PBKDF2_ITERS) {
    uint8_t salt[AUTH_SALT_BYTES];
    if (!rnd(salt, sizeof salt)) return false;
    AuthRecord r;
    r.iterations = iterations;
    r.salt.assign(reinterpret_cast<const char*>(salt), sizeof salt);
    r.hash = pbkdf2_sha256(pin, r.salt, iterations, AUTH_HASH_BYTES);
    out = std::move(r);
    return true;
}

/// True iff `pin` is the record's PIN. The final compare is constant-time; a
/// malformed PIN is refused before the (slow) derivation.
inline bool auth_verify(const AuthRecord& r, const std::string& pin) {
    if (!auth_pin_wellformed(pin)) return false;
    const std::string d = pbkdf2_sha256(pin, r.salt, r.iterations, r.hash.size());
    return const_time_eq(d, r.hash);
}

constexpr const char* AUTH_SCHEME = "pbkdf2-sha256";

inline std::string auth_serialize_line(const std::string& name, const AuthRecord& r) {
    return name + ":" + AUTH_SCHEME + ":" + std::to_string(r.iterations) + ":" +
           auth_hex(r.salt) + ":" + auth_hex(r.hash);
}

/// Parse one non-comment, non-blank line. False with `err` set on anything
/// malformed; never partially accepted.
inline bool auth_parse_line(const std::string& line, std::string& name, AuthRecord& out,
                            std::string* err = nullptr) {
    auto fail = [&](const char* why) { if (err) *err = why; return false; };
    std::vector<std::string> f;
    size_t start = 0;
    for (size_t i = 0; i <= line.size(); ++i)
        if (i == line.size() || line[i] == ':') { f.push_back(line.substr(start, i - start)); start = i + 1; }
    if (f.size() != 5) return fail("expected 5 fields");
    if (validate_username(f[0]) != NameVerdict::Ok) return fail("invalid name");
    if (f[1] != AUTH_SCHEME) return fail("unknown hash scheme");
    char* end = nullptr;
    errno = 0;
    const unsigned long long it = std::strtoull(f[2].c_str(), &end, 10);
    if (f[2].empty() || *end != '\0' || errno == ERANGE || f[2][0] == '-') return fail("non-numeric iterations");
    if (it < AUTH_ITERS_MIN || it > AUTH_ITERS_MAX) return fail("iterations out of range");
    AuthRecord r;
    r.iterations = static_cast<uint32_t>(it);
    if (!auth_unhex(f[3], r.salt) || r.salt.size() < 8 || r.salt.size() > 64) return fail("bad salt");
    if (!auth_unhex(f[4], r.hash) || r.hash.size() != AUTH_HASH_BYTES) return fail("bad hash");
    name = f[0];
    out = std::move(r);
    return true;
}

/// Every name that carries a PIN. Not internally locked.
class AuthFile {
  public:
    bool has(const std::string& name) const { return entries_.count(name) > 0; }
    const AuthRecord* find(const std::string& name) const {
        auto it = entries_.find(name);
        return it == entries_.end() ? nullptr : &it->second;
    }
    void set(const std::string& name, const AuthRecord& r) { entries_[name] = r; }
    bool erase(const std::string& name) { return entries_.erase(name) > 0; }
    size_t size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }

    /// Names in sorted order (the file's order too, so a save is deterministic).
    std::vector<std::string> names() const {
        std::vector<std::string> out;
        for (const auto& kv : entries_) out.push_back(kv.first);
        return out;
    }

    /// All-or-nothing: on the first malformed line or duplicate name, `out` is
    /// untouched and `err` / `errLine` (1-based) say why.
    static bool load(std::istream& in, AuthFile& out, std::string* err = nullptr, int* errLine = nullptr) {
        AuthFile built;
        std::string line;
        int n = 0;
        while (std::getline(in, line)) {
            ++n;
            size_t b = 0, e = line.size();
            while (e > b && (line[e - 1] == '\r' || line[e - 1] == ' ' || line[e - 1] == '\t')) --e;
            while (b < e && (line[b] == ' ' || line[b] == '\t')) ++b;
            if (b >= e || line[b] == '#') continue;
            std::string name, why;
            AuthRecord r;
            if (!auth_parse_line(line.substr(b, e - b), name, r, &why)) {
                if (err) *err = why;
                if (errLine) *errLine = n;
                return false;
            }
            if (built.has(name)) {
                if (err) *err = "duplicate name";
                if (errLine) *errLine = n;
                return false;
            }
            built.set(name, r);
        }
        out = std::move(built);
        return true;
    }

    std::string serialize() const {
        std::ostringstream ss;
        ss << "# eden_auth.txt — login PINs (ROADMAP-SERVER 8.6). Salted PBKDF2-HMAC-SHA-256;\n"
              "# the PIN itself is never stored. Issue with `edenctl passwd <name>`.\n"
              "# name:pbkdf2-sha256:<iterations>:<salt hex>:<hash hex>\n";
        for (const auto& kv : entries_) ss << auth_serialize_line(kv.first, kv.second) << "\n";
        return ss.str();
    }

  private:
    std::map<std::string, AuthRecord> entries_;
};

// --- what a session is allowed -------------------------------------------------------

/// The op level a session actually gets. `fileLevel` is the name's `eden_ops.txt`
/// level (or `def` if it has none); `hasPin` is whether the name has an auth
/// entry; `verified` is whether this session has logged in.
///
/// A PIN-protected name that has not logged in gets the *lower* of the default
/// and its own level — never more than an anonymous name, and never more than
/// the real owner would get. A name with no PIN is unchanged (its claimed level).
inline int auth_effective_level(int fileLevel, int def, bool hasPin, bool verified) {
    if (hasPin && !verified) return fileLevel < def ? fileLevel : def;
    return fileLevel;
}

/// The level a session may use to bypass a protected zone, or -1 for "none".
/// Only a logged-in session bypasses anything: a claimed name is not an identity.
inline int auth_zone_bypass_level(int effectiveLevel, bool verified) {
    return verified ? effectiveLevel : -1;
}

}  // namespace ewb
