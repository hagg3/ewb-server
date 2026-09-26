// world_store.h — the chunked world store: the server's block-delta model, its
// `EDMB` binary on-disk format, and the legacy text reader kept for every world
// shipped before it.
//
// ROADMAP-SERVER stage 7.6. Pure and header-only so it can be unit-tested offline
// (`world_store_test.cpp`) without a socket, a world file or a lock; the server
// (`server_posix.cpp`) supplies `g_worldMtx`, the cell cap and the sign hook.
//
// Why it exists — two costs, one cause. The model used to be a flat
// `unordered_map<uint64_t, Cell>`:
//
//   * `saveWorld()` copied the whole map under the world lock and wrote ~19 bytes
//     of text per cell;
//   * `serveRegion()` walked **every cell in the world** under the world lock and
//     filtered by the reply box, per request, up to
//     `SV_MAX_REGIONS_PER_SESSION` times per client per session.
//
// A 16^3 chunk map fixes both: the save serialises ~4 bytes/cell into a blob, and
// a `REGION` visits only the chunks whose x/z footprint intersects the box —
// thousands of chunks instead of millions of cells.
//
// ============================================================================
// ⚠️  THE INVARIANT: three cell states in one byte
// ============================================================================
//
// The sparse map distinguished three states for free:
//
//   absent                 no delta — the client renders its own base terrain
//   present, `type == 0`   explicitly mined — render air even where the base
//                          terrain has a block
//   present, `type != 0`   a placed block (or 255, a painted base block)
//
// A dense `uint8 type[4096]` has no "absent", so the array needs a value for it.
// This store's resolution, which every read and write path below obeys:
//
//   ┌ array value ┬ meaning ──────────────────────────────────────────────────┐
//   │ 0           │ **no delta** — the slot is absent, exactly like a key the │
//   │             │ old map did not contain. Never emitted, never counted in  │
//   │             │ `size()`, never written to disk.                          │
//   │ 254         │ `CELL_MINED` — **explicitly mined**. Reads back as logical│
//   │             │ type 0 (air) from `get()` / `for_each*()`, so every caller│
//   │             │ that tested `cell.type == SV_AIR` keeps working unchanged.│
//   │ 1..253, 255 │ that block type, verbatim (255 = painted base block).     │
//   └─────────────┴───────────────────────────────────────────────────────────┘
//
// So: **the array default means absence, and `CELL_MINED` is the only encoding of
// air.** The sentinel is an in-memory storage detail — it is mapped back to 0 on
// every read, it never reaches `emit_cell_records()`, `eden_import`'s emitters,
// the wire, or the disk format. Nothing outside this header may see a 254.
//
// The one thing that costs: array value 254 is **reserved**, so the store cannot
// hold a block of literal type 254. Nothing in the server can produce one —
// `ACTION` validates types to `ewb::MAX_BLOCK_TYPE` (127) and the painted-base
// sentinel is 255 — but a legacy text world converted from an exotic `.eden`
// could in principle contain one (`eden_import` warns above `MAX_BLOCK_TYPE`, and
// since 7.6 refuses 254 outright the same way it already refused 255). A 254 met
// while loading is therefore **coerced to mined air and counted**
// (`reserved_coerced()`), so an operator sees it instead of it silently becoming
// a different block.
//
// ============================================================================
// EDMB — the binary world format
// ============================================================================
//
//   ["EDMB"][u32 version = 1][u64 chunkCount]
//     per chunk: [i32 cx][i32 cy][i32 cz][u32 nonZero]
//                nonZero x ( [u16 localIdx][u8 type][u8 color] )
//
// All integers little-endian, packed, no alignment. `localIdx` is
// `(ly << 8) | (lz << 4) | lx` within the 16^3 chunk. Chunks are written in
// ascending key order, and slots in ascending `localIdx`, so a save is
// byte-deterministic for a given world.
//
// ⚠️ On disk, `type == 0` means **explicitly mined** (logical air), not "absent":
// only the `nonZero` slots that carry a delta are listed at all, so absence is
// represented by omission and the 254 sentinel never leaves memory. A reader of
// this format needs no knowledge of the sentinel.
//
// `world_load()` sniffs the 4 magic bytes and falls back to the legacy
// `x:y:z:type:color` text reader when they do not match, so every world already
// shipped — `worlds/<name>/eden_world.model`, `testdata/carved_64z.model`,
// anything `eden_import` writes — keeps loading with no migration step.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <istream>
#include <string>
#include <unordered_map>
#include <vector>

