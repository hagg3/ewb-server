// eden_fixture.h — synthetic `.eden` files for the offline suites.
//
// Test-only. Not compiled into `edenserver`, `edenmatch` or `eden_import`; used
// by `eden_file_test.cpp` (ROADMAP-SERVER stage 5.0) and `eden_import_test.cpp`
// (stage 5.1) so there is exactly one writer of the format the parser reads.
//
// **Every fixture is synthesized here.** No specimen from any private tree is
// copied into this repository, and nothing here encodes anything beyond the
// public format description in `eden_file.h`.
//
// One deliberate duplication: `base_profile()` below restates the client's base
// terrain as a literal, rather than calling `eden_import.h`'s
// `eden_default_profile()`. It is the independent half of the "`diff` on a flat
// world emits zero cells" check — if the two ever disagree, that test is
// supposed to fail.

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "eden_file.h"
#include "snapz_codec.h"   // ewb::raw_deflate — to build a deflated ZIP member

namespace edenfix {

// ── little-endian append helpers ────────────────────────────────────────────

inline void put_u16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8));
}
inline void put_u32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}
inline void put_u64(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(uint8_t(x >> (8 * i)));
}
inline void put_i32(std::vector<uint8_t>& v, int32_t x) { put_u32(v, uint32_t(x)); }
inline void put_f32(std::vector<uint8_t>& v, float f) {
    uint32_t u; std::memcpy(&u, &f, 4); put_u32(v, u);
}
inline void put_u32_at(std::vector<uint8_t>& v, size_t at, uint32_t x) {
    for (int i = 0; i < 4; ++i) v[at + i] = uint8_t(x >> (8 * i));
}

// ── voxels ──────────────────────────────────────────────────────────────────

struct Voxel { uint8_t type, paint; };

// The client's fixed base terrain (docs/import.md, measured): bedrock @0,
// stone 1..15, dirt 16..31, grass @32, air >= 33. Identical in 64z and 256z.
inline Voxel base_profile(int z) {
    if (z == 0)  return {1, 0};    // bedrock
    if (z <= 15) return {2, 0};    // stone
    if (z <= 31) return {3, 0};    // dirt
    if (z == 32) return {8, 0};    // grass
    return {0, 0};                 // air
}

// A 16x16xbands chunk in the game's raw storage order:
//   type  = band*8192 + lx*256 + ly*16 + lz
//   paint = type + 4096   (band is 8192 B: 4096 type then 4096 paint)
template <class Fn>
inline std::vector<uint8_t> build_chunk(int bands, Fn voxel) {
    std::vector<uint8_t> c(size_t(bands) * 8192, 0);
    for (int band = 0; band < bands; ++band)
        for (int lx = 0; lx < 16; ++lx)
            for (int ly = 0; ly < 16; ++ly)
                for (int lz = 0; lz < 16; ++lz) {
                    int z = band * 16 + lz;
                    Voxel vx = voxel(lx, ly, z);
                    size_t o = size_t(band) * 8192 + size_t(lx) * 256 + size_t(ly) * 16 + lz;
                    c[o] = vx.type;
                    c[o + 4096] = vx.paint;
                }
    return c;
}

// ── worlds ──────────────────────────────────────────────────────────────────

struct ChunkSpec {
    int32_t cx, cy;
    std::vector<uint8_t> data;
    // If non-zero, the directory records this offset instead of the natural
    // packed one — used to manufacture the 107,072-byte overlap case.
    uint64_t forced_offset = 0;
};

struct WorldSpec {
    int32_t version = 4;
    std::string name = "Test World";
    float pos[3]  = {65540.0f, 33.925f, 65536.0f};
    float home[3] = {65536.0f, 22.0f, 65536.0f};
    float yaw = 1.5f;
    int32_t seed = 12345;
    size_t creature_gap = 0;               // bytes between last chunk end and directory
    std::vector<ChunkSpec> chunks;
    std::vector<uint8_t> trailer;          // appended raw after the directory rows
    // Raw directory rows emitted *before* the real chunk rows — interior content
    // that the parser must gate/drop rather than fold into the trailer.
    std::vector<ewb::EdenDirEntry> interior_rows;
};

inline std::vector<uint8_t> build_world(const WorldSpec& w) {
    std::vector<uint8_t> out;

    // Chunk data, packed from 192, honouring forced offsets.
    std::vector<uint64_t> offsets;
    std::vector<uint8_t> body;
    uint64_t cursor = 192;
    for (auto& ch : w.chunks) {
        uint64_t off = ch.forced_offset ? ch.forced_offset : cursor;
        if (off + ch.data.size() > body.size() + 192)
            body.resize(off + ch.data.size() - 192, 0);
        std::memcpy(body.data() + (off - 192), ch.data.data(), ch.data.size());
        offsets.push_back(off);
        cursor = std::max(cursor, off + ch.data.size());
    }
    uint64_t dir_offset = 192 + body.size() + w.creature_gap;

    // Header (192 bytes).
    put_i32(out, w.seed);
    for (float f : w.pos)  put_f32(out, f);
    for (float f : w.home) put_f32(out, f);
    put_f32(out, w.yaw);
    put_u64(out, dir_offset);
    {
        std::string nm = w.name;
        nm.resize(50, '\0');
        out.insert(out.end(), nm.begin(), nm.end());   // name @40..90
    }
    out.push_back(0); out.push_back(0);    // pad @90..92
    put_i32(out, w.version);               // version @92..96
    out.resize(132, 0);                    // hash[36] @96..132
    for (int i = 0; i < 16; ++i) out.push_back(uint8_t(3 + i));  // skycolors @132..148
    out.resize(192, 0);

    out.insert(out.end(), body.begin(), body.end());
    out.resize(dir_offset, 0);             // creature gap

    for (auto& e : w.interior_rows) {
        put_i32(out, e.cx);
        put_i32(out, e.cy);
        put_u64(out, e.off);
    }
    for (size_t i = 0; i < w.chunks.size(); ++i) {
        put_i32(out, w.chunks[i].cx);
        put_i32(out, w.chunks[i].cy);
        put_u64(out, offsets[i]);
    }
    out.insert(out.end(), w.trailer.begin(), w.trailer.end());
    return out;
}

