// zones.h — the pure, testable half of ROADMAP-SERVER stage 8.1: anti-grief
// protected-zone model.
//
// A zone is a box of the world an operator has marked protected. This header
// owns only the model: the `eden_zones.txt` grammar, load/normalise/validate,
// and the two lookups enforcement will need (`ZoneSet::blocking`,
// `ZoneSet::intersects`). No sockets, no globals, no world model — the server
// wires this up at the four edit call sites in stage 8.2. See
// WORKING/anti-grief-zones-plan-2026-09-17.md § "Zone model (zones.h ...)" for
// the design this implements.
//
// Grammar, one zone per line:
//
//   name:x0:y0:z0:x1:y1:z1:flags[:level]
//
//   spawn:65500:0:65500:65572:255:65572:all
//   museum:65800:30:65400:65850:90:65460:all:2
//
//   * name  — [A-Za-z0-9_-]{1,32}, unique within the set.
//   * box   — inclusive block coordinates; either corner order is accepted and
//             normalised (min/max per axis) on load.
//   * flags — v1 recognises only "all" (denies every edit) and "off" (kept on
//             file but not enforced). Any other token is a hard parse error —
//             a typo must never silently mean "unprotected" (plan rationale).
//   * level — optional trailing integer 0..2, the lowest op level that bypasses
//             the zone. Honoured since stage 8.6, and **only** for a session that
//             has logged in with its PIN (auth.h): a claimed name never bypasses
//             anything. No level = nobody bypasses it in game.
//
// Coordinates share the server's existing 24-bit x/z range (server_posix.cpp
// ~2177, ~2595: 0..0xFFFFFF) and its world height (server_posix.cpp ~237:
// SV_WORLD_HEIGHT = 256, so y in 0..255). A box outside that range is refused
// at load, same as an unknown flag or a duplicate name.
//
// Persistence follows the `eden_ops.txt` / `eden_signs.txt` pattern: plain
// text, one record per line, `#` comments and blank lines ignored, written
// with durable_write.h's atomic replace (the stage 7.29 convention) rather
// than a raw ofstream + rename.
//
// Pure and unit-tested by zones_test.cpp.

#pragma once

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <istream>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#include "durable_write.h"

namespace ewb {

// Coordinate range shared with the rest of the server (server_posix.cpp: x/z
// 0..0xFFFFFF, y 0..SV_WORLD_HEIGHT-1).
constexpr int64_t ZONE_XZ_MIN = 0;
constexpr int64_t ZONE_XZ_MAX = 0xFFFFFF;
constexpr int64_t ZONE_Y_MIN  = 0;
constexpr int64_t ZONE_Y_MAX  = 255;  // SV_WORLD_HEIGHT - 1

// Hard cap on the number of zones a set may hold; loading a 257th is a
// parse-time error (plan § "Caps").
constexpr size_t ZONE_MAX = 256;

constexpr size_t ZONE_NAME_MAX_LEN = 32;

inline bool zone_name_valid(const std::string& name) {
    if (name.empty() || name.size() > ZONE_NAME_MAX_LEN) return false;
    for (char c : name) {
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    }
    return true;
}

inline bool zone_xz_in_range(int64_t v) { return v >= ZONE_XZ_MIN && v <= ZONE_XZ_MAX; }
inline bool zone_y_in_range(int64_t v)  { return v >= ZONE_Y_MIN && v <= ZONE_Y_MAX; }

/// One protected box. Coordinates are always stored normalised (lo <= hi per
/// axis) regardless of the corner order in the source line.
struct Zone {
    std::string name;
    int64_t x0 = 0, y0 = 0, z0 = 0;  // inclusive, normalised lo
    int64_t x1 = 0, y1 = 0, z1 = 0;  // inclusive, normalised hi
    bool enforced = true;   // flag "all" (true) vs "off" (false)
    bool hasLevel = false;  // a trailing level field was present
    int  level = 0;         // lowest logged-in op level that bypasses the zone (8.6)

    bool contains(int64_t x, int64_t y, int64_t z) const {
        return x >= x0 && x <= x1 && y >= y0 && y <= y1 && z >= z0 && z <= z1;
    }

    /// True if a session whose zone-bypass level is `level` (auth.h — -1 for
    /// "not logged in") may edit inside this zone.
    bool bypassedBy(int level) const { return hasLevel && level >= 0 && level >= this->level; }