namespace ewb {

// --- geometry ----------------------------------------------------------------

constexpr int WS_CHUNK      = 16;                        ///< chunk edge, blocks
constexpr int WS_CHUNK_CELLS = WS_CHUNK * WS_CHUNK * WS_CHUNK;   ///< 4096

/// World height in blocks, mirroring `server_posix.cpp`'s `SV_WORLD_HEIGHT`
/// (tied together by the `static_assert` below it, in that file). This is what
/// lets a box query's Y sweep be a small fixed range instead of unbounded: no
/// valid cell can have `y >= WS_WORLD_HEIGHT`, so no chunk can exist above
/// `cy == WS_MAX_CY`.
constexpr int WS_WORLD_HEIGHT = 256;
constexpr int WS_MAX_CY = (WS_WORLD_HEIGHT - 1) >> 4;   // 15, for a 256-tall world

/// The in-array "explicitly mined" sentinel. See the invariant table above. This
/// value is **never** visible through any accessor on `WorldStore`.
constexpr unsigned char CELL_MINED = 254;

/// Coordinate masks, identical to the packed key the sparse map used
/// (`wkey()` in server_posix.cpp): x and z 24-bit, y 16-bit. Keeping them makes
/// the swap behaviour-preserving for out-of-range or negative input.
constexpr uint32_t WS_XZ_MASK = 0xFFFFFFu;
constexpr uint32_t WS_Y_MASK  = 0xFFFFu;

/// The addressable world: x/z in `[0, 0xFFFFFF]`, y in `[0, WS_WORLD_HEIGHT)` —
/// the same box `ACTION`, `setblock`/`fill`, WorldEdit and the sign parser
/// validate against. It is also exactly the set of coordinates the masking in
/// `key_of` maps one-to-one onto a chunk `for_each_in_box` sweeps:
///   - y >= WS_WORLD_HEIGHT lands in `cy > WS_MAX_CY`, which no box query
///     visits — stored, saved, counted against the cap, never sent, and not
///     addressable by any player to remove (ROADMAP-SERVER 7.22);
///   - y < 0 masks to `cy` 4080..4095, the same;
///   - x/z outside 24 bits wrap onto the opposite edge of the world, i.e. an
///     *existing, reachable* cell some 16 million blocks away.
/// `WorldStore::set` refuses anything outside it (see `out_of_range()`).
inline bool ws_in_world(int x, int y, int z) {
    return x >= 0 && z >= 0 && (uint32_t)x <= WS_XZ_MASK && (uint32_t)z <= WS_XZ_MASK &&
           y >= 0 && y < WS_WORLD_HEIGHT;
}

struct WorldCell {
    unsigned char type = 0;    ///< logical: 0 = air (mined), 255 = painted base
    unsigned char color = 0;
};

/// One 16^3 chunk. `nonZero` is the number of slots that carry a delta — i.e.
/// slots whose array value is not 0 — which is what `size()` sums and what EDMB
/// writes per chunk.
struct WChunk {
    unsigned char type[WS_CHUNK_CELLS];
    unsigned char color[WS_CHUNK_CELLS];
    uint32_t nonZero;
    WChunk() : nonZero(0) { std::memset(type, 0, sizeof type); std::memset(color, 0, sizeof color); }
};

/// Pack a chunk coordinate triple into a map key. cx/cz are 20-bit (24-bit block
/// coords >> 4), cy is 12-bit (16-bit y >> 4).
inline uint64_t ws_chunk_key(int cx, int cy, int cz) {
    return ((uint64_t)(uint32_t)cx << 32) | ((uint64_t)(uint32_t)cz << 12) | (uint64_t)(uint32_t)cy;
}
inline void ws_unkey(uint64_t k, int& cx, int& cy, int& cz) {
    cx = (int)((k >> 32) & 0xFFFFFu);
    cz = (int)((k >> 12) & 0xFFFFFu);
    cy = (int)( k        & 0xFFFu);
}
inline int ws_local_index(int x, int y, int z) {
    return ((y & 15) << 8) | ((z & 15) << 4) | (x & 15);
}

/// Encode a logical type for storage: air becomes the sentinel, everything else
/// is verbatim. Only `WorldStore` calls this.
inline unsigned char ws_encode_type(unsigned char logical) {
    return logical == 0 ? CELL_MINED : logical;
}
/// Decode a stored slot value to a logical type. Only valid for a present slot.
inline unsigned char ws_decode_type(unsigned char stored) {
    return stored == CELL_MINED ? 0 : stored;
}

// --- the store ---------------------------------------------------------------

class WorldStore {
public:
    /// Number of cells carrying a delta — the same number the sparse map's
    /// `size()` returned, and what the `--max-world-cells` cap counts.
    size_t size() const { return cells_; }
    bool empty() const { return cells_ == 0; }
    size_t chunk_count() const { return chunks_.size(); }
    /// Cells whose stored type was the reserved 254 and were loaded as air.
    size_t reserved_coerced() const { return coerced_; }
    /// Writes `set()` refused because the coordinate is outside `ws_in_world`.
    /// Nonzero after a load means the file held cells no client could ever see.
    size_t out_of_range() const { return outOfRange_; }