inline ChunkSpec flat_chunk(int bands, int32_t cx = 0, int32_t cy = 0) {
    return {cx, cy, build_chunk(bands, [](int, int, int z) { return base_profile(z); }), 0};
}

// ── signs ───────────────────────────────────────────────────────────────────

struct SignRec { int32_t x, y, z, a, b, c; std::string text; };

inline std::vector<uint8_t> build_sgn1(const std::vector<SignRec>& recs) {
    std::vector<uint8_t> b;
    b.insert(b.end(), {'S', 'G', 'N', '1'});
    put_u32(b, 1);                          // version
    put_u32(b, uint32_t(recs.size()));      // count
    for (auto& r : recs) {
        put_i32(b, r.x); put_i32(b, r.y); put_i32(b, r.z);
        put_i32(b, r.a); put_i32(b, r.b); put_i32(b, r.c);
        std::string t = r.text; t.resize(96, '\0');
        b.insert(b.end(), t.begin(), t.end());
    }
    return b;
}

// Wrap signs as the inline trailer form: an outer "SGN1"|len|0 (12 B), then the
// sidecar container, then zero padding — all split into 12-byte payload slots
// each prefixed with ff ff ff ff to make a 16-byte tagged row.
inline std::vector<uint8_t> build_inline_trailer(const std::vector<SignRec>& recs,
                                                 int pad_rows = 3) {
    std::vector<uint8_t> inner = build_sgn1(recs);
    std::vector<uint8_t> payload;
    payload.insert(payload.end(), {'S', 'G', 'N', '1'});
    put_u32(payload, uint32_t(inner.size()));
    put_u32(payload, 0);
    payload.insert(payload.end(), inner.begin(), inner.end());
    while (payload.size() % 12 != 0) payload.push_back(0);

    std::vector<uint8_t> trailer;
    for (size_t i = 0; i < payload.size(); i += 12) {
        trailer.insert(trailer.end(), {0xff, 0xff, 0xff, 0xff});
        trailer.insert(trailer.end(), payload.begin() + i, payload.begin() + i + 12);
    }
    for (int i = 0; i < pad_rows; ++i) {
        trailer.insert(trailer.end(), {0xff, 0xff, 0xff, 0xff});
        for (int j = 0; j < 12; ++j) trailer.push_back(0);
    }
    return trailer;
}

// ── minimal ZIP writer ──────────────────────────────────────────────────────

// method 0 = stored, 8 = raw deflate.
inline void zip_add(std::vector<uint8_t>& out, std::vector<uint8_t>& central,
                    const std::string& name, const std::vector<uint8_t>& data,
                    uint16_t method) {
    uint32_t crc = uint32_t(crc32(0, data.data(), uInt(data.size())));
    std::vector<uint8_t> stored =
        method == 8 ? ewb::raw_deflate(data.data(), data.size()) : data;
    uint32_t local_off = uint32_t(out.size());

    put_u32(out, 0x04034b50);
    put_u16(out, 20); put_u16(out, 0); put_u16(out, method);
    put_u16(out, 0);  put_u16(out, 0);
    put_u32(out, crc);
    put_u32(out, uint32_t(stored.size()));
    put_u32(out, uint32_t(data.size()));
    put_u16(out, uint16_t(name.size())); put_u16(out, 0);
    out.insert(out.end(), name.begin(), name.end());
    out.insert(out.end(), stored.begin(), stored.end());

    put_u32(central, 0x02014b50);
    put_u16(central, 20); put_u16(central, 20); put_u16(central, 0);
    put_u16(central, method); put_u16(central, 0); put_u16(central, 0);
    put_u32(central, crc);
    put_u32(central, uint32_t(stored.size()));
    put_u32(central, uint32_t(data.size()));
    put_u16(central, uint16_t(name.size()));
    put_u16(central, 0); put_u16(central, 0); put_u16(central, 0);
    put_u16(central, 0); put_u32(central, 0);
    put_u32(central, local_off);
    central.insert(central.end(), name.begin(), name.end());
}

struct ZipMember { std::string name; std::vector<uint8_t> data; uint16_t method; };

inline std::vector<uint8_t> build_zip(const std::vector<ZipMember>& members) {
    std::vector<uint8_t> out, central;
    for (auto& m : members) zip_add(out, central, m.name, m.data, m.method);
    uint32_t cd_off = uint32_t(out.size());
    out.insert(out.end(), central.begin(), central.end());
    uint32_t cd_size = uint32_t(out.size()) - cd_off;
    put_u32(out, 0x06054b50);
    put_u16(out, 0); put_u16(out, 0);
    put_u16(out, uint16_t(members.size()));
    put_u16(out, uint16_t(members.size()));
    put_u32(out, cd_size);
    put_u32(out, cd_off);
    put_u16(out, 0);
    return out;
}

}  // namespace edenfix
