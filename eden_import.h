// eden_import.h — the `.eden` → server-world conversion core.
//
// ROADMAP-SERVER stage 5.1 (Phase 5 — `.eden` world import). Pure and
// header-only: an `EdenWorld` (from `eden_file.h`) plus options in, emitted
// cells / sign lines / projections out. No file I/O, no argument parsing, no
// terminal output — that is `eden_import.cpp`'s job. Offline suite:
// `eden_import_test.cpp`, wired into `build_server.sh`.
//
// What this file is responsible for, and why each part exists:
//
//   * **The axis rename.** `.eden` stores a plane in (x, y) and height in z;
//     the server stores a plane in (x, z) and height in y. Blocks and signs get
//     the same rename and nothing else — both formats already centre the plane
//     on 65536 (`docs/protocol.md` § Coordinate model).
//
//   * **The base profile.** The client synthesizes a fixed flat terrain for
//     ground it has never been told about — bedrock at 0, stone 1..15, dirt
//     16..31, grass at 32, air above — and a `.eden` stores that same profile
//     verbatim for untouched columns. Emitting only what *differs* from it is
//     what makes a faithful import (caves and all) cost ~1 % of a full voxel
//     dump. The profile is **data**, not a constant, so a surprise from a live
//     client is a `--base-profile` fix rather than a code change.
//
//   * **Two budgets, not one.** Total cells decide whether the server will hold
//     the world (`SV_MAX_WORLD_CELLS`). Worst-case *records* inside a single
//     `REGION` box decide whether it will play: `handleRegion` answers every
//     request with every in-box cell, so a dense import turns a 750 ms-cadence
//     request into a multi-megabyte reply. The second number is the one that
//     bites, and neither is visible after the fact — so both are projected
//     before anything is written.
//
// Header-only, C++17. Pure: no filesystem, no terminal, nothing to mock.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>     // snprintf, for the spawn line's fixed precision
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "eden_file.h"
#include "hardening.h"      // sanitize_text, MAX_BLOCK_TYPE, MAX_PAINT_INDEX
#include "region_query.h"   // region_box, CELL_* sentinels, REGION_RADIUS
#include "sign_store.h"     // Sign, SIGN_COORD_MAX, SIGN_Y_MAX, SIGN_TEXT_MAX
#include "spawn_store.h"    // Spawn, format_spawn_line (eden_spawn.txt grammar)

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

// ── options ─────────────────────────────────────────────────────────────────

/// Which voxels of a saved chunk become cells. One emitter, three presets.
enum class AirFill {
    Diff,    ///< emit iff (type, paint) differs from the base profile — default
    Solid,   ///< emit solids only; sub-surface voids fill back in
    Full,    ///< emit every voxel; byte-faithful, needs no profile hypothesis
};

inline const char* eden_air_fill_name(AirFill f) {
    return f == AirFill::Diff ? "diff" : f == AirFill::Solid ? "solid" : "full";
}
inline bool eden_parse_air_fill(const std::string& s, AirFill& out) {
    if (s == "diff")  { out = AirFill::Diff;  return true; }
    if (s == "solid") { out = AirFill::Solid; return true; }
    if (s == "full")  { out = AirFill::Full;  return true; }
    return false;
}

/// `SV_MAX_WORLD_CELLS` as `server_posix.cpp` compiles it. Stage 5.3 makes the
/// server's copy a runtime flag; until then this is also the effective ceiling.
inline constexpr size_t EDEN_DEFAULT_MAX_WORLD_CELLS = 4'000'000;

/// The largest `REGION` reply ever captured from the real Eden server: ~507 k
/// records, ~3 MB on the wire, ~395 ms to sort, deflate and send. Not a limit —
/// a reference point, and the only empirical one there is.
inline constexpr size_t EDEN_REGION_RECORDS_OBSERVED = 507'000;

/// Default refusal ceiling for worst-case region records: ~4x the largest reply
/// ever observed. Past this an import is not "big", it is a different shape of
/// server than any client has been seen to cope with, on a path clients re-hit
/// every 750 ms. Override with `--max-region-records` (0 disables the check).
inline constexpr size_t EDEN_DEFAULT_MAX_REGION_RECORDS = 2'000'000;

struct ImportOptions {
    AirFill     air_fill = AirFill::Diff;
    BaseProfile profile  = eden_default_profile();
    size_t      max_world_cells    = EDEN_DEFAULT_MAX_WORLD_CELLS;
    size_t      max_region_records = EDEN_DEFAULT_MAX_REGION_RECORDS;
    int         region_radius      = REGION_RADIUS;
    bool        strict = false;   ///< promote the block/paint warnings to errors
};