    void clear() { chunks_.clear(); cells_ = 0; coerced_ = 0; outOfRange_ = 0; }

    /// Resident bytes of chunk payload (the arrays only) — what the denser
    /// storage actually buys, for the cap advice in `docs/configuration.md`.
    size_t payload_bytes() const { return chunks_.size() * sizeof(WChunk); }

    bool contains(int x, int y, int z) const {
        const WChunk* c = find_chunk(x, y, z);
        return c && c->type[ws_local_index(x, y, z)] != 0;
    }

    /// `true` if the cell carries a delta; `out` then holds its **logical**
    /// type/colour (mined reads back as type 0, never 254).
    bool get(int x, int y, int z, WorldCell& out) const {
        const WChunk* c = find_chunk(x, y, z);
        if (!c) return false;
        const int i = ws_local_index(x, y, z);
        const unsigned char t = c->type[i];
        if (t == 0) return false;
        out.type = ws_decode_type(t);
        out.color = c->color[i];
        return true;
    }

    /// Write a logical type/colour. Creates the chunk if needed. A logical type
    /// of `CELL_MINED` (254) cannot be stored — see the header comment — and is
    /// recorded as mined air, counted by `reserved_coerced()`.
    ///
    /// Returns false, stores nothing and counts `out_of_range()` for a
    /// coordinate outside `ws_in_world` — without this, `key_of`'s masking
    /// would alias it into a chunk no box query reaches, or onto another cell.
    bool set(int x, int y, int z, unsigned char type, unsigned char color) {
        if (!ws_in_world(x, y, z)) { ++outOfRange_; return false; }
        if (type == CELL_MINED) { ++coerced_; type = 0; }
        WChunk& c = chunks_[key_of(x, y, z)];
        const int i = ws_local_index(x, y, z);
        if (c.type[i] == 0) { ++c.nonZero; ++cells_; }
        c.type[i] = ws_encode_type(type);
        c.color[i] = color;
        return true;
    }

    /// Visit every present cell as `fn(x, y, z, logicalType, color)`. Order is
    /// unspecified (hash order of the chunk map), as the sparse map's was.
    template <class F>
    void for_each(F&& fn) const {
        for (const auto& kv : chunks_) {
            int cx, cy, cz; ws_unkey(kv.first, cx, cy, cz);
            emit_chunk(kv.second, cx, cy, cz, fn);
        }
    }

    /// What one box scan touched — the numbers `region-stats` reports.
    struct BoxScan {
        size_t chunks_total = 0;    ///< chunks in the world
        size_t chunks_visited = 0;  ///< chunks whose x/z footprint met the box
        size_t cells_visited = 0;   ///< slots examined (16^3 per visited chunk)
        size_t cells_emitted = 0;   ///< present cells inside the box
    };

