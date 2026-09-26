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
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>     // snprintf, for the spawn line's fixed precision
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "eden_file.h"
#include "hardening.h"      // sanitize_text, MAX_BLOCK_TYPE, MAX_PAINT_INDEX
#include "region_query.h"    // region_box, CELL_* sentinels, REGION_RADIUS
#include "world_store.h"     // CELL_MINED: the other reserved block id (stage 7.6)
#include "sign_store.h"     // Sign, SIGN_COORD_MAX, SIGN_Y_MAX, SIGN_TEXT_MAX
#include "spawn_store.h"    // Spawn, format_spawn_line (eden_spawn.txt grammar)
#include "base_profile.h"   // BaseProfile, eden_default_profile, eden_parse_profile

namespace ewb {

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

/// The largest `REGION` reply ever captured from the real Eden server: ~254 k
/// records, ~0.65 MB on the wire (base64), ~5.07 MB decompressed. Not a limit —
/// a reference point, and the only empirical one there is. (Originally doubled by a
/// capture-logger bug; corrected 2026-09-12.)
inline constexpr size_t EDEN_REGION_RECORDS_OBSERVED = 253'671;

/// Default refusal ceiling for worst-case region records: deliberately ~8x the
/// largest observed (253,671), chosen to be over-provisioned for realistic worlds.
/// The previous constant 2M was correct but under-justified (the capture was 2× too
/// large; the new observed figure is 254k). Past this an import is not "big", it is
/// a different shape of server than any client has been seen to cope with, on a path
/// clients re-hit every 750 ms. Override with `--max-region-records` (0 disables it).
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
    size_t sentinel_cells   = 0;   ///< type 254 or 255 — reserved by the server's cell
                                   ///< encoding (hard error). 255 is the painted-base
                                   ///< sentinel; 254 is world_store.h's CELL_MINED.
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

    // Sorted by (cx, cy) once, up front: every x window is then one contiguous range
    // found by binary search, so the cost is the window's own chunks per candidate x
    // rather than all of `per_chunk` per candidate (stage 7.30 — this used to be
    // O(distinct cx x chunks), quadratic on a wide sparse world).
    std::sort(per_chunk.begin(), per_chunk.end());

    std::vector<int> xs;
    xs.reserve(per_chunk.size());
    for (const auto& e : per_chunk) xs.push_back(e.first.first);
    xs.erase(std::unique(xs.begin(), xs.end()), xs.end());   // already cx-ordered