// ── the emitter ─────────────────────────────────────────────────────────────

/// Chunks in a deterministic order, so two runs of the tool over one file
/// produce byte-identical output (the golden test depends on this).
inline std::vector<const EdenChunk*> eden_sorted_chunks(const EdenWorld& w) {
    std::vector<const EdenChunk*> v;
    v.reserve(w.chunks.size());
    for (const EdenChunk& c : w.chunks) v.push_back(&c);
    std::sort(v.begin(), v.end(), [](const EdenChunk* a, const EdenChunk* b) {
        return a->cx != b->cx ? a->cx < b->cx : a->cy < b->cy;
    });
    return v;
}

/// Records a cell becomes in a `REGION` reply — the table `emit_cell_records`
/// implements, counted rather than materialised. A painted solid costs two.
inline int eden_record_cost(uint8_t type, uint8_t paint) {
    const bool paintable = (paint != 0 && paint <= CELL_MAX_PAINT);
    if (type == CELL_AIR) return 1;                       // flag 1, -1
    if (type == CELL_PAINTED_BASE) return paintable ? 1 : 0;
    return paintable ? 2 : 1;                             // flag 0 [+ flag 3]
}

/// The one selection rule, shared by the projection pass and the write pass so
/// they can never disagree about what a world contains.
inline bool eden_keep_voxel(const ImportOptions& o, int z, uint8_t type, uint8_t paint) {
    switch (o.air_fill) {
        case AirFill::Full:  return true;
        case AirFill::Solid: return type != CELL_AIR;
        case AirFill::Diff:  break;
    }
    const BaseVoxel b = o.profile.at(z);
    return !(type == b.type && paint == b.paint);
}

/// Walk one chunk in emission order, calling `fn(x, y, z, type, paint)` with
/// **server** coordinates for every voxel the strategy keeps.
template <class Fn>
inline void eden_scan_chunk(const EdenWorld& w, const EdenChunk& c,
                            const ImportOptions& o, Fn&& fn) {
    const uint8_t* b = w.data();
    for (int lx = 0; lx < 16; ++lx)
        for (int ly = 0; ly < 16; ++ly)
            for (int z = 0; z <= w.z_ceiling; ++z) {
                const uint8_t type  = eden_block(b, c, lx, ly, z);
                const uint8_t paint = eden_paint(b, c, lx, ly, z);
                if (!eden_keep_voxel(o, z, type, paint)) continue;
                // The axis rename, in the one place it happens for blocks.
                fn(c.cx * 16 + lx, z, c.cy * 16 + ly, type, paint);
            }
}

/// Whole world, chunks in `eden_sorted_chunks` order.
template <class Fn>
inline void eden_scan(const EdenWorld& w, const ImportOptions& o, Fn&& fn) {
    for (const EdenChunk* c : eden_sorted_chunks(w)) eden_scan_chunk(w, *c, o, fn);
}

// ── projection ──────────────────────────────────────────────────────────────

struct RegionHotspot {
    size_t records = 0;   ///< records a single REGION reply would carry
    int    x0 = 0, x1 = 0, z0 = 0, z1 = 0;   ///< the box, in server coords
};

struct ImportProjection {
    size_t cells   = 0;         ///< lines in eden_world.model
    size_t records = 0;         ///< records if the whole world were one reply
    bool   empty   = true;
    int    x0 = 0, x1 = 0, y0 = 0, y1 = 0, z0 = 0, z1 = 0;   ///< bbox, server coords

    RegionHotspot worst_region;

    size_t bad_type_cells   = 0;   ///< type > MAX_BLOCK_TYPE (warn: client may know it)
    size_t bad_paint_cells  = 0;   ///< paint > CELL_MAX_PAINT (warn: dropped on the wire)
    size_t sentinel_cells   = 0;   ///< type == 255 (hard error: collides with the sentinel)
    int    sentinel_at[3]   = {0, 0, 0};   ///< first offender, server coords
};