    /// Visit every present cell whose x/z lies in `[x0,x1] x [z0,z1]`, as
    /// `fn(x, y, z, logicalType, color)`. This is the whole point of the chunk
    /// store: candidate chunk coordinates are derived directly from the box and
    /// probed with `unordered_map::find` (average O(1) each), so cost scales
    /// with the box's footprint — `(x-chunks) * (z-chunks) * (WS_MAX_CY+1)`
    /// lookups — not with the size of the world. (An earlier version of this
    /// function iterated every chunk in the map and bounding-box-tested each
    /// one; that was still O(total chunks) and defeated the point of this
    /// stage on a world whose chunk count dwarfs one box's footprint — fixed
    /// before shipping.) y has no equivalent box bound (`REGION` wants a full
    /// height column), so the sweep uses `WS_MAX_CY` instead.
    template <class F>
    BoxScan for_each_in_box(int x0, int x1, int z0, int z1, F&& fn) const {
        BoxScan st;
        st.chunks_total = chunks_.size();
        const int cx0 = x0 >> 4, cx1 = x1 >> 4;
        const int cz0 = z0 >> 4, cz1 = z1 >> 4;
        for (int cx = cx0; cx <= cx1; ++cx) {
            const int bx0 = cx * WS_CHUNK, bx1 = bx0 + WS_CHUNK - 1;
            for (int cz = cz0; cz <= cz1; ++cz) {
                const int bz0 = cz * WS_CHUNK, bz1 = bz0 + WS_CHUNK - 1;
                const bool whole = bx0 >= x0 && bx1 <= x1 && bz0 >= z0 && bz1 <= z1;
                for (int cy = 0; cy <= WS_MAX_CY; ++cy) {
                    auto it = chunks_.find(ws_chunk_key(cx, cy, cz));
                    if (it == chunks_.end()) continue;
                    ++st.chunks_visited;
                    st.cells_visited += WS_CHUNK_CELLS;
                    const WChunk& c = it->second;
                    for (int i = 0; i < WS_CHUNK_CELLS; ++i) {
                        const unsigned char t = c.type[i];
                        if (t == 0) continue;
                        const int lx = i & 15, lz = (i >> 4) & 15, ly = (i >> 8) & 15;
                        const int x = bx0 + lx, z = bz0 + lz;
                        if (!whole && (x < x0 || x > x1 || z < z0 || z > z1)) continue;
                        ++st.cells_emitted;
                        fn(x, cy * WS_CHUNK + ly, z, ws_decode_type(t), c.color[i]);
                    }
                }
            }
        }
        return st;
    }

    /// Visit every present cell of one x/z column, bottom to top, as
    /// `fn(y, logicalType, color)`. `WS_MAX_CY + 1` chunk probes — the per-column
    /// cost `topmap` (stage 8.5) pays per sample, independent of world size.
    template <class F>
    void for_each_in_column(int x, int z, F&& fn) const {
        if (x < 0 || z < 0 || (uint32_t)x > WS_XZ_MASK || (uint32_t)z > WS_XZ_MASK) return;
        const int cx = x >> 4, cz = z >> 4;
        const int col = ((z & 15) << 4) | (x & 15);
        for (int cy = 0; cy <= WS_MAX_CY; ++cy) {
            auto it = chunks_.find(ws_chunk_key(cx, cy, cz));
            if (it == chunks_.end()) continue;
            const WChunk& c = it->second;
            for (int ly = 0; ly < WS_CHUNK; ++ly) {
                const int i = (ly << 8) | col;
                const unsigned char t = c.type[i];
                if (t == 0) continue;
                fn(cy * WS_CHUNK + ly, ws_decode_type(t), c.color[i]);
            }
        }
    }

    // --- EDMB serialisation --------------------------------------------------

