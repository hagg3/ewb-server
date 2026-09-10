// eden_import_test.cpp — offline checks for ROADMAP-SERVER stage 5.1
// (`eden_import.h`, the `.eden` → server-world conversion core, and the
// `eden_import` CLI built from it).
//
//   clang++ -std=c++17 -O2 -Wall eden_import_test.cpp -lz -o eden_import_test
//   ./eden_import_test          # run from the repo root
//
// Fixtures are synthesized by `eden_fixture.h`; no specimen from any private
// tree is copied into this repo. Covers:
//   * `diff` on a flat world emits **zero** cells — the sharpest check there is
//     that the shipped base profile matches what the format stores
//   * `full` round-trips every voxel of a chunk; `solid` drops air
//   * the golden end-to-end: fixture .eden -> eden_world.model, byte-for-byte
//     against testdata/carved_64z.model
//   * the axis rename, for blocks and for signs, with a `:` in sign text
//   * the two budget projections, including the sliding REGION window
//   * type 255 (hard error) and paint > 54 (warn, keep)
//   * the CLI: --dry-run writes nothing, refusals are refusals, and a real run
//     produces the golden plus its sidecars
//
// The CLI half shells out to `./eden_import`, which `build_server.sh` builds
// first; run this suite from the repo root.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "eden_file.h"
#include "eden_fixture.h"
#include "eden_import.h"

using namespace ewb;
using namespace edenfix;

static int g_fail = 0;
#define CHECK(cond, msg)                                                          \
    do {                                                                          \
        if (!(cond)) {                                                            \
            std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_fail;                                                             \
        }                                                                         \
    } while (0)

// ── shared fixtures ─────────────────────────────────────────────────────────

// Fixture 1: one flat 64z chunk of pure base terrain, at a realistic chunk
// coordinate (4096 -> block 65536, the plane's centre).
static std::vector<uint8_t> fixture_flat_64z() {
    WorldSpec w;
    w.version = 4;
    w.name = "Flat";
    w.chunks.push_back(flat_chunk(4, 4096, 4096));
    return build_world(w);
}

// Fixture 2: the flat world with a 3x3x3 void carved at z 20..22 and two blocks
// placed at z 33 — one plain, one painted (so it costs two records).
static const int kVoidLo = 4, kVoidHi = 6, kVoidZ0 = 20, kVoidZ1 = 22;
static std::vector<uint8_t> fixture_carved_64z() {
    WorldSpec w;
    w.version = 4;
    w.name = "Carved";
    w.chunks.push_back({4096, 4096, build_chunk(4, [](int lx, int ly, int z) -> Voxel {
        if (lx >= kVoidLo && lx <= kVoidHi && ly >= kVoidLo && ly <= kVoidHi &&
            z >= kVoidZ0 && z <= kVoidZ1)
            return {0, 0};                                    // carved void
        if (z == 33 && lx == 1 && ly == 1) return {8, 0};      // a plain block
        if (z == 33 && lx == 2 && ly == 1) return {74, 12};    // a painted block
        return base_profile(z);
    }), 0});
    return build_world(w);
}

static EdenWorld load(const std::vector<uint8_t>& bytes) { return eden_load(bytes); }

static size_t count_lines(const std::string& s) {
    size_t n = 0;
    for (char c : s) if (c == '\n') ++n;
    return n;
}

// ── the emitter ─────────────────────────────────────────────────────────────

static void test_profile_table() {
    BaseProfile p = eden_default_profile();
    CHECK(p.at(0).type == 1, "profile: bedrock at 0");
    CHECK(p.at(1).type == 2 && p.at(15).type == 2, "profile: stone 1..15");
    CHECK(p.at(16).type == 3 && p.at(31).type == 3, "profile: dirt 16..31");
    CHECK(p.at(32).type == 8, "profile: grass at 32");
    CHECK(p.at(33).type == 0 && p.at(255).type == 0, "profile: air from 33 up");
    CHECK(p.at(-1).type == 0, "profile: below 0 is air");
    for (int z = 0; z <= 255; ++z) CHECK(p.at(z).paint == 0, "profile: never painted");
    CHECK(eden_empty_profile().at(0).type == 0, "profile: none is all air");
}