/// Largest record count any single `REGION` reply could carry.
///
/// The reply box is chunk-aligned and exactly `W` chunks on a side, so this is a
/// max-sum sliding window over the chunk grid. An optimal window can always be
/// shifted until its low edge sits on a populated chunk row (shifting that far
/// drops nothing and may gain), so only populated coordinates are candidates —
/// which keeps this exact without materialising a grid over a footprint that
/// may legitimately span the whole 32768-chunk coordinate space.
inline RegionHotspot eden_worst_region(std::vector<std::pair<std::pair<int, int>, size_t>> per_chunk,
                                       int region_radius = REGION_RADIUS) {
    RegionHotspot best;
    if (per_chunk.empty()) return best;

    const RegionBox probe = region_box(0, 0, region_radius);
    const int W = (probe.x1 - probe.x0 + 1) / REGION_CHUNK;   // chunks per side

    // Sorted by cy: the inner sweep walks it in one pass per x candidate.
    std::sort(per_chunk.begin(), per_chunk.end(),
              [](const auto& a, const auto& b) { return a.first.second < b.first.second; });

    std::vector<int> xs;
    xs.reserve(per_chunk.size());
    for (const auto& e : per_chunk) xs.push_back(e.first.first);
    std::sort(xs.begin(), xs.end());
    xs.erase(std::unique(xs.begin(), xs.end()), xs.end());

    std::vector<std::pair<int, size_t>> col;   // (cy, records) inside the x window
    for (int x_start : xs) {
        const int x_end = x_start + W - 1;
        col.clear();
        for (const auto& e : per_chunk)
            if (e.first.first >= x_start && e.first.first <= x_end)
                col.push_back({e.first.second, e.second});   // already cy-ordered
        size_t sum = 0;
        for (size_t lo = 0, hi = 0; lo < col.size(); ++lo) {
            if (hi < lo) { hi = lo; sum = 0; }
            const int y_end = col[lo].first + W - 1;
            while (hi < col.size() && col[hi].first <= y_end) sum += col[hi++].second;
            if (sum > best.records) {
                best.records = sum;
                best.x0 = x_start * REGION_CHUNK;
                best.x1 = (x_end + 1) * REGION_CHUNK - 1;
                best.z0 = col[lo].first * REGION_CHUNK;
                best.z1 = (col[lo].first + W) * REGION_CHUNK - 1;
            }
            sum -= col[lo].second;
        }
    }
    return best;
}

/// Pass 1: count everything, materialise nothing. Every number an operator is
/// asked to approve comes from here, before a byte is written.
inline ImportProjection eden_project(const EdenWorld& w, const ImportOptions& o) {
    ImportProjection p;
    std::vector<std::pair<std::pair<int, int>, size_t>> per_chunk;
    per_chunk.reserve(w.chunks.size());

    for (const EdenChunk* c : eden_sorted_chunks(w)) {
        size_t chunk_records = 0;
        eden_scan_chunk(w, *c, o, [&](int x, int y, int z, uint8_t type, uint8_t paint) {
            ++p.cells;
            const int cost = eden_record_cost(type, paint);
            p.records      += size_t(cost);
            chunk_records  += size_t(cost);

            if (type == CELL_PAINTED_BASE) {
                if (p.sentinel_cells == 0) { p.sentinel_at[0] = x; p.sentinel_at[1] = y; p.sentinel_at[2] = z; }
                ++p.sentinel_cells;
            } else if (type > MAX_BLOCK_TYPE) {
                ++p.bad_type_cells;
            }
            if (paint > CELL_MAX_PAINT) ++p.bad_paint_cells;

            if (p.empty) {
                p.empty = false;
                p.x0 = p.x1 = x; p.y0 = p.y1 = y; p.z0 = p.z1 = z;
            } else {
                p.x0 = std::min(p.x0, x); p.x1 = std::max(p.x1, x);
                p.y0 = std::min(p.y0, y); p.y1 = std::max(p.y1, y);
                p.z0 = std::min(p.z0, z); p.z1 = std::max(p.z1, z);
            }
        });
        if (chunk_records) per_chunk.push_back({{c->cx, c->cy}, chunk_records});
    }
    p.worst_region = eden_worst_region(std::move(per_chunk), o.region_radius);
    return p;
}

// ── output formatting ───────────────────────────────────────────────────────

/// One `eden_world.model` line, exactly the grammar `loadWorld()` parses.
inline void eden_append_model_line(std::string& out, int x, int y, int z,
                                   uint8_t type, uint8_t paint) {
    out += std::to_string(x); out += ':';
    out += std::to_string(y); out += ':';
    out += std::to_string(z); out += ':';
    out += std::to_string(int(type)); out += ':';
    out += std::to_string(int(paint)); out += '\n';
}

/// The whole model file, as one string. At the 4 M-cell cap that is roughly
/// 110 MB — fine for a one-shot offline tool, and it buys the atomic
/// temp+rename write the server's own saves use. Use `eden_scan` directly if
/// you ever need this streamed.
inline std::string eden_build_model(const EdenWorld& w, const ImportOptions& o) {
    std::string out;
    eden_scan(w, o, [&](int x, int y, int z, uint8_t type, uint8_t paint) {
        eden_append_model_line(out, x, y, z, type, paint);
    });
    return out;
}