    const auto by_cx = [](const std::pair<std::pair<int, int>, size_t>& e, int cx) {
        return e.first.first < cx;
    };
    std::vector<std::pair<int, size_t>> col;   // (cy, records) inside the x window
    for (int x_start : xs) {
        const int x_end = x_start + W - 1;
        col.clear();
        const auto first = std::lower_bound(per_chunk.begin(), per_chunk.end(), x_start, by_cx);
        const auto last  = std::lower_bound(first, per_chunk.end(), x_end + 1, by_cx);
        for (auto it = first; it != last; ++it) col.push_back({it->first.second, it->second});
        std::sort(col.begin(), col.end());   // the sweep below wants cy order
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

            // 255 is the painted-base sentinel and 254 is the chunk store's
            // "explicitly mined" sentinel (world_store.h, stage 7.6). Neither can
            // survive a round trip through the server's model, so a world
            // containing one is refused rather than quietly altered.
            if (type >= CELL_MINED) {
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

// ── origin: the header fields the server does not store ─────────────────────

/// `eden_origin.txt` — what a world's source `.eden` header said that the server
/// has nowhere to keep (stage 5.6). `eden_import` writes it beside the world;
/// `eden_export` replays it, so a world that came *from* a `.eden` goes back as
/// that same world rather than as defaults. The server never reads it: it is
/// inert state next to the world, in the same hand-editable spirit as
/// `eden_spawn.txt`.
///
/// Grammar: one `key: value` per line, `#` comments and blank lines skipped,
/// keys case-sensitive, an unknown key ignored (so a newer tool's file still
/// reads). Every key is optional — a hand-written file may carry one line.
///
///     name: Texture Test
///     seed: 0
///     pos: 65548.37:47.92:65607.27
///     home: 65536.00:33.92:65536.00
///     yaw: 1.5707964
///     version: 4
///     height: 64z
///     sky: 14 14 14 14 14 14 14 14 14 14 14 14 14 14 14 14
///     base-profile: default
///
/// Floats are written with `%.9g`, which round-trips an IEEE single exactly —
/// the fidelity this file exists for. Deliberately *not* recorded: the header's
/// content hash (it would be stale the moment the world is edited), the bytes
/// nothing parses, and `dir_offset` (a property of the chunk payload).
struct EdenOrigin {
    std::optional<std::string> name;
    std::optional<int32_t>     seed;
    std::optional<Spawn>       pos;
    std::optional<Spawn>       home;
    std::optional<float>       yaw;
    std::optional<int32_t>     version;
    std::optional<int>         height;          ///< 64 or 256 — the source's z format
    std::optional<std::array<uint8_t, 16>> sky;
    std::optional<std::string> base_profile;    ///< `default`, `none` or `custom`

    bool empty() const {
        return !name && !seed && !pos && !home && !yaw && !version && !height && !sky &&
               !base_profile;
    }
};

inline constexpr const char* EDEN_ORIGIN_FILE = "eden_origin.txt";

/// Snapshot a source header. `base_profile` is what the import diffed against
/// (`default`, `none`, or `custom` for a `--base-profile FILE` — the file's
/// contents are not copied, so export can only warn).
inline EdenOrigin eden_origin_from(const EdenWorld& w, const std::string& base_profile) {
    EdenOrigin o;
    o.name = w.hdr.name;
    o.seed = w.hdr.seed;
    o.pos  = Spawn{w.hdr.pos[0], w.hdr.pos[1], w.hdr.pos[2]};
    o.home = Spawn{w.hdr.home[0], w.hdr.home[1], w.hdr.home[2]};
    o.yaw  = w.hdr.yaw;
    o.version = w.hdr.version;
    o.height  = w.z_ceiling + 1;
    std::array<uint8_t, 16> sky;
    std::memcpy(sky.data(), w.hdr.skycolors, 16);
    o.sky = sky;
    o.base_profile = base_profile;
    return o;
}

namespace origin_detail {
inline std::string fmt_f(float f) {
    char b[48];
    std::snprintf(b, sizeof b, "%.9g", double(f));
    return b;
}
inline std::string fmt_xyz(const Spawn& s) {
    return fmt_f(s.x) + ":" + fmt_f(s.y) + ":" + fmt_f(s.z);
}
inline bool finite3(const Spawn& s) {
    return std::isfinite(s.x) && std::isfinite(s.y) && std::isfinite(s.z);
}
inline bool parse_int(const std::string& v, long long lo, long long hi, long long& out) {
    if (v.empty()) return false;
    char* e = nullptr;
    errno = 0;
    const long long x = std::strtoll(v.c_str(), &e, 10);
    if (errno || e != v.c_str() + v.size() || x < lo || x > hi) return false;
    out = x;
    return true;
}
inline bool parse_float(const std::string& v, float& out) {
    if (v.empty()) return false;
    char* e = nullptr;
    const float f = std::strtof(v.c_str(), &e);
    if (e != v.c_str() + v.size() || !std::isfinite(f)) return false;
    out = f;
    return true;
}
inline bool parse_xyz(const std::string& v, Spawn& out) {
    const size_t a = v.find(':');
    const size_t b = a == std::string::npos ? a : v.find(':', a + 1);
    if (b == std::string::npos || v.find(':', b + 1) != std::string::npos) return false;
    return parse_float(v.substr(0, a), out.x) &&
           parse_float(v.substr(a + 1, b - a - 1), out.y) &&
           parse_float(v.substr(b + 1), out.z);
}
}  // namespace origin_detail

/// The file body. Non-finite floats are left out (the reader would refuse them,
/// and `strtof` would not give the bits back anyway); control characters in the
/// name become spaces so one header can never smuggle in a second line.
inline std::string eden_format_origin(const EdenOrigin& o) {
    using namespace origin_detail;
    std::string out =
        "# Written by eden_import: the source .eden header's fields the server does not\n"
        "# store. eden_export replays them; the server never reads this file. Hand-edit\n"
        "# freely - every key is optional. See docs/export.md.\n";
    if (o.name) {
        std::string n = *o.name;
        for (char& c : n)
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = ' ';
        out += "name: " + n + "\n";
    }
    if (o.seed)    out += "seed: " + std::to_string(*o.seed) + "\n";
    if (o.pos && finite3(*o.pos))   out += "pos: "  + fmt_xyz(*o.pos)  + "\n";
    if (o.home && finite3(*o.home)) out += "home: " + fmt_xyz(*o.home) + "\n";
    if (o.yaw && std::isfinite(*o.yaw)) out += "yaw: " + fmt_f(*o.yaw) + "\n";
    if (o.version) out += "version: " + std::to_string(*o.version) + "\n";
    if (o.height)  out += "height: " + std::to_string(*o.height) + "z\n";
    if (o.sky) {
        out += "sky:";
        for (uint8_t b : *o.sky) out += " " + std::to_string(int(b));
        out += "\n";
    }
    if (o.base_profile) out += "base-profile: " + *o.base_profile + "\n";
    return out;
}

/// Parse an origin file. False with `err` naming the line on the first
/// malformed one — the caller decides whether that is fatal (`eden_export`
/// treats it as a refusal: a half-read header is worse than a loud stop).
/// `unknown` receives the count of keys skipped for not being recognised.
inline bool eden_parse_origin(const std::string& text, EdenOrigin& out, std::string& err,
                              size_t* unknown = nullptr) {
    using namespace origin_detail;
    out = EdenOrigin{};
    err.clear();
    if (unknown) *unknown = 0;
    size_t pos = 0;
    int lineno = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
        ++lineno;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos || line[b] == '#') continue;

        auto fail = [&](const std::string& why) {
            err = "line " + std::to_string(lineno) + ": " + why;
            return false;
        };
        const size_t colon = line.find(':', b);
        if (colon == std::string::npos) return fail("expected `key: value`");
        std::string key = line.substr(b, colon - b);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        std::string val = line.substr(colon + 1);
        // One separating space is not part of the value; anything past that is,
        // so a name keeps its own spacing. Everything but `name` is trimmed.
        if (!val.empty() && val.front() == ' ') val.erase(0, 1);
        std::string trimmed = val;
        {
            const size_t vb = trimmed.find_first_not_of(" \t");
            trimmed = vb == std::string::npos ? "" : trimmed.substr(vb);
            while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t'))
                trimmed.pop_back();
        }

        long long n = 0;
        if (key == "name") {
            out.name = val.size() > 50 ? val.substr(0, 50) : val;
        } else if (key == "seed") {
            if (!parse_int(trimmed, INT32_MIN, INT32_MAX, n)) return fail("seed wants a 32-bit integer");
            out.seed = int32_t(n);
        } else if (key == "version") {
            if (!parse_int(trimmed, INT32_MIN, INT32_MAX, n)) return fail("version wants a 32-bit integer");
            out.version = int32_t(n);
        } else if (key == "yaw") {
            float f;
            if (!parse_float(trimmed, f)) return fail("yaw wants a finite number");
            out.yaw = f;
        } else if (key == "pos" || key == "home") {
            Spawn s;
            if (!parse_xyz(trimmed, s)) return fail(key + " wants x:y:z (three finite numbers)");
            (key == "pos" ? out.pos : out.home) = s;
        } else if (key == "height") {
            if (trimmed == "64z" || trimmed == "64") out.height = 64;
            else if (trimmed == "256z" || trimmed == "256") out.height = 256;
            else return fail("height wants 64z or 256z");
        } else if (key == "sky") {
            std::array<uint8_t, 16> sky{};
            size_t at = 0, count = 0;
            while (at < trimmed.size()) {
                const size_t s0 = trimmed.find_first_not_of(" \t", at);
                if (s0 == std::string::npos) break;
                size_t s1 = trimmed.find_first_of(" \t", s0);
                if (s1 == std::string::npos) s1 = trimmed.size();
                if (count >= 16 || !parse_int(trimmed.substr(s0, s1 - s0), 0, 255, n))
                    return fail("sky wants 16 integers, each 0..255");
                sky[count++] = uint8_t(n);
                at = s1;
            }
            if (count != 16) return fail("sky wants 16 integers, each 0..255");
            out.sky = sky;
        } else if (key == "base-profile") {
            if (trimmed != "default" && trimmed != "none" && trimmed != "custom")
                return fail("base-profile wants default, none or custom");
            out.base_profile = trimmed;
        } else if (unknown) {
            ++*unknown;
        }
    }
    return true;
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
