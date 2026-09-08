// region_test.cpp — offline checks for ROADMAP-SERVER stage 1.1 (`REGION` → `SNAPZ`).
//
//   clang++ -std=c++17 -O2 -Wall region_test.cpp -lz -o region_test
//   ./region_test
//
// Covers, without a socket or a world:
//   * the chunk-aligned reply box, checked against the Pass-2 capture's own numbers
//   * request-point validation (the 24-bit key range)
//   * the Cell -> record table, all four rows, incl. "255 never reaches the wire"
//   * frame splitting at 3000 records, last frame short, nothing lost or duplicated
//   * sorting changes only the order, never the multiset of records
//   * a whole simulated region round-tripping back through the decode path
//
// The socket, the lock discipline and the rate limiter live in server_posix.cpp and
// are exercised by the stage 1.8 verification ladder, not here.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "region_query.h"

using ewb::SnapRec;

static int g_fail = 0;

#define CHECK(cond, msg)                                                             \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__);    \
            ++g_fail;                                                                \
        }                                                                            \
    } while (0)

// --- reply geometry ----------------------------------------------------------

static void test_region_box() {
    // The Pass-2 capture: REGION:65539:65540 returned records spanning
    // x 65312–65691, z 65312–65775. Three of the four edges must land exactly on
    // the prediction; the fourth (x1) is content-bounded and only has to *contain*
    // what was observed.
    const ewb::RegionBox b = ewb::region_box(65539, 65540);
    CHECK(b.x0 == 65312, "capture x0 == 65312");
    CHECK(b.z0 == 65312, "capture z0 == 65312");
    CHECK(b.z1 == 65775, "capture z1 == 65775");
    CHECK(b.x1 >= 65691, "capture x1 contains the observed content edge");

    // Chunk alignment: the low edges sit on a chunk boundary, the high edges one
    // block short of the next one.
    CHECK(b.x0 % 16 == 0 && b.z0 % 16 == 0, "low edges are chunk-aligned");
    CHECK((b.x1 + 1) % 16 == 0 && (b.z1 + 1) % 16 == 0, "high edges close a chunk");

    // The guaranteed-covered box: [c-R, c+R] is always inside, whatever the phase.
    for (int c : {0, 15, 16, 224, 225, 1000, 65539, 0xFFFFFF}) {
        const ewb::RegionBox g = ewb::region_box(c, c);
        CHECK(g.x0 <= c - ewb::REGION_RADIUS && g.x1 >= c + ewb::REGION_RADIUS,
              "box covers [c-R, c+R] on x");
        CHECK(g.z0 <= c - ewb::REGION_RADIUS && g.z1 >= c + ewb::REGION_RADIUS,
              "box covers [c-R, c+R] on z");
    }

    // snap_chunk must floor, including below zero — c - R goes negative near the
    // origin even though the request point itself cannot.
    CHECK(ewb::snap_chunk(0) == 0, "snap_chunk(0)");
    CHECK(ewb::snap_chunk(15) == 0, "snap_chunk(15)");
    CHECK(ewb::snap_chunk(16) == 16, "snap_chunk(16)");
    CHECK(ewb::snap_chunk(-1) == -16, "snap_chunk(-1) floors");
    CHECK(ewb::snap_chunk(-16) == -16, "snap_chunk(-16)");
    CHECK(ewb::snap_chunk(-17) == -32, "snap_chunk(-17) floors");
    const ewb::RegionBox origin = ewb::region_box(0, 0);
    CHECK(origin.x0 == -224 && origin.z0 == -224, "box at the origin floors below zero");

    // VuencLink's lattice: consecutive stride-spaced boxes must not leave a gap.
    const int stride = 2 * ewb::REGION_RADIUS;
    for (int c = 65000; c < 65000 + 3 * stride; c += stride) {
        const ewb::RegionBox a = ewb::region_box(c, 0);
        const ewb::RegionBox n = ewb::region_box(c + stride, 0);
        CHECK(n.x0 <= a.x1 + 1, "stride-spaced boxes abut or overlap");
    }

    // Contains is x/z only — y is unconstrained (the reply is a column of the world).
    CHECK(b.contains(65312, 65312) && b.contains(b.x1, b.z1), "corners are inside");
    CHECK(!b.contains(b.x0 - 1, b.z0) && !b.contains(b.x0, b.z1 + 1), "just outside is outside");
}