    /// Serialise the whole store to an EDMB blob. Deterministic: chunks ascending
    /// by key, slots ascending by local index.
    std::string to_edmb() const {
        std::vector<uint64_t> keys;
        keys.reserve(chunks_.size());
        for (const auto& kv : chunks_) if (kv.second.nonZero) keys.push_back(kv.first);
        std::sort(keys.begin(), keys.end());

        std::string out;
        out.reserve(16 + keys.size() * 16 + cells_ * 4);
        out.append("EDMB", 4);
        put_u32(out, 1);
        put_u64(out, (uint64_t)keys.size());
        for (uint64_t k : keys) {
            const WChunk& c = chunks_.at(k);
            int cx, cy, cz; ws_unkey(k, cx, cy, cz);
            put_u32(out, (uint32_t)cx);
            put_u32(out, (uint32_t)cy);
            put_u32(out, (uint32_t)cz);
            put_u32(out, c.nonZero);
            for (int i = 0; i < WS_CHUNK_CELLS; ++i) {
                const unsigned char t = c.type[i];
                if (t == 0) continue;
                put_u16(out, (uint16_t)i);
                out.push_back((char)ws_decode_type(t));   // logical on disk
                out.push_back((char)c.color[i]);
            }
        }
        return out;
    }

    static bool is_edmb(const char* p, size_t n) {
        return n >= 4 && p[0] == 'E' && p[1] == 'D' && p[2] == 'M' && p[3] == 'B';
    }

    /// Parse an EDMB blob into this store (additive — call `clear()` first for a
    /// fresh load). Returns false with `err` set on a malformed or truncated
    /// file, having applied whatever it read before the damage.
    ///
    /// A chunk above `WS_MAX_CY` is well-formed EDMB but not a world cell: a
    /// server before 7.22 let a TNT blast near the top of the world write
    /// `y = 256..260` and saved it. Those cells are dropped and counted in
    /// `out_of_range()` (via `set`), so the next save no longer carries them.
    bool load_edmb(const char* p, size_t n, std::string& err) {
        size_t o = 0;
        if (!is_edmb(p, n)) { err = "not an EDMB file"; return false; }
        o = 4;
        uint32_t ver = 0; if (!take_u32(p, n, o, ver)) { err = "truncated header"; return false; }
        if (ver != 1) { err = "unsupported EDMB version " + std::to_string(ver); return false; }
        uint64_t chunkCount = 0;
        if (!take_u64(p, n, o, chunkCount)) { err = "truncated header"; return false; }
        // A bogus count must not make us reserve gigabytes: each chunk is at
        // least 16 bytes on the wire, so this is the hard ceiling.
        if (chunkCount > (uint64_t)(n / 16) + 1) { err = "chunk count exceeds file size"; return false; }
        for (uint64_t ci = 0; ci < chunkCount; ++ci) {
            uint32_t ux = 0, uy = 0, uz = 0, nz = 0;
            if (!take_u32(p, n, o, ux) || !take_u32(p, n, o, uy) ||
                !take_u32(p, n, o, uz) || !take_u32(p, n, o, nz)) {
                err = "truncated chunk header"; return false;
            }
            if (nz > (uint32_t)WS_CHUNK_CELLS) { err = "chunk claims more cells than 16^3"; return false; }
            if ((uint64_t)nz * 4 > (uint64_t)(n - o)) { err = "truncated chunk body"; return false; }
            const int cx = (int)(ux & 0xFFFFFu), cy = (int)(uy & 0xFFFu), cz = (int)(uz & 0xFFFFFu);
            for (uint32_t si = 0; si < nz; ++si) {
                uint16_t idx = 0;
                if (!take_u16(p, n, o, idx)) { err = "truncated cell"; return false; }
                if (idx >= WS_CHUNK_CELLS) { err = "cell index out of range"; return false; }
                const unsigned char t = (unsigned char)p[o++];
                const unsigned char col = (unsigned char)p[o++];
                set(cx * WS_CHUNK + (idx & 15),
                    cy * WS_CHUNK + ((idx >> 8) & 15),
                    cz * WS_CHUNK + ((idx >> 4) & 15), t, col);
            }
        }
        return true;
    }

private:
    template <class F>
    static void emit_chunk(const WChunk& c, int cx, int cy, int cz, F& fn) {
        const int bx = cx * WS_CHUNK, by = cy * WS_CHUNK, bz = cz * WS_CHUNK;
        for (int i = 0; i < WS_CHUNK_CELLS; ++i) {
            const unsigned char t = c.type[i];
            if (t == 0) continue;
            fn(bx + (i & 15), by + ((i >> 8) & 15), bz + ((i >> 4) & 15),
               ws_decode_type(t), c.color[i]);
        }
    }

