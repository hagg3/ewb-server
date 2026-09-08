// region_query.h — the `REGION` → `SNAPZ` spatial query: reply geometry, the
// `Cell` → record encoding table, record ordering and frame splitting.
//
// ROADMAP-SERVER stage 1.1. Everything here is pure and header-only so it can be
// unit-tested offline (`region_test.cpp`) without a socket or a world; the server
// itself (`server_posix.cpp`) only supplies the map scan, the lock discipline and
// the rate limiter.
//
// Wire contract, from `eden-server-hosting-plan-2026-09-07.md` §1.1 and
// `TEST WORLDS/CAPTURE-FINDINGS.md`:
//
//   client  REGION:<x>:<z>\n        an integer *point* in absolute server coords
//   server  SNAPZ:<count>:<b64>\n   a burst; see snapz_codec.h for the framing
//
// The reply covers a **chunk-aligned box** around the point:
//
//   x0 = floor((cx - R)/16)*16   x1 = floor((cx + R)/16)*16 + 15
//   z0 = floor((cz - R)/16)*16   z1 = floor((cz + R)/16)*16 + 15
//
// with R = REGION_RADIUS = 224.
//
// ⚠️ R = 224 is **derived, not measured** — it comes from fitting the Pass-2
// capture's observed span (`REGION:65539:65540` → x 65312–65691, z 65312–65775)
// against that formula: x0, z0 and z1 land exactly on the prediction, and the
// fourth edge is content-bounded. It is the same number VuencLink hardcodes as
// `net::region::REGION_RADIUS`, whose `REGION_STRIDE = 2*R` lattice is built to
// tile this box without gaps — so our server and our client agree by construction.
// `region_box()` takes the radius as a defaulted argument precisely so hosting our
// own server can vary it experimentally (`--region-radius`), which is the only
// place that number can be measured without burning someone else's CPU.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "snapz_codec.h"