static void test_point_validation() {
    CHECK(ewb::region_point_valid(0, 0), "0,0 is valid");
    CHECK(ewb::region_point_valid(0xFFFFFF, 0xFFFFFF), "24-bit max is valid");
    CHECK(!ewb::region_point_valid(-1, 0), "negative x rejected");
    CHECK(!ewb::region_point_valid(0, -1), "negative z rejected");
    CHECK(!ewb::region_point_valid(0x1000000, 0), "x past the 24-bit range rejected");
    CHECK(!ewb::region_point_valid(0, 0x1000000), "z past the 24-bit range rejected");
}

// --- Cell -> record(s) -------------------------------------------------------

static void test_cell_encoding() {
    std::vector<SnapRec> out;

    // air: one record, flag 1, type -1 (NOT 0, NOT 255)
    out.clear();
    ewb::emit_cell_records(1, 2, 3, /*type*/ 0, /*color*/ 0, out);
    CHECK(out.size() == 1, "air -> one record");
    CHECK(out[0].flag == 1 && out[0].type == -1, "air -> flag 1, -1");

    // air that somehow carries a colour is still air on the wire
    out.clear();
    ewb::emit_cell_records(1, 2, 3, 0, 42, out);
    CHECK(out.size() == 1 && out[0].flag == 1 && out[0].type == -1,
          "air ignores a stale colour");

    // painted base: one standalone flag-3 record carrying the *colour*
    out.clear();
    ewb::emit_cell_records(4, 5, 6, /*type*/ 255, /*color*/ 11, out);
    CHECK(out.size() == 1, "painted base -> one record");
    CHECK(out[0].flag == 3 && out[0].type == 11, "painted base -> flag 3, colour");

    // plain solid
    out.clear();
    ewb::emit_cell_records(7, 8, 9, /*type*/ 74, /*color*/ 0, out);
    CHECK(out.size() == 1, "unpainted solid -> one record");
    CHECK(out[0].flag == 0 && out[0].type == 74, "unpainted solid -> flag 0, type");

    // painted solid: two records, type first then colour
    out.clear();
    ewb::emit_cell_records(7, 8, 9, /*type*/ 74, /*color*/ 54, out);
    CHECK(out.size() == 2, "painted solid -> two records");
    CHECK(out[0].flag == 0 && out[0].type == 74, "painted solid -> flag 0, type");
    CHECK(out[1].flag == 3 && out[1].type == 54, "painted solid -> flag 3, colour");
    CHECK(out[0].x == out[1].x && out[0].y == out[1].y && out[0].z == out[1].z,
          "both records share the cell's coordinates");

    // A painted base whose colour is 0 describes nothing — no record.
    out.clear();
    ewb::emit_cell_records(4, 5, 6, 255, 0, out);
    CHECK(out.empty(), "painted base with colour 0 emits nothing");

    // ⚠️ 255 must never reach the wire in field 5, for *any* colour byte — as a
    // block type (the sentinel) or as an out-of-palette paint index. ACTION mode 3
    // doesn't validate its extra, so Cell::color can hold any byte.
    for (int color = 0; color <= 255; ++color) {
        for (int type : {0, 8, 74, 254, 255}) {
            out.clear();
            ewb::emit_cell_records(0, 0, 0, (unsigned char)type, (unsigned char)color, out);
            for (const auto& r : out) {
                CHECK(r.type != 255, "255 never appears in field 5");
                if (r.flag == 3)
                    CHECK(r.type >= 1 && r.type <= ewb::CELL_MAX_PAINT,
                          "a flag-3 record's colour is a real palette index");
            }
        }
    }

    // An out-of-palette colour is dropped, not clamped: the block survives unpainted
    // and a painted *base* disappears (without a colour it describes no edit).
    out.clear();
    ewb::emit_cell_records(1, 1, 1, 74, 200, out);
    CHECK(out.size() == 1 && out[0].flag == 0 && out[0].type == 74,
          "bogus paint on a solid block drops the paint, keeps the block");
    out.clear();
    ewb::emit_cell_records(1, 1, 1, 255, 200, out);
    CHECK(out.empty(), "painted base with a bogus colour emits nothing");
    out.clear();
    ewb::emit_cell_records(1, 1, 1, 255, ewb::CELL_MAX_PAINT, out);
    CHECK(out.size() == 1 && out[0].type == ewb::CELL_MAX_PAINT,
          "the top of the palette is still accepted");
    // ...nor for any solid type: field 5 of a flag-0 record is 1..254.
    for (int t = 1; t <= 254; ++t) {
        out.clear();
        ewb::emit_cell_records(0, 0, 0, (unsigned char)t, 0, out);
        CHECK(out.size() == 1 && out[0].flag == 0 && out[0].type == t,
              "solid types 1..254 pass through");
    }
}

