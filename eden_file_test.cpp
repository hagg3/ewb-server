// eden_file_test.cpp — offline checks for ROADMAP-SERVER stage 5.0 (`eden_file.h`,
// the `.eden` world-format parser).
//
//   clang++ -std=c++17 -O2 -Wall eden_file_test.cpp -lz -o eden_file_test
//   ./eden_file_test
//
// Every fixture is synthesized here — no specimen from any private tree is
// copied into this repo. Covers, without a socket or a real world file:
//   * the 192-byte header decode (all fields)
//   * chunk-size detection: version >= 5, the creature-gap detector overriding
//     `version = 2`/`4` on 256z, the 12,000-byte legacy slot rule, min-gap fallback
//   * the coordinate + off >= 192 directory gate
//   * per-chunk *derived* spans, incl. the 107,072-byte overlap case — a read past
//     the span reads as air, never the neighbour's bytes
//   * voxel addressing against the documented base terrain profile
//   * one sign parser behind the sidecar and the inline post-directory trailer
//   * ZIP: member selection past a __MACOSX/ decoy, stored + deflated, bomb cap
//
// The golden end-to-end (fixture .eden → eden_world.model byte-for-byte) belongs
// to stage 5.1, where the converter that produces the model file exists.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "eden_file.h"
#include "snapz_codec.h"   // ewb::raw_deflate — to build a deflated ZIP member

using namespace ewb;

static int g_fail = 0;
#define CHECK(cond, msg)                                                          \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_fail;                                                            \
        }                                                                       \
    } while (0)

// ── little-endian append helpers ────────────────────────────────────────────

static void put_u16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8));
}
static void put_u32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}
static void put_u64(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(uint8_t(x >> (8 * i)));
}
static void put_i32(std::vector<uint8_t>& v, int32_t x) { put_u32(v, uint32_t(x)); }
static void put_u32_at(std::vector<uint8_t>& v, size_t at, uint32_t x) {
    for (int i = 0; i < 4; ++i) v[at + i] = uint8_t(x >> (8 * i));
}
static void put_f32(std::vector<uint8_t>& v, float f) {
    uint32_t u; std::memcpy(&u, &f, 4); put_u32(v, u);
}

// ── fixture builders ────────────────────────────────────────────────────────

struct Voxel { uint8_t type, paint; };