static void test_diff_flat_emits_nothing() {
    ImportOptions o;   // diff + default profile
    EdenWorld w = load(fixture_flat_64z());
    ImportProjection p = eden_project(w, o);
    CHECK(p.cells == 0, "diff on a flat 64z world emits zero cells");
    CHECK(p.records == 0 && p.empty, "diff on a flat world has no records and no bbox");
    CHECK(eden_build_model(w, o).empty(), "diff on a flat world writes an empty model");
    CHECK(p.worst_region.records == 0, "diff on a flat world has no region hotspot");

    // The same profile has to hold in the 256z format — that is the whole claim.
    WorldSpec big;
    big.version = 6;
    big.chunks.push_back(flat_chunk(16, 4096, 4096));
    CHECK(eden_project(load(build_world(big)), o).cells == 0,
          "diff on a flat 256z world emits zero cells");
}

static void test_full_round_trips_every_voxel() {
    ImportOptions o;
    o.air_fill = AirFill::Full;
    EdenWorld w = load(fixture_carved_64z());
    ImportProjection p = eden_project(w, o);
    CHECK(p.cells == 16 * 16 * 64, "full emits every voxel of the chunk");

    // Every emitted cell must carry exactly the bytes the file holds, at the
    // renamed coordinate: server(x, y, z) = file(cx*16+lx, z, cy*16+ly).
    size_t seen = 0, mismatched = 0;
    eden_scan(w, o, [&](int x, int y, int z, uint8_t type, uint8_t paint) {
        const int lx = x - 65536, ly = z - 65536;
        Voxel want = base_profile(y);
        if (lx >= kVoidLo && lx <= kVoidHi && ly >= kVoidLo && ly <= kVoidHi &&
            y >= kVoidZ0 && y <= kVoidZ1) want = {0, 0};
        if (y == 33 && lx == 1 && ly == 1) want = {8, 0};
        if (y == 33 && lx == 2 && ly == 1) want = {74, 12};
        if (lx < 0 || lx > 15 || ly < 0 || ly > 15 || type != want.type || paint != want.paint)
            ++mismatched;
        ++seen;
    });
    CHECK(seen == 16 * 16 * 64, "full: scan visits every voxel");
    CHECK(mismatched == 0, "full: every voxel round-trips through the axis rename");
    CHECK(p.y0 == 0 && p.y1 == 63, "full: bbox spans the whole 64z column");
    CHECK(p.x0 == 65536 && p.x1 == 65551, "full: bbox x is the chunk's 16 blocks");
    CHECK(p.z0 == 65536 && p.z1 == 65551, "full: bbox z is the chunk's 16 blocks");
}

static void test_solid_skips_air() {
    ImportOptions o;
    o.air_fill = AirFill::Solid;
    EdenWorld w = load(fixture_carved_64z());
    ImportProjection p = eden_project(w, o);
    // 33 solid layers per column (0..32) over 256 columns, minus the 27 carved
    // voxels, plus the two blocks placed at 33.
    CHECK(p.cells == 33 * 256 - 27 + 2, "solid emits solids only");
    bool any_air = false;
    eden_scan(w, o, [&](int, int, int, uint8_t type, uint8_t) { if (!type) any_air = true; });
    CHECK(!any_air, "solid never emits an air cell");
}

static void test_diff_carved_counts() {
    ImportOptions o;
    EdenWorld w = load(fixture_carved_64z());
    ImportProjection p = eden_project(w, o);
    CHECK(p.cells == 27 + 2, "diff on the carved fixture: 27 air + 2 placed blocks");
    // 27 air records + 1 plain solid + 2 for the painted solid (flag 0 and flag 3).
    CHECK(p.records == 27 + 1 + 2, "diff: a painted solid costs two records");
    CHECK(p.y0 == 20 && p.y1 == 33, "diff: bbox spans the void and the placed blocks");
    CHECK(p.bad_type_cells == 0 && p.bad_paint_cells == 0 && p.sentinel_cells == 0,
          "diff: the carved fixture is clean");
    CHECK(p.worst_region.records == 30, "diff: the single chunk is the region hotspot");
}

static void test_record_cost_matches_the_wire() {
    // eden_record_cost must agree with emit_cell_records, which is what the
    // server actually puts on the wire — the projection is worthless otherwise.
    const uint8_t types[]  = {0, 1, 8, 74, 127, 254, CELL_PAINTED_BASE};
    const uint8_t paints[] = {0, 1, 54, 55, 200, 255};
    for (uint8_t t : types)
        for (uint8_t c : paints) {
            std::vector<SnapRec> recs;
            emit_cell_records(1, 2, 3, t, c, recs);
            CHECK(recs.size() == size_t(eden_record_cost(t, c)),
                  "record cost matches emit_cell_records");
        }
}