namespace ewb {

// --- geometry ----------------------------------------------------------------

/// Blocks a single reply covers around the request point. See the ⚠️ above.
constexpr int REGION_RADIUS = 224;

/// The world's chunk edge, in blocks — the box is aligned to this.
constexpr int REGION_CHUNK = 16;

/// `ACTION` already rejects x/z outside this; an unvalidated `REGION` point would
/// produce a box that wraps the 24-bit key space.
constexpr int REGION_COORD_MAX = 0xFFFFFF;

/// Records per full `SNAPZ` frame. The capture shows a flat 3000 for every frame
/// and a short final one (170 frames, last = 1671) — the split is **not** aligned
/// to world chunks.
constexpr size_t SNAPZ_FRAME_RECORDS = 3000;

// --- wire record vocabulary --------------------------------------------------

constexpr int32_t SNAP_FLAG_SOLID = 0;  ///< field 5 = block type
constexpr int32_t SNAP_FLAG_AIR   = 1;  ///< field 5 = -1
constexpr int32_t SNAP_FLAG_PAINT = 3;  ///< field 5 = paint colour index (0..54)
constexpr int32_t SNAP_AIR_TYPE   = -1;

// --- server-model cell sentinels (mirror server_posix.cpp) -------------------

constexpr unsigned char CELL_AIR          = 0;
constexpr unsigned char CELL_PAINTED_BASE = 255;

/// Highest legal paint index. The capture's every `flag == 3` field-5 value fell in
/// `0..54` — a `PAINT_RGB` index (the game's palette is 55 entries, 0 = the white
/// sentinel), never a block id. Nothing validates the `extra` of an incoming
/// `ACTION:...:3:<extra>` today, so a hostile or buggy client can park an arbitrary
/// byte in `Cell::color`; `emit_cell_records` refuses to put one on the wire.
constexpr unsigned char CELL_MAX_PAINT = 54;

/// floor-to-chunk that is correct for negative coordinates too (`cx - R` can go
/// below 0 near the origin even though the request point itself cannot).
inline int snap_chunk(int v) {
    int q = v / REGION_CHUNK;
    if (v < 0 && v % REGION_CHUNK != 0) --q;
    return q * REGION_CHUNK;
}

struct RegionBox {
    int x0, x1, z0, z1;
    bool contains(int x, int z) const { return x >= x0 && x <= x1 && z >= z0 && z <= z1; }
};

inline RegionBox region_box(int cx, int cz, int radius = REGION_RADIUS) {
    return {snap_chunk(cx - radius), snap_chunk(cx + radius) + REGION_CHUNK - 1,
            snap_chunk(cz - radius), snap_chunk(cz + radius) + REGION_CHUNK - 1};
}

inline bool region_point_valid(int x, int z) {
    return x >= 0 && x <= REGION_COORD_MAX && z >= 0 && z <= REGION_COORD_MAX;
}

// --- Cell -> record(s) -------------------------------------------------------

/// The plan's §1.1 encoding table, in one place:
///
/// | cell state                     | record(s)                                    |
/// |--------------------------------|----------------------------------------------|
/// | `type == 0` (air)              | `flag 1, -1`                                 |
/// | `type == 255` (painted base)   | `flag 3, color`                              |
/// | `type` 1..254, `color == 0`    | `flag 0, type`                               |
/// | `type` 1..254, `color != 0`    | `flag 0, type` **and** `flag 3, color`       |
///
/// ⚠️ The `255` painted-base sentinel is **internal and must never reach the
/// wire**: on the wire air is `-1` (not `0`, not `255`) and a painted base block
/// is a standalone `flag 3` record. The capture confirms both halves — 2670 cells
/// carried both a `(0, type)` and a `(3, colour)` record, ~4k carried only
/// `(3, colour)`, and every `flag == 3` field-5 value was a `PAINT_RGB` index in
/// `0..54`, never a block id.
///
/// ⚠️ A colour above `CELL_MAX_PAINT` is **dropped, not clamped**. `ACTION` mode 3
/// does not validate its `extra`, so `Cell::color` can hold any byte — and a colour
/// of exactly 255 would put the painted-base sentinel's own value in field 5, which
/// is the one thing this encoding is built to keep off the wire. Dropping a bogus
/// paint leaves the cell's *type* record intact (a solid block just renders
/// unpainted) and drops a painted-base cell entirely, since without a valid colour
/// it describes no edit at all. Validating at ingest instead is stage 1.7's job;
/// this is the defence-in-depth half, in the one place that writes the wire.
inline void emit_cell_records(int x, int y, int z, unsigned char type, unsigned char color,
                              std::vector<SnapRec>& out) {
    const bool paintable = (color != 0 && color <= CELL_MAX_PAINT);
    if (type == CELL_AIR) {
        out.push_back({x, y, z, SNAP_FLAG_AIR, SNAP_AIR_TYPE});
        return;
    }
    if (type == CELL_PAINTED_BASE) {
        if (paintable) out.push_back({x, y, z, SNAP_FLAG_PAINT, static_cast<int32_t>(color)});
        return;
    }
    out.push_back({x, y, z, SNAP_FLAG_SOLID, static_cast<int32_t>(type)});
    if (paintable) out.push_back({x, y, z, SNAP_FLAG_PAINT, static_cast<int32_t>(color)});
}

// --- ordering ----------------------------------------------------------------

/// Sort by `(z, x, y, flag)` before deflating. `g_world` is an `unordered_map`, so
/// unsorted records arrive in hash order — near-random leading bytes. Sorted ones
/// are near-monotonic and compress materially better (the capture's own ratio was
/// ~5.8×). Record order is **not semantically significant**: VuencLink's
/// `ingest_snapz` merges `flag 0` / `flag 3` in either order, and the real client
/// must too, since the capture shows both orders occurring. `flag` is the last key
/// only to make the output deterministic.
inline void sort_records(std::vector<SnapRec>& v) {
    std::sort(v.begin(), v.end(), [](const SnapRec& a, const SnapRec& b) {
        if (a.z != b.z) return a.z < b.z;
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        return a.flag < b.flag;
    });
}

/// How many `SNAPZ` lines `n` records become.
inline size_t snapz_frame_count(size_t n) {
    return (n + SNAPZ_FRAME_RECORDS - 1) / SNAPZ_FRAME_RECORDS;
}

}  // namespace ewb