    bool overlaps(int64_t bx0, int64_t by0, int64_t bz0,
                  int64_t bx1, int64_t by1, int64_t bz1) const {
        return x0 <= bx1 && bx0 <= x1 &&
               y0 <= by1 && by0 <= y1 &&
               z0 <= bz1 && bz0 <= z1;
    }
};

/// Parse one non-comment, non-blank `eden_zones.txt` line into `out`.
/// Returns false on any malformed line: bad name, wrong field count, box
/// coordinates out of range, or an unrecognised flag — never partially
/// accepted, per the plan's "typo never silently unprotected" rule.
inline bool zone_parse_line(const std::string& line, Zone& out, std::string* err = nullptr) {
    auto fail = [&](const char* why) { if (err) *err = why; return false; };

    std::vector<std::string> f;
    size_t start = 0;
    for (size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == ':') {
            f.emplace_back(line.substr(start, i - start));
            start = i + 1;
        }
    }
    if (f.size() < 8 || f.size() > 9) return fail("expected 8 or 9 fields");

    if (!zone_name_valid(f[0])) return fail("invalid name");

    int64_t v[6];
    for (int i = 0; i < 6; ++i) {
        const std::string& s = f[i + 1];
        if (s.empty()) return fail("empty coordinate");
        char* end = nullptr;
        errno = 0;
        long long n = std::strtoll(s.c_str(), &end, 10);
        if (end == s.c_str() || *end != '\0' || errno == ERANGE) return fail("non-numeric coordinate");
        v[i] = n;
    }
    int64_t x0 = std::min(v[0], v[3]), x1 = std::max(v[0], v[3]);
    int64_t y0 = std::min(v[1], v[4]), y1 = std::max(v[1], v[4]);
    int64_t z0 = std::min(v[2], v[5]), z1 = std::max(v[2], v[5]);
    if (!zone_xz_in_range(x0) || !zone_xz_in_range(x1) ||
        !zone_xz_in_range(z0) || !zone_xz_in_range(z1))
        return fail("x/z coordinate out of range");
    if (!zone_y_in_range(y0) || !zone_y_in_range(y1))
        return fail("y coordinate out of range");

    const std::string& flag = f[7];
    bool enforced;
    if (flag == "all") enforced = true;
    else if (flag == "off") enforced = false;
    else return fail("unknown flag");

    bool hasLevel = false;
    int level = 0;
    if (f.size() == 9) {
        const std::string& lv = f[8];
        if (lv.empty()) return fail("empty level");
        char* end = nullptr;
        errno = 0;
        long n = std::strtol(lv.c_str(), &end, 10);
        if (end == lv.c_str() || *end != '\0' || errno == ERANGE) return fail("non-numeric level");
        // Op levels are 0..2 (control.h). Anything else is a typo, and a typo must
        // not quietly mean "nobody" or "everybody".
        if (n < 0 || n > 2) return fail("level out of range (0..2)");
        hasLevel = true;
        level = static_cast<int>(n);
    }

    Zone z;
    z.name = f[0];
    z.x0 = x0; z.y0 = y0; z.z0 = z0;
    z.x1 = x1; z.y1 = y1; z.z1 = z1;
    z.enforced = enforced;
    z.hasLevel = hasLevel;
    z.level = level;
    out = std::move(z);
    return true;
}

inline std::string zone_serialize_line(const Zone& z) {
    std::string out = z.name + ":" +
        std::to_string(z.x0) + ":" + std::to_string(z.y0) + ":" + std::to_string(z.z0) + ":" +
        std::to_string(z.x1) + ":" + std::to_string(z.y1) + ":" + std::to_string(z.z1) + ":" +
        (z.enforced ? "all" : "off");
    if (z.hasLevel) out += ":" + std::to_string(z.level);
    return out;
}

/// The set of all protected zones for one world. Load rejects the whole file
/// (leaving the set unchanged) on any malformed line, duplicate name, or a
/// 257th zone — a corrupt/edited file must never come up half-protected.
class ZoneSet {
public:
    void clear() { zones_.clear(); }

    const std::vector<Zone>& zones() const { return zones_; }
    size_t size() const { return zones_.size(); }