// ── budgets ─────────────────────────────────────────────────────────────────

static void test_worst_region_window() {
    // The reply box is 29 chunks on a side. Three chunks: two adjacent, one far
    // enough away that no single box can hold all three.
    std::vector<std::pair<std::pair<int, int>, size_t>> chunks = {
        {{100, 100}, 10}, {{101, 100}, 20}, {{500, 500}, 25},
    };
    RegionHotspot h = eden_worst_region(chunks);
    CHECK(h.records == 30, "worst region: picks the adjacent pair, not the lone chunk");
    CHECK(h.x0 <= 100 * 16 && h.x1 >= 101 * 16 + 15, "worst region: box covers both chunks");

    // Exactly at the window edge: 29 chunks in a row are one box, 30 are not.
    std::vector<std::pair<std::pair<int, int>, size_t>> row;
    for (int i = 0; i < 29; ++i) row.push_back({{i, 0}, 1});
    CHECK(eden_worst_region(row).records == 29, "worst region: 29 chunks fit one box");
    row.push_back({{29, 0}, 1});
    CHECK(eden_worst_region(row).records == 29, "worst region: the 30th does not");

    // Same in the z axis, and diagonally.
    std::vector<std::pair<std::pair<int, int>, size_t>> colv;
    for (int i = 0; i < 30; ++i) colv.push_back({{0, i}, 1});
    CHECK(eden_worst_region(colv).records == 29, "worst region: window applies to z too");

    CHECK(eden_worst_region({}).records == 0, "worst region: empty world");

    // A smaller radius must yield a smaller window.
    CHECK(eden_worst_region(row, 16).records < 29, "worst region: honours --region-radius");
}

static void test_worst_region_uses_records_not_cells() {
    // A chunk of painted solids costs two records per cell; the hotspot number
    // is the one that lands on the wire, so it must reflect that.
    WorldSpec w;
    w.version = 4;
    w.chunks.push_back({4096, 4096, build_chunk(4, [](int, int, int z) -> Voxel {
        if (z == 40) return {74, 12};      // painted solid, above the base profile
        return base_profile(z);
    }), 0});
    ImportProjection p = eden_project(load(build_world(w)), ImportOptions{});
    CHECK(p.cells == 256, "painted layer: one cell per column");
    CHECK(p.records == 512, "painted layer: two records per cell");
    CHECK(p.worst_region.records == 512, "worst region counts records, not cells");
}

// ── anomalies ───────────────────────────────────────────────────────────────

static void test_sentinel_and_paint_flags() {
    WorldSpec w;
    w.version = 4;
    w.chunks.push_back({4096, 4096, build_chunk(4, [](int lx, int ly, int z) -> Voxel {
        if (z == 40 && lx == 3 && ly == 4) return {255, 7};    // the sentinel
        if (z == 41 && lx == 3 && ly == 4) return {74, 200};   // paint out of palette
        if (z == 42 && lx == 3 && ly == 4) return {200, 0};    // unknown block id
        return base_profile(z);
    }), 0});
    ImportProjection p = eden_project(load(build_world(w)), ImportOptions{});
    CHECK(p.sentinel_cells == 1, "type 255 is counted");
    CHECK(p.sentinel_at[0] == 65539 && p.sentinel_at[1] == 40 && p.sentinel_at[2] == 65540,
          "type 255 reports the first offender in server coords");
    CHECK(p.bad_paint_cells == 1, "paint over 54 is counted");
    CHECK(p.bad_type_cells == 1, "a block id over 127 is counted, not the sentinel again");
    CHECK(p.cells == 3, "anomalous voxels are still emitted, not silently dropped");
}

// ── signs ───────────────────────────────────────────────────────────────────

