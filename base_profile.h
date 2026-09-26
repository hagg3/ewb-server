// base_profile.h — the terrain a client draws in a cell the server has never
// stored: height-indexed, the same in every column.
//
// Split out of `eden_import.h` (ROADMAP-SERVER 3.7) so the server can use it
// without pulling in the `.eden` parser. The two users read one table:
//
//   * `eden_import` / `eden_export` diff a saved world against it (`--base-profile`);
//   * `server_posix.cpp`'s WorldEdit path reads it as "what an untouched cell
//     looks like", so a `//set 0` into open sky stores nothing, `//paint` skips
//     natural air, and `//undo` puts natural ground back rather than air.
//
// Pure and header-only; covered by `eden_import_test.cpp` (the profile, the
// grammar) and `worldedit_test.cpp` (the WorldEdit reading of it).

#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace ewb {

// ── the base terrain profile ────────────────────────────────────────────────

struct BaseVoxel {
    uint8_t type = 0, paint = 0;
    bool operator==(const BaseVoxel& o) const { return type == o.type && paint == o.paint; }
};

/// Height-indexed terrain the client draws for itself. Anything above the last
/// listed layer is air, so a 64z and a 256z world share one table.
struct BaseProfile {
    std::vector<BaseVoxel> layers;
    BaseVoxel at(int z) const {
        return (z >= 0 && size_t(z) < layers.size()) ? layers[size_t(z)] : BaseVoxel{};
    }
};

/// Block ids of the default profile, named for the summary and the docs.
enum : uint8_t { EDEN_BEDROCK = 1, EDEN_STONE = 2, EDEN_DIRT = 3, EDEN_GRASS = 8 };

/// The measured default: bedrock @0, stone 1..15, dirt 16..31, grass @32, air
/// above — identical in both height formats. See `docs/import.md`.
inline BaseProfile eden_default_profile() {
    BaseProfile p;
    p.layers.resize(33);
    p.layers[0] = {EDEN_BEDROCK, 0};
    for (int z = 1; z <= 15; ++z) p.layers[size_t(z)] = {EDEN_STONE, 0};
    for (int z = 16; z <= 31; ++z) p.layers[size_t(z)] = {EDEN_DIRT, 0};
    p.layers[32] = {EDEN_GRASS, 0};
    return p;
}

/// `--base-profile none`: nothing is assumed about the client's terrain, so
/// every non-air voxel differs from it.
inline BaseProfile eden_empty_profile() { return BaseProfile{}; }

/// Parse a profile file. One layer per line, blanks and `#` comments skipped:
///
///     0:1          # z 0        -> type 1, paint 0
///     1-15:2       # z 1..15    -> type 2
///     32:8:0       # explicit paint
///
/// Heights not mentioned are air. Returns false with `err` set on a bad line.
inline bool eden_parse_profile(const std::string& text, BaseProfile& out, std::string& err) {
    out.layers.clear();
    size_t pos = 0;
    int lineno = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
        ++lineno;

        size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
            line.pop_back();
        size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        line = line.substr(b);

        auto fail = [&](const char* why) {
            err = "line " + std::to_string(lineno) + ": " + why;
            return false;
        };

        std::vector<std::string> f;
        size_t start = 0;
        for (;;) {
            size_t c = line.find(':', start);
            f.push_back(line.substr(start, c == std::string::npos ? std::string::npos : c - start));
            if (c == std::string::npos) break;
            start = c + 1;
        }
        if (f.size() < 2 || f.size() > 3) return fail("expected z[-z]:type[:paint]");

        long z0 = 0, z1 = 0;
        size_t dash = f[0].find('-');
        char* endp = nullptr;
        if (dash == std::string::npos) {
            z0 = z1 = std::strtol(f[0].c_str(), &endp, 10);
            if (endp != f[0].c_str() + f[0].size() || f[0].empty()) return fail("bad height");
        } else {
            std::string a = f[0].substr(0, dash), b2 = f[0].substr(dash + 1);
            z0 = std::strtol(a.c_str(), &endp, 10);
            if (a.empty() || endp != a.c_str() + a.size()) return fail("bad height range");
            z1 = std::strtol(b2.c_str(), &endp, 10);
            if (b2.empty() || endp != b2.c_str() + b2.size()) return fail("bad height range");
        }
        if (z0 < 0 || z1 < z0 || z1 > 255) return fail("height out of range (0..255)");

        long type = std::strtol(f[1].c_str(), &endp, 10);
        if (f[1].empty() || endp != f[1].c_str() + f[1].size()) return fail("bad type");
        if (type < 0 || type > 255) return fail("type out of range (0..255)");
        long paint = 0;
        if (f.size() == 3) {
            paint = std::strtol(f[2].c_str(), &endp, 10);
            if (f[2].empty() || endp != f[2].c_str() + f[2].size()) return fail("bad paint");
            if (paint < 0 || paint > 255) return fail("paint out of range (0..255)");
        }
        if (out.layers.size() < size_t(z1) + 1) out.layers.resize(size_t(z1) + 1);
        for (long z = z0; z <= z1; ++z)
            out.layers[size_t(z)] = {uint8_t(type), uint8_t(paint)};
    }
    err.clear();
    return true;
}

}  // namespace ewb