/// A `.eden` sign record in server coordinates, or false if it cannot be one.
/// The rename is the same as for blocks — the file's `z` is already an absolute
/// height, so there is no origin offset to apply on any axis.
inline bool eden_sign_to_server(const EdenSign& s, Sign& out) {
    const int x = s.x, y = s.z, z = s.y;
    if (x < 0 || x > SIGN_COORD_MAX) return false;
    if (z < 0 || z > SIGN_COORD_MAX) return false;
    if (y < 0 || y > SIGN_Y_MAX)     return false;
    out.x = x; out.y = y; out.z = z;
    // a/b/c are unknown fields; `sign_store.h` insists they pass through
    // verbatim, and inventing semantics here would be the one way to break that.
    out.a = s.a; out.b = s.b; out.c = s.c;
    out.text = sanitize_text(s.text, SIGN_TEXT_MAX);
    return true;
}

/// One `eden_signs.txt` line — `x:y:z:a:b:c:text`, text last so a `:` inside it
/// survives (`sign_store.h`'s `parse_sign_line` splits only the first six).
inline std::string eden_format_sign_line(const Sign& s) {
    return std::to_string(s.x) + ":" + std::to_string(s.y) + ":" + std::to_string(s.z) + ":" +
           std::to_string(s.a) + ":" + std::to_string(s.b) + ":" + std::to_string(s.c) + ":" +
           sanitize_text(s.text, SIGN_TEXT_MAX) + "\n";
}

/// Convert a whole sign list, dropping (and counting) any record that cannot be
/// expressed in server coordinates.
inline std::vector<Sign> eden_convert_signs(const std::vector<EdenSign>& in, size_t& dropped) {
    std::vector<Sign> out;
    dropped = 0;
    out.reserve(in.size());
    for (const EdenSign& s : in) {
        Sign t;
        if (eden_sign_to_server(s, t)) out.push_back(std::move(t));
        else ++dropped;
    }
    return out;
}

inline std::string eden_build_signs(const std::vector<Sign>& signs) {
    std::string out;
    for (const Sign& s : signs) out += eden_format_sign_line(s);
    return out;
}

// ── spawn ───────────────────────────────────────────────────────────────────

// `Spawn` and the `eden_spawn.txt` line grammar live in `spawn_store.h`, shared
// verbatim with the server that reads the file back (stage 5.3). This alias keeps
// the CLI call sites reading `eden_format_spawn`.
inline std::string eden_format_spawn(const Spawn& s) { return format_spawn_line(s); }

/// The header's `pos` is already in server order — `(x_plane, height, y_plane)`
/// — so unlike blocks and signs it needs no rename. `pos` is the default source
/// and `home` is not: both measured specimens' `pos.y` was a walkable standing
/// height while `home.y` was not.
inline Spawn eden_spawn_from(const EdenHeader& h, bool use_home) {
    const float* p = use_home ? h.home : h.pos;
    return {p[0], p[1], p[2]};
}

inline bool eden_spawn_in_range(const Spawn& s) {
    return s.x >= 0 && s.x <= float(SIGN_COORD_MAX) &&
           s.z >= 0 && s.z <= float(SIGN_COORD_MAX) &&
           s.y >= 0 && s.y <= float(SIGN_Y_MAX);
}

/// The `.gitignore` every output directory gets. `worlds/` is tracked in this
/// repository, so without it an imported personal world is one `git add -A`
/// away from being published (`docs/import.md`).
inline const char* eden_output_gitignore() {
    return "# Written by eden_import. worlds/ is tracked in this repository, so an\n"
           "# imported world would otherwise be committed by `git add -A`.\n"
           "*\n"
           "!.gitignore\n";
}

// ── names ───────────────────────────────────────────────────────────────────

/// Turn a world name (or an input filename) into a directory-safe slug.
inline std::string eden_slug(const std::string& in) {
    std::string out;
    for (char ch : in) {
        const unsigned char u = (unsigned char)ch;
        if ((u >= 'a' && u <= 'z') || (u >= '0' && u <= '9')) out += char(u);
        else if (u >= 'A' && u <= 'Z') out += char(u - 'A' + 'a');
        else if (!out.empty() && out.back() != '-') out += '-';
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    if (out.empty()) out = "world";
    if (out.size() > 64) out.resize(64);
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}

}  // namespace ewb