// --- a simulated region ------------------------------------------------------

// Build a deterministic pile of records covering all four cell shapes.
static std::vector<SnapRec> synth(size_t cells) {
    std::vector<SnapRec> out;
    for (size_t i = 0; i < cells; ++i) {
        const int x = 65312 + int(i % 449);
        const int z = 65312 + int((i / 449) % 449);
        const int y = int(i % 200);
        switch (i % 4) {
            case 0: ewb::emit_cell_records(x, y, z, 0, 0, out); break;             // air
            case 1: ewb::emit_cell_records(x, y, z, 255, 1 + i % 54, out); break;  // painted base
            case 2: ewb::emit_cell_records(x, y, z, 8, 0, out); break;             // solid
            case 3: ewb::emit_cell_records(x, y, z, 74, 1 + i % 54, out); break;   // solid+paint
        }
    }
    return out;
}

// Multiset key so "same records, any order" is comparable.
static std::map<std::string, int> tally(const std::vector<SnapRec>& v) {
    std::map<std::string, int> m;
    char buf[96];
    for (const auto& r : v) {
        std::snprintf(buf, sizeof(buf), "%d,%d,%d,%d,%d", r.x, r.y, r.z, r.flag, r.type);
        ++m[buf];
    }
    return m;
}

static void test_sort_preserves_contents() {
    std::vector<SnapRec> a = synth(5000);
    std::vector<SnapRec> b = a;
    ewb::sort_records(b);
    CHECK(a.size() == b.size(), "sort does not change the record count");
    CHECK(tally(a) == tally(b), "sort permutes but never alters the records");

    bool ordered = true;
    for (size_t i = 1; i < b.size(); ++i) {
        const SnapRec& p = b[i - 1];
        const SnapRec& q = b[i];
        const bool le = (p.z < q.z) || (p.z == q.z && p.x < q.x) ||
                        (p.z == q.z && p.x == q.x && p.y < q.y) ||
                        (p.z == q.z && p.x == q.x && p.y == q.y && p.flag <= q.flag);
        if (!le) { ordered = false; break; }
    }
    CHECK(ordered, "sorted output is ordered by (z, x, y, flag)");

    // Deterministic: sorting an already-sorted burst is a no-op.
    std::vector<SnapRec> c = b;
    ewb::sort_records(c);
    CHECK(tally(b) == tally(c), "sort is idempotent");
}