    static uint64_t key_of(int x, int y, int z) {
        return ws_chunk_key((int)((uint32_t)x & WS_XZ_MASK) >> 4,
                            (int)((uint32_t)y & WS_Y_MASK)  >> 4,
                            (int)((uint32_t)z & WS_XZ_MASK) >> 4);
    }
    const WChunk* find_chunk(int x, int y, int z) const {
        auto it = chunks_.find(key_of(x, y, z));
        return it == chunks_.end() ? nullptr : &it->second;
    }

    static void put_u16(std::string& s, uint16_t v) {
        s.push_back((char)(v & 0xFF)); s.push_back((char)((v >> 8) & 0xFF));
    }
    static void put_u32(std::string& s, uint32_t v) {
        for (int i = 0; i < 4; ++i) s.push_back((char)((v >> (8 * i)) & 0xFF));
    }
    static void put_u64(std::string& s, uint64_t v) {
        for (int i = 0; i < 8; ++i) s.push_back((char)((v >> (8 * i)) & 0xFF));
    }
    static bool take_u16(const char* p, size_t n, size_t& o, uint16_t& v) {
        if (n - o < 2) return false;
        v = (uint16_t)((unsigned char)p[o] | ((unsigned char)p[o + 1] << 8)); o += 2; return true;
    }
    static bool take_u32(const char* p, size_t n, size_t& o, uint32_t& v) {
        if (n - o < 4) return false;
        v = 0; for (int i = 0; i < 4; ++i) v |= (uint32_t)(unsigned char)p[o + i] << (8 * i);
        o += 4; return true;
    }
    static bool take_u64(const char* p, size_t n, size_t& o, uint64_t& v) {
        if (n - o < 8) return false;
        v = 0; for (int i = 0; i < 8; ++i) v |= (uint64_t)(unsigned char)p[o + i] << (8 * i);
        o += 8; return true;
    }

    std::unordered_map<uint64_t, WChunk> chunks_;
    size_t cells_ = 0;
    size_t coerced_ = 0;
    size_t outOfRange_ = 0;
};

// --- the legacy text reader --------------------------------------------------

/// Parse one legacy `x:y:z:type[:color]` line into `out`. Returns false for a
/// line with fewer than four fields (blank lines, trailing newline, comments) —
/// byte-for-byte the acceptance rule the pre-7.6 `loadWorld()` used, `atoi` and
/// all, so no world that loaded before stops loading now.
inline bool world_parse_text_line(const std::string& line, int& x, int& y, int& z,
                                  unsigned char& type, unsigned char& color) {
    if (line.empty()) return false;
    int v[5] = {0, 0, 0, 0, 0};
    int n = 0;
    size_t start = 0;
    while (n < 5) {
        const size_t sep = line.find(':', start);
        const std::string f = line.substr(start, sep == std::string::npos ? std::string::npos : sep - start);
        v[n++] = std::atoi(f.c_str());
        if (sep == std::string::npos) break;
        start = sep + 1;
    }
    if (n < 4) return false;
    x = v[0]; y = v[1]; z = v[2];
    type = (unsigned char)v[3];
    color = (unsigned char)v[4];
    return true;
}

/// Load the legacy text format from a stream into `store`. Returns the rows
/// stored; a row outside `ws_in_world` is skipped and counted in the store's
/// `out_of_range()`.
inline size_t world_load_text(std::istream& in, WorldStore& store) {
    std::string line;
    size_t n = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        int x, y, z; unsigned char t, c;
        if (!world_parse_text_line(line, x, y, z, t, c)) continue;
        if (store.set(x, y, z, t, c)) ++n;   // out-of-world rows: counted by the store
    }
    return n;
}

/// Which reader a blob got. `saveWorld()` always writes EDMB; this is only about
/// what was found on disk.
enum class WorldFormat { Edmb, LegacyText };

}  // namespace ewb