static void test_sign_rename_and_grammar() {
    size_t dropped = 0;
    // sign(x, y, z) = (plane x, plane y, height)  ->  server(x, height, plane y)
    std::vector<EdenSign> in = {
        {65540, 65551, 33, 4, 2, 1, "subway: collab Stations"},
        {65540, 65551, 999, 0, 0, 0, "height out of range"},
        {-3, 65551, 33, 0, 0, 0, "negative plane"},
    };
    std::vector<Sign> out = eden_convert_signs(in, dropped);
    CHECK(out.size() == 1 && dropped == 2, "signs: out-of-range records dropped and counted");
    if (out.size() == 1) {
        CHECK(out[0].x == 65540, "sign rename: x is x");
        CHECK(out[0].y == 33, "sign rename: server y is the file's z (height)");
        CHECK(out[0].z == 65551, "sign rename: server z is the file's y (plane)");
        CHECK(out[0].a == 4 && out[0].b == 2 && out[0].c == 1, "sign a/b/c verbatim");
    }

    // The line the tool writes must be a line `sign_store.h` reads back, with a
    // ':' inside the text surviving — that is the one property the format has.
    const std::string line = eden_format_sign_line(out[0]);
    Sign back;
    bool skip = false;
    CHECK(parse_sign_line(line, back, skip), "sign line round-trips through parse_sign_line");
    CHECK(back.x == out[0].x && back.y == out[0].y && back.z == out[0].z,
          "sign line round-trip: coordinates");
    CHECK(back.a == 4 && back.b == 2 && back.c == 1, "sign line round-trip: a/b/c");
    CHECK(back.text == "subway: collab Stations", "sign line round-trip: ':' inside the text");

    // Control bytes must not reach the file: a newline would forge a second row.
    EdenSign nasty = {1, 2, 3, 0, 0, 0, "a\nb:c"};
    std::vector<Sign> s2 = eden_convert_signs({nasty}, dropped);
    CHECK(s2.size() == 1, "signs: a control byte does not drop the record");
    CHECK(count_lines(eden_format_sign_line(s2[0])) == 1, "signs: text cannot forge a new line");
}

static void test_signs_from_a_world() {
    // The inline trailer path, end to end from bytes.
    WorldSpec w;
    w.version = 4;
    w.chunks.push_back(flat_chunk(4, 4096, 4096));
    w.trailer = build_inline_trailer({{65540, 65551, 33, 4, 2, 1, "hi"}});
    EdenWorld world = load(build_world(w));
    size_t dropped = 0;
    std::vector<Sign> s = eden_convert_signs(world.signs, dropped);
    CHECK(s.size() == 1 && s[0].y == 33 && s[0].z == 65551, "inline trailer signs are renamed");
    CHECK(eden_build_signs(s) == "65540:33:65551:4:2:1:hi\n", "eden_signs.txt body");
}

// ── spawn ───────────────────────────────────────────────────────────────────

static void test_spawn() {
    EdenWorld w = load(fixture_flat_64z());
    // The header's pos is already (x_plane, height, y_plane) — server order.
    Spawn s = eden_spawn_from(w.hdr, false);
    CHECK(s.x == 65540.0f && s.y == 33.925f && s.z == 65536.0f, "spawn: header pos, no rename");
    CHECK(eden_spawn_from(w.hdr, true).y == 22.0f, "spawn: home is available but different");
    CHECK(eden_format_spawn(s) == "65540.00:33.92:65536.00\n", "spawn: eden_spawn.txt line");
    CHECK(eden_spawn_in_range(s), "spawn: a walkable header pos is in range");
    CHECK(!eden_spawn_in_range({65536.0f, 900.0f, 65536.0f}), "spawn: height out of range");
    CHECK(!eden_spawn_in_range({-1.0f, 33.0f, 65536.0f}), "spawn: negative plane out of range");
}

// ── the profile file ────────────────────────────────────────────────────────

static void test_profile_file() {
    BaseProfile p;
    std::string err;
    CHECK(eden_parse_profile("# a comment\n\n0:1\n1-15:2\n16-31:3\n32:8:0\n", p, err),
          "profile file: the default table parses");
    CHECK(err.empty(), "profile file: no error on a good file");
    BaseProfile d = eden_default_profile();
    bool same = p.layers.size() == d.layers.size();
    for (size_t i = 0; same && i < p.layers.size(); ++i) same = (p.layers[i] == d.layers[i]);
    CHECK(same, "profile file: reproduces the built-in default exactly");

    CHECK(eden_parse_profile("40:9:3\n", p, err) && p.at(40).type == 9 && p.at(40).paint == 3,
          "profile file: explicit paint");
    CHECK(p.at(0).type == 0, "profile file: unmentioned heights are air");

    CHECK(!eden_parse_profile("0\n", p, err), "profile file: a bare height is an error");
    CHECK(!eden_parse_profile("300:1\n", p, err), "profile file: height over 255 is an error");
    CHECK(!eden_parse_profile("5-1:1\n", p, err), "profile file: an inverted range is an error");
    CHECK(!eden_parse_profile("0:999\n", p, err), "profile file: type over 255 is an error");
    CHECK(!eden_parse_profile("x:1\n", p, err), "profile file: a non-numeric height is an error");
    CHECK(!err.empty(), "profile file: errors name the line");
}