// The client's fixed base terrain (docs/import.md, measured): bedrock @0,
// stone 1..15, dirt 16..31, grass @32, air >= 33. Identical in 64z and 256z.
static Voxel base_profile(int z) {
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
static std::vector<uint8_t> build_chunk(int bands, Fn voxel) {
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
    std::vector<EdenDirEntry> interior_rows;
};

static std::vector<uint8_t> build_world(const WorldSpec& w, size_t bands_out = 0) {
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
    (void)bands_out;
    return out;
}

// ── sign fixtures ───────────────────────────────────────────────────────────

struct SignRec { int32_t x, y, z, a, b, c; std::string text; };

static std::vector<uint8_t> build_sgn1(const std::vector<SignRec>& recs) {
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

// Wrap a single sign as the inline trailer form: an outer "SGN1"|len|0 (12 B),
// then the sidecar container, then zero padding — all split into 12-byte payload
// slots each prefixed with ff ff ff ff to make a 16-byte tagged row.
static std::vector<uint8_t> build_inline_trailer(const std::vector<SignRec>& recs,
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
static void zip_add(std::vector<uint8_t>& out, std::vector<uint8_t>& central,
                    const std::string& name, const std::vector<uint8_t>& data,
                    uint16_t method) {
    uint32_t crc = uint32_t(crc32(0, data.data(), uInt(data.size())));
    std::vector<uint8_t> stored =
        method == 8 ? raw_deflate(data.data(), data.size()) : data;
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

static std::vector<uint8_t> build_zip(const std::vector<ZipMember>& members) {
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

// ── tests ───────────────────────────────────────────────────────────────────

static ChunkSpec flat_chunk(int bands, int32_t cx = 0, int32_t cy = 0) {
    return {cx, cy, build_chunk(bands, [](int, int, int z) { return base_profile(z); }), 0};
}

static void test_header() {
    WorldSpec w;
    w.version = 6;
    w.name = "Hello Eden";
    w.chunks.push_back(flat_chunk(16));
    std::vector<uint8_t> bytes = build_world(w);

    EdenHeader h = eden_parse_header(bytes.data(), bytes.size());
    CHECK(h.seed == 12345, "header seed");
    CHECK(h.pos[0] == 65540.0f && h.pos[1] == 33.925f, "header pos");
    CHECK(h.home[1] == 22.0f, "header home");
    CHECK(h.yaw == 1.5f, "header yaw");
    CHECK(h.name == "Hello Eden", "header name (NUL-trimmed)");
    CHECK(h.version == 6, "header version");
    CHECK(h.skycolors[0] == 3 && h.skycolors[15] == 18, "header skycolors");
    CHECK(h.dir_offset == 192 + 16 * 8192, "header dir_offset");
}

static void test_flat_64z() {
    WorldSpec w;
    w.version = 4;
    w.chunks.push_back(flat_chunk(4));
    EdenWorld world = eden_load(build_world(w));

    CHECK(world.chunk_size == 32768, "64z chunk size");
    CHECK(world.bands == 4 && world.z_ceiling == 63, "64z bands/ceiling");
    CHECK(world.chunks.size() == 1, "64z one chunk");
    CHECK(world.chunks[0].span() == 32768, "64z full span");

    const uint8_t* b = world.data();
    const EdenChunk& c = world.chunks[0];
    CHECK(eden_block(b, c, 0, 0, 0) == 1,  "voxel bedrock @0");
    CHECK(eden_block(b, c, 5, 9, 15) == 2, "voxel stone @15");
    CHECK(eden_block(b, c, 5, 9, 16) == 3, "voxel dirt @16");
    CHECK(eden_block(b, c, 15, 15, 32) == 8, "voxel grass @32");
    CHECK(eden_block(b, c, 0, 0, 33) == 0, "voxel air @33");
    CHECK(eden_block(b, c, 0, 0, 63) == 0, "voxel air @ceiling");
}

static void test_carved_and_paint() {
    WorldSpec w;
    w.version = 4;
    w.chunks.push_back({0, 0, build_chunk(4, [](int lx, int ly, int z) -> Voxel {
        if (lx >= 4 && lx <= 6 && ly >= 4 && ly <= 6 && z >= 20 && z <= 22)
            return {0, 0};                       // 3x3x3 carved void
        if (z == 33 && lx == 1 && ly == 1) return {255, 200};  // raw pass-through
        return base_profile(z);
    }), 0});
    EdenWorld world = eden_load(build_world(w));
    const uint8_t* b = world.data();
    const EdenChunk& c = world.chunks[0];

    CHECK(eden_block(b, c, 5, 5, 21) == 0, "carved void reads air");
    CHECK(eden_block(b, c, 5, 5, 19) == 3, "rock below the void intact");
    CHECK(eden_block(b, c, 1, 1, 33) == 255, "type 255 passes through the parser verbatim");
    CHECK(eden_paint(b, c, 1, 1, 33) == 200, "paint 200 passes through verbatim");
}

static void test_detect_version2_256z() {
    // version = 2, one 131072-byte chunk, 24000-byte creature gap → 256z.
    WorldSpec w;
    w.version = 2;
    w.creature_gap = 24000;
    w.chunks.push_back(flat_chunk(16));
    EdenWorld world = eden_load(build_world(w));
    CHECK(world.chunk_size == 131072, "creature-gap overrides version=2 → 256z");
    CHECK(world.bands == 16 && world.z_ceiling == 255, "256z bands/ceiling");
}

static void test_detect_single_chunk_256z_v4() {
    // version = 4 but a single 131072-byte chunk with a valid creature gap;
    // min-gap has no second offset, creature-gap must decide.
    WorldSpec w;
    w.version = 4;
    w.creature_gap = 60 * 400;   // exactly 400 slots
    w.chunks.push_back(flat_chunk(16));
    EdenWorld world = eden_load(build_world(w));
    CHECK(world.chunk_size == 131072, "single-chunk 256z resolved by creature gap");
}

static void test_detect_legacy_12000_gap() {
    // Two 32768-byte chunks, a 12,000-byte gap (200 slots, not 400) → 64z.
    WorldSpec w;
    w.version = 4;
    w.creature_gap = 12000;
    w.chunks.push_back(flat_chunk(4, 10, 10));
    w.chunks.push_back(flat_chunk(4, 10, 11));
    EdenWorld world = eden_load(build_world(w));
    CHECK(world.chunk_size == 32768, "12,000-byte gap is a valid 64z creature block");
    CHECK(world.chunks.size() == 2, "both legacy chunks kept");
}

static void test_detect_min_gap_fallback() {
    // A gap that is a valid slot count for neither chunk-size interpretation
    // (5000 is not a multiple of 60, and negative against the 256z guess), so
    // the creature-gap detector abstains and the min-offset-gap fallback runs:
    // two chunks 32768 apart → 64z.
    WorldSpec w;
    w.version = 0;
    w.creature_gap = 5000;
    w.chunks.push_back(flat_chunk(4, 20, 20));
    w.chunks.push_back(flat_chunk(4, 20, 21));
    EdenWorld world = eden_load(build_world(w));
    CHECK(world.chunk_size == 32768, "min-gap fallback: 32768-apart chunks → 64z");

    // The other half of the fallback, and the one that costs a whole world if
    // it is wrong: two 256z chunks 131072 apart, again with a creature gap that
    // is a valid slot count for neither size. The fallback must answer 256z —
    // answering 64z here reads every chunk at a quarter of its real height.
    WorldSpec big;
    big.version = 0;
    big.creature_gap = 5000;
    big.chunks.push_back(flat_chunk(16, 20, 20));
    big.chunks.push_back(flat_chunk(16, 20, 21));
    EdenWorld bw = eden_load(build_world(big));
    CHECK(bw.chunk_size == 131072, "min-gap fallback: 131072-apart chunks → 256z");
    CHECK(bw.bands == 16 && bw.chunks.size() == 2, "min-gap fallback: 256z bands, both chunks");
    if (bw.chunks.size() == 2)
        CHECK(bw.chunks[0].span() == 131072, "min-gap fallback: full 256z span");

    // Single chunk, creature-gap abstains: the only evidence is whether a 256z
    // chunk fits before the directory. A 64z chunk plus its <= 24,000-byte
    // creature block cannot reach 131,072, so "it fits" means 256z.
    WorldSpec one;
    one.version = 0;
    one.creature_gap = 5000;
    one.chunks.push_back(flat_chunk(16));
    CHECK(eden_load(build_world(one)).chunk_size == 131072,
          "min-gap fallback: lone 256z chunk resolved by directory headroom");
    WorldSpec one64;
    one64.version = 0;
    one64.creature_gap = 5000;
    one64.chunks.push_back(flat_chunk(4));
    CHECK(eden_load(build_world(one64)).chunk_size == 32768,
          "min-gap fallback: lone 64z chunk stays 64z");
}

// A directory row's `off` is arbitrary bytes out of the file. A row whose
// offset is near 2^64 must be rejected, not wrapped into range by `off +
// chunk_size` — the wrapped form passes a naive bounds test and then reads
// wildly out of the buffer. Same for a row pointing into the directory itself,
// whose bytes would otherwise be decoded as voxels.
static void test_hostile_directory_offsets() {
    WorldSpec w;
    w.version = 4;
    w.chunks.push_back(flat_chunk(4, 7, 7));
    std::vector<uint8_t> bytes = build_world(w);

    const uint64_t dir_offset = 192 + 32768;
    auto with_extra_row = [&](int32_t cx, int32_t cy, uint64_t off) {
        std::vector<uint8_t> v = bytes;
        put_i32(v, cx); put_i32(v, cy); put_u64(v, off);
        return v;
    };

    // off + 32768 wraps to 100, which is <= the file size.
    EdenWorld wrapped = eden_load(with_extra_row(1, 1, ~uint64_t(0) - 32768 + 101));
    CHECK(wrapped.chunks.size() == 1, "hostile: wrapping offset row rejected");
    for (auto& c : wrapped.chunks)
        CHECK(c.off + c.span() <= wrapped.size() && c.end >= c.off,
              "hostile: every kept chunk stays inside the buffer");

    // A row pointing at the directory would decode directory bytes as voxels.
    EdenWorld in_dir = eden_load(with_extra_row(2, 2, dir_offset + 16));
    CHECK(in_dir.chunks.size() == 1, "hostile: row pointing into the directory rejected");

    // Detection must not be skewed by either, and the good chunk survives.
    CHECK(wrapped.chunk_size == 32768 && in_dir.chunk_size == 32768,
          "hostile: chunk-size detection ignores unaddressable rows");
    CHECK(wrapped.chunks[0].cx == 7 && in_dir.chunks[0].cx == 7,
          "hostile: the real chunk survives");
}

static void test_short_span_overlap() {
    // Two 131072-byte chunks, but the directory places the second only 107,072
    // bytes after the first — they overlap by 24,000. The first chunk's span
    // must clamp to 107,072 and a read past it must return air, not chunk 2.
    WorldSpec w;
    w.version = 6;
    // chunk 0 has grass at z=32 everywhere; chunk 1 has a marker block (5) at
    // the voxel that a naive chunk_size read of chunk 0 would land on.
    w.chunks.push_back({0, 0, build_chunk(16, [](int, int, int z) { return base_profile(z); }), 192});
    // z that maps past offset 107072 within chunk 0: band*8192 needs > 107072 →
    // band >= 14 (114688). Put a distinctive block at band 14, lz 0 (z=224).
    w.chunks.push_back({0, 1, build_chunk(16, [](int lx, int ly, int z) -> Voxel {
        if (z == 224 && lx == 0 && ly == 0) return {5, 0};
        return base_profile(z);
    }), 192 + 107072});
    w.creature_gap = 24000;

    EdenWorld world = eden_load(build_world(w));
    CHECK(world.chunk_size == 131072, "overlap fixture is 256z");
    // Find chunk (0,0).
    const EdenChunk* c0 = nullptr;
    for (auto& c : world.chunks) if (c.cx == 0 && c.cy == 0) c0 = &c;
    CHECK(c0 != nullptr, "overlap: chunk (0,0) present");
    CHECK(c0 && c0->span() == 107072, "overlap: chunk (0,0) span clamped to 107072");
    CHECK(c0 && eden_block(world.data(), *c0, 0, 0, 224) == 0,
          "overlap: read past the derived span is air, not the neighbour's block");
    CHECK(c0 && eden_block(world.data(), *c0, 0, 0, 32) == 8,
          "overlap: reads inside the span still work");
}

static void test_inline_sign_trailer() {
    WorldSpec w;
    w.version = 4;
    w.chunks.push_back(flat_chunk(4, 100, 100));
    // interior garbage row (fails the coord gate) that must be dropped, not
    // captured into the trailer.
    w.interior_rows.push_back({-1, 777, 999999});
    w.trailer = build_inline_trailer({{65540, 65551, 33, 4, 2, 1, "a:b sign"}});
    EdenWorld world = eden_load(build_world(w));

    CHECK(world.chunks.size() == 1, "trailer world: one real chunk");
    CHECK(world.signs.size() == 1, "inline trailer: one sign decoded");
    if (world.signs.size() == 1) {
        const EdenSign& s = world.signs[0];
        CHECK(s.x == 65540 && s.y == 65551 && s.z == 33, "inline sign coords");
        CHECK(s.a == 4 && s.b == 2 && s.c == 1, "inline sign a/b/c verbatim");
        CHECK(s.text == "a:b sign", "inline sign text keeps a ':'");
    }
}

static void test_sidecar_signs() {
    std::vector<uint8_t> good = build_sgn1({
        {1, 2, 3, 0, 0, 0, "one"},
        {4, 5, 6, 7, 8, 9, "two:with:colons"},
    });
    auto signs = eden_parse_signs(good);
    CHECK(signs.size() == 2, "sidecar: two signs");
    CHECK(signs.size() == 2 && signs[1].text == "two:with:colons", "sidecar: text with colons");
    CHECK(signs.size() == 2 && signs[1].b == 8, "sidecar: b verbatim");

    good[0] = 'X';
    CHECK(eden_parse_signs(good).empty(), "sidecar: wrong magic → empty");

    std::vector<uint8_t> torn = build_sgn1({{1, 2, 3, 0, 0, 0, "a"}, {4, 5, 6, 0, 0, 0, "b"}});
    torn.resize(torn.size() - 40);
    CHECK(eden_parse_signs(torn).size() == 1, "sidecar: torn trailing record dropped");

    std::vector<uint8_t> empty_text = build_sgn1({{1, 2, 3, 0, 0, 0, ""}});
    auto et = eden_parse_signs(empty_text);
    CHECK(et.size() == 1 && et[0].text.empty(), "sidecar: empty text ok");

    // inline parser rejects untagged / misaligned input
    CHECK(eden_parse_inline_signs(std::vector<uint8_t>(16, 0)).empty(),
          "inline: untagged rows → empty");
    CHECK(eden_parse_inline_signs(std::vector<uint8_t>(20, 0xff)).empty(),
          "inline: non-16-aligned → empty");

    // A trailer carrying the bare SGN1 container with no outer wrapper row.
    std::vector<uint8_t> bare = build_sgn1({{9, 8, 7, 1, 2, 3, "bare"}});
    while (bare.size() % 12 != 0) bare.push_back(0);
    std::vector<uint8_t> rows;
    for (size_t i = 0; i < bare.size(); i += 12) {
        rows.insert(rows.end(), {0xff, 0xff, 0xff, 0xff});
        rows.insert(rows.end(), bare.begin() + i, bare.begin() + i + 12);
    }
    auto bs = eden_parse_inline_signs(rows);
    CHECK(bs.size() == 1 && bs[0].text == "bare", "inline: unwrapped SGN1 container also parses");
}

static void test_zip() {
    // fixture: a flat 64z world.
    WorldSpec w;
    w.version = 4;
    w.chunks.push_back(flat_chunk(4));
    std::vector<uint8_t> raw = build_world(w);

    // stored member + a __MACOSX decoy that would parse as garbage.
    std::vector<uint8_t> zip_stored = build_zip({
        {"__MACOSX/._world.eden", std::vector<uint8_t>(200, 0xAB), 0},
        {"world.eden", raw, 0},
    });
    CHECK(eden_is_zip(zip_stored.data(), zip_stored.size()), "zip: magic detected");
    EdenWorld z1 = eden_load(zip_stored);
    CHECK(z1.chunk_size == 32768 && z1.chunks.size() == 1,
          "zip: stored .eden member selected past the __MACOSX decoy");

    // deflated member.
    std::vector<uint8_t> zip_deflate = build_zip({{"w.eden", raw, 8}});
    EdenWorld z2 = eden_load(zip_deflate);
    CHECK(z2.chunk_size == 32768 && z2.chunks.size() == 1, "zip: deflated member inflates");

    // bomb cap, path 1: the declared uncompressed size is over the ceiling.
    bool threw = false;
    try { eden_load(zip_deflate.data(), zip_deflate.size(), /*max_unzip=*/1024); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw, "zip: declared size over the cap is refused (bomb guard)");

    // bomb cap, path 2: the declared size *lies*, so the pre-check passes and
    // only the growth guard inside the inflate loop can stop it. This is the
    // real bomb shape — a 167x member whose header claims it is tiny.
    std::vector<uint8_t> lying = zip_deflate;               // single member
    size_t eocd   = lying.size() - 22;
    size_t cd_off = rd_u32(lying.data() + eocd + 16);
    put_u32_at(lying, 22, 64);            // local header  +22 = uncompressed size
    put_u32_at(lying, cd_off + 24, 64);   // central entry +24 = uncompressed size
    CHECK(rd_u32(lying.data() + cd_off) == 0x02014b50, "zip: bomb fixture patched in place");
    threw = false;
    try { eden_load(lying.data(), lying.size(), /*max_unzip=*/4096); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw, "zip: understated size still refused by the inflate growth guard");

    // and with a truthful, generous ceiling the same archive still parses.
    CHECK(eden_load(lying).chunks.size() == 1, "zip: honest ceiling still parses the member");
}

static void test_directory_gate() {
    // A row with off < 192 and a row at a wild coordinate are both excluded;
    // the one good chunk survives.
    WorldSpec w;
    w.version = 4;
    w.chunks.push_back(flat_chunk(4, 5, 5));
    w.interior_rows.push_back({6, 6, 100});       // off < 192 → dropped in Pass B
    // (a coord-gate failure interior to the rows is covered by test_inline_sign_trailer)
    EdenWorld world = eden_load(build_world(w));
    CHECK(world.chunks.size() == 1, "directory gate: off < 192 row excluded");
    CHECK(world.chunks[0].cx == 5, "directory gate: the valid chunk survives");
}

int main() {
    test_header();
    test_flat_64z();
    test_carved_and_paint();
    test_detect_version2_256z();
    test_detect_single_chunk_256z_v4();
    test_detect_legacy_12000_gap();
    test_detect_min_gap_fallback();
    test_hostile_directory_offsets();
    test_short_span_overlap();
    test_inline_sign_trailer();
    test_sidecar_signs();
    test_zip();
    test_directory_gate();

    if (g_fail) {
        std::fprintf(stderr, "\n%d check(s) FAILED\n", g_fail);
        return 1;
    }
    std::printf("eden_file_test: all checks passed\n");
    return 0;
}