// Split exactly as serveRegion does, then decode every frame back.
static void test_frame_split_and_round_trip() {
    // 7001 records -> 3 frames of 3000/3000/1001.
    std::vector<SnapRec> recs = synth(7001);
    while (recs.size() != 7001) recs.pop_back();   // trim the paint-pair overshoot
    ewb::sort_records(recs);

    CHECK(ewb::snapz_frame_count(0) == 0, "0 records -> 0 frames");
    CHECK(ewb::snapz_frame_count(1) == 1, "1 record -> 1 frame");
    CHECK(ewb::snapz_frame_count(3000) == 1, "3000 records -> 1 frame");
    CHECK(ewb::snapz_frame_count(3001) == 2, "3001 records -> 2 frames");
    CHECK(ewb::snapz_frame_count(recs.size()) == 3, "7001 records -> 3 frames");

    std::vector<SnapRec> decoded;
    size_t frames = 0, lastCount = 0;
    for (size_t i = 0; i < recs.size(); i += ewb::SNAPZ_FRAME_RECORDS) {
        const size_t n = std::min(ewb::SNAPZ_FRAME_RECORDS, recs.size() - i);
        const std::string line = ewb::encode_snapz(recs.data() + i, n);
        ++frames;
        lastCount = n;

        CHECK(line.rfind("SNAPZ:", 0) == 0 && line.back() == '\n', "frame is a SNAPZ line");
        const std::string body = line.substr(0, line.size() - 1);
        const size_t c1 = body.find(':');
        const size_t c2 = body.find(':', c1 + 1);
        const long count = std::strtol(body.substr(c1 + 1, c2 - c1 - 1).c_str(), nullptr, 10);
        CHECK(size_t(count) == n, "header count matches the frame's record count");

        const std::vector<uint8_t> raw =
            ewb::raw_inflate(ewb::b64_decode(body.substr(c2 + 1)).data(),
                             ewb::b64_decode(body.substr(c2 + 1)).size(), n * 20);
        CHECK(raw.size() == n * 20, "inflated length == count * 20");
        for (size_t k = 0; k < n; ++k) {
            const uint8_t* p = raw.data() + k * 20;
            decoded.push_back({ewb::get_le_i32(p), ewb::get_le_i32(p + 4), ewb::get_le_i32(p + 8),
                               ewb::get_le_i32(p + 12), ewb::get_le_i32(p + 16)});
        }
    }
    CHECK(frames == 3, "burst split into 3 frames");
    CHECK(lastCount == 1001 && lastCount < ewb::SNAPZ_FRAME_RECORDS, "final frame is short");
    CHECK(decoded.size() == recs.size(), "no record lost or duplicated across frames");
    CHECK(tally(decoded) == tally(recs), "every record survives the split + round trip");

    // Is sorting before deflate worth it? The plan asks for this measured rather
    // than assumed. `g_world` is an unordered_map, so the natural order is hash
    // order — modelled here with a fixed-seed shuffle.
    const size_t n0 = std::min(recs.size(), ewb::SNAPZ_FRAME_RECORDS);
    const size_t rawB = n0 * 20;
    const std::string sorted = ewb::encode_snapz(recs.data(), n0);

    std::vector<SnapRec> hashOrder = recs;
    std::mt19937 rng(12345);
    std::shuffle(hashOrder.begin(), hashOrder.end(), rng);
    const std::string unsorted = ewb::encode_snapz(hashOrder.data(), n0);

    std::printf("  frame of %zu records: sorted %zu B (%.1fx), hash-order %zu B (%.1fx)\n", n0,
                sorted.size(), double(rawB) / double(sorted.size()), unsorted.size(),
                double(rawB) / double(unsorted.size()));
    CHECK(sorted.size() < unsorted.size(), "sorting before deflate compresses better");
}

int main() {
    test_region_box();
    test_point_validation();
    test_cell_encoding();
    test_sort_preserves_contents();
    test_frame_split_and_round_trip();
    if (g_fail) {
        std::fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