static void test_slug() {
    CHECK(eden_slug("My World") == "my-world", "slug: spaces and case");
    CHECK(eden_slug("  ../etc/passwd ") == "etc-passwd", "slug: path separators cannot survive");
    CHECK(eden_slug("!!!") == "world", "slug: an unusable name falls back");
    CHECK(eden_slug(std::string(200, 'a')).size() == 64, "slug: bounded length");
}

// ── the golden model file ───────────────────────────────────────────────────

static bool read_text(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

static void test_golden_model() {
    const std::string path = "testdata/carved_64z.model";
    std::string golden;
    if (!read_text(path, golden)) {
        std::fprintf(stderr, "FAIL: cannot read %s (run this suite from the repo root)\n",
                     path.c_str());
        ++g_fail;
        return;
    }
    const std::string got = eden_build_model(load(fixture_carved_64z()), ImportOptions{});
    CHECK(got == golden, "golden: diff of the carved fixture matches testdata/carved_64z.model");
    CHECK(count_lines(golden) == 29, "golden: 29 lines");

    // Determinism is what makes a golden meaningful: emit it twice.
    CHECK(got == eden_build_model(load(fixture_carved_64z()), ImportOptions{}),
          "golden: the emitter is deterministic");

    // And every line must be a line the server's own loader accepts.
    size_t parsed = 0;
    std::istringstream ss(got);
    std::string line;
    while (std::getline(ss, line)) {
        int v[5] = {0, 0, 0, 0, 0}, n = 0;
        std::istringstream ls(line);
        std::string tok;
        while (n < 5 && std::getline(ls, tok, ':')) v[n++] = std::atoi(tok.c_str());
        if (n == 5 && v[0] >= 0 && v[0] <= REGION_COORD_MAX && v[2] >= 0 &&
            v[2] <= REGION_COORD_MAX && v[1] >= 0 && v[1] <= SIGN_Y_MAX)
            ++parsed;
    }
    CHECK(parsed == 29, "golden: every line is a well-formed, in-range model row");
}

// ── the CLI ─────────────────────────────────────────────────────────────────

static bool write_file(const std::string& path, const std::vector<uint8_t>& b) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    return bool(f);
}

static bool exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