    const Zone* find(const std::string& name) const {
        for (const auto& z : zones_)
            if (z.name == name) return &z;
        return nullptr;
    }

    /// Add one zone. Returns false (set unchanged) on a duplicate name or the
    /// ZONE_MAX cap.
    bool add(const Zone& z, std::string* err = nullptr) {
        auto fail = [&](const char* why) { if (err) *err = why; return false; };
        if (find(z.name)) return fail("duplicate name");
        if (zones_.size() >= ZONE_MAX) return fail("zone cap reached");
        zones_.push_back(z);
        return true;
    }

    bool remove(const std::string& name) {
        for (auto it = zones_.begin(); it != zones_.end(); ++it)
            if (it->name == name) { zones_.erase(it); return true; }
        return false;
    }

    /// First enforcing (flag "all", not "off") zone containing the cell that
    /// `level` does not bypass, or null. `level` is the session's zone-bypass
    /// level (auth.h `auth_zone_bypass_level`): -1, the default, bypasses nothing.
    const Zone* blocking(int64_t x, int64_t y, int64_t z, int level = -1) const {
        for (const auto& z_ : zones_)
            if (z_.enforced && z_.contains(x, y, z) && !z_.bypassedBy(level)) return &z_;
        return nullptr;
    }

    /// True if any enforcing zone names a bypass level. When none does, a caller
    /// need not work out who is asking at all — nobody bypasses anything.
    bool hasBypassLevels() const {
        for (const auto& z_ : zones_)
            if (z_.enforced && z_.hasLevel) return true;
        return false;
    }

    /// True if any enforcing zone's box overlaps the given box at all. Lets a
    /// caller (explosion / WorldEdit batch) skip a per-cell scan when its box
    /// touches no zone.
    bool intersects(int64_t x0, int64_t y0, int64_t z0,
                     int64_t x1, int64_t y1, int64_t z1) const {
        int64_t bx0 = std::min(x0, x1), bx1 = std::max(x0, x1);
        int64_t by0 = std::min(y0, y1), by1 = std::max(y0, y1);
        int64_t bz0 = std::min(z0, z1), bz1 = std::max(z0, z1);
        for (const auto& z_ : zones_)
            if (z_.enforced && z_.overlaps(bx0, by0, bz0, bx1, by1, bz1)) return true;
        return false;
    }

    /// Parse a whole `eden_zones.txt` stream. `#`-comments and blank lines are
    /// skipped. On the first malformed line, duplicate name, or cap overflow,
    /// `out` is left untouched, `errLine` (1-based) and `err` describe the
    /// failure, and the function returns false.
    static bool load(std::istream& in, ZoneSet& out, std::string* err = nullptr, int* errLine = nullptr) {
        ZoneSet built;
        std::string line;
        int lineNo = 0;
        while (std::getline(in, line)) {
            ++lineNo;
            size_t b = 0, e = line.size();
            while (e > b && (line[e - 1] == '\r' || line[e - 1] == ' ' || line[e - 1] == '\t')) --e;
            while (b < e && (line[b] == ' ' || line[b] == '\t')) ++b;
            if (b >= e || line[b] == '#') continue;
            Zone z;
            std::string why;
            if (!zone_parse_line(line.substr(b, e - b), z, &why)) {
                if (err) *err = why;
                if (errLine) *errLine = lineNo;
                return false;
            }
            std::string addWhy;
            if (!built.add(z, &addWhy)) {
                if (err) *err = addWhy;
                if (errLine) *errLine = lineNo;
                return false;
            }
        }
        out = std::move(built);
        return true;
    }

    void serialize(std::ostream& out) const {
        out << "# eden_zones.txt — protected boxes (ROADMAP-SERVER Phase 8).\n";
        out << "# name:x0:y0:z0:x1:y1:z1:flags[:level]\n";
        for (const auto& z : zones_) out << zone_serialize_line(z) << "\n";
    }

    std::string serialize() const {
        std::ostringstream ss;
        serialize(ss);
        return ss.str();
    }

private:
    std::vector<Zone> zones_;
};

/// Atomically write `set` to `path` (durable_write.h — stage 7.29 convention).
inline bool zone_save_file(const std::string& path, const ZoneSet& set, std::string& err) {
    return write_file_durable(path, set.serialize(), err);
}

}  // namespace ewb