static int run(const std::string& cmd) {
    const int rc = std::system((cmd + " >/dev/null 2>&1").c_str());
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

static void test_cli() {
    if (!exists("./eden_import")) {
        std::fprintf(stderr, "FAIL: ./eden_import not built — run build_server.sh\n");
        ++g_fail;
        return;
    }
    char tmpl[] = "/tmp/eden_import_test.XXXXXX";
    const char* dir = ::mkdtemp(tmpl);
    if (!dir) { std::fprintf(stderr, "FAIL: mkdtemp\n"); ++g_fail; return; }
    const std::string d = dir;
    const std::string in = d + "/carved.eden";
    const std::string out = d + "/out";
    CHECK(write_file(in, fixture_carved_64z()), "cli: fixture written");

    const std::string base = "./eden_import '" + in + "' --out '" + out + "'";

    // --dry-run must not create the output directory, let alone write into it.
    CHECK(run(base + " --dry-run") == 0, "cli: --dry-run succeeds");
    CHECK(!exists(out), "cli: --dry-run writes nothing");

    // A real run writes the four files, and the model is the golden.
    CHECK(run(base) == 0, "cli: a real run succeeds");
    std::string got, golden;
    CHECK(read_text(out + "/eden_world.model", got), "cli: model written");
    CHECK(read_text("testdata/carved_64z.model", golden), "cli: golden readable");
    CHECK(got == golden, "cli: the written model is byte-for-byte the golden");
    CHECK(exists(out + "/eden_signs.txt"), "cli: signs sidecar written");
    CHECK(exists(out + "/eden_spawn.txt"), "cli: spawn sidecar written");
    std::string ign;
    CHECK(read_text(out + "/.gitignore", ign) && ign.find("\n*\n") != std::string::npos,
          "cli: the output .gitignore excludes the world");
    std::string spawn;
    CHECK(read_text(out + "/eden_spawn.txt", spawn) && spawn == "65540.00:33.92:65536.00\n",
          "cli: spawn comes from the header pos");

    // An existing directory is not overwritten without --force.
    CHECK(run(base) != 0, "cli: refuses to overwrite without --force");
    CHECK(run(base + " --force") == 0, "cli: --force overwrites");

    // --yes is the non-interactive scripting path: it implies --force but does
    // not weaken the budget verdicts.
    CHECK(run(base + " --yes") == 0, "cli: --yes overwrites without --force");
    CHECK(run("./eden_import '" + in + "' --out '" + d + "/y1' --yes"
              " --air-fill full --max-world-cells 100") != 0,
          "cli: --yes does not bypass the cell cap");

    // The cell cap is refusable, and refusing means writing nothing.
    const std::string out2 = d + "/out2";
    const std::string full = "./eden_import '" + in + "' --out '" + out2 + "' --air-fill full";
    CHECK(run(full + " --max-world-cells 100") != 0, "cli: over the cell cap is refused");
    CHECK(!exists(out2), "cli: a refused import writes nothing");
    CHECK(run(full + " --max-world-cells 100000") == 0, "cli: raising the cap allows it");

    // The region ceiling is refusable independently of the cell cap.
    const std::string out3 = d + "/out3";
    CHECK(run("./eden_import '" + in + "' --out '" + out3 +
              "' --air-fill full --max-region-records 100") != 0,
          "cli: over the region ceiling is refused");
    CHECK(!exists(out3), "cli: a region refusal writes nothing");

    // Type 255 is a hard error, not a warning.
    WorldSpec bad;
    bad.version = 4;
    bad.chunks.push_back({4096, 4096, build_chunk(4, [](int lx, int ly, int z) -> Voxel {
        if (z == 40 && lx == 0 && ly == 0) return {255, 3};
        return base_profile(z);
    }), 0});
    const std::string badin = d + "/sentinel.eden";
    const std::string out4 = d + "/out4";
    CHECK(write_file(badin, build_world(bad)), "cli: sentinel fixture written");
    CHECK(run("./eden_import '" + badin + "' --out '" + out4 + "'") != 0,
          "cli: type 255 is a hard error");
    CHECK(!exists(out4), "cli: the type-255 refusal writes nothing");

    // --strict promotes the paint warning to an error; the default keeps going.
    WorldSpec warn;
    warn.version = 4;
    warn.chunks.push_back({4096, 4096, build_chunk(4, [](int lx, int ly, int z) -> Voxel {
        if (z == 40 && lx == 0 && ly == 0) return {74, 200};
        return base_profile(z);
    }), 0});
    const std::string warnin = d + "/paint.eden";
    CHECK(write_file(warnin, build_world(warn)), "cli: paint fixture written");
    CHECK(run("./eden_import '" + warnin + "' --out '" + d + "/out5'") == 0,
          "cli: an out-of-palette paint only warns");
    CHECK(run("./eden_import '" + warnin + "' --out '" + d + "/out6' --strict") != 0,
          "cli: --strict promotes it to an error");

    // A missing input, a bad flag and a bad profile file all fail loudly.
    CHECK(run("./eden_import '" + d + "/nope.eden'") != 0, "cli: a missing input fails");
    CHECK(run(base + " --air-fill sideways") != 0, "cli: an unknown strategy fails");
    CHECK(run(base + " --nonsense") != 0, "cli: an unknown flag fails");
    CHECK(run("./eden_import --help") == 0, "cli: --help succeeds");

    run("rm -rf '" + d + "'");
}

int main() {
    test_profile_table();
    test_diff_flat_emits_nothing();
    test_full_round_trips_every_voxel();
    test_solid_skips_air();
    test_diff_carved_counts();
    test_record_cost_matches_the_wire();
    test_worst_region_window();
    test_worst_region_uses_records_not_cells();
    test_sentinel_and_paint_flags();
    test_sign_rename_and_grammar();
    test_signs_from_a_world();
    test_spawn();
    test_profile_file();
    test_slug();
    test_golden_model();
    test_cli();

    if (g_fail) {
        std::fprintf(stderr, "\n%d check(s) FAILED\n", g_fail);
        return 1;
    }
    std::printf("eden_import_test: all checks passed\n");
    return 0;
}
