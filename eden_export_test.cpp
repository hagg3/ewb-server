// eden_export_test.cpp — offline checks for ROADMAP-SERVER stage 5.5
// (`eden_export.h`, the server-world → `.eden` writer, and the `eden_export`
// CLI built from it).
//
//   clang++ -std=c++17 -O2 -Wall eden_export_test.cpp -lz -o eden_export_test
//   ./eden_export_test          # run from the repo root
//
// Fixtures are synthesized by `eden_fixture.h`; no specimen from any private
// tree is copied into this repo. Covers:
//   * **the round-trip property**, which is the exit criterion for this stage:
//     `import(export(import(f)))` has the same cell set as `import(f)`, and
//     `export(import(export(S)))` is byte-identical to `export(S)` — over flat,
//     carved, cave, 256z, mined, painted-base, signed and empty worlds
//   * the two sentinels: `CELL_MINED` (254, which a store surfaces as logical
//     air) and `CELL_PAINTED_BASE` (255), including 255 over a profile with no
//     block there
//   * chunk selection: a chunk is emitted iff the store holds a cell in it
//   * fill-then-overlay: an emitted chunk's untouched columns are base terrain
//   * the axis rename, backwards, for blocks and for signs
//   * 64z vs 256z selection, and `--z` forcing it
//   * drop-and-count: cells above the ceiling, chunk coordinates out of range,
//     signs that cannot be expressed
//   * signs as a sidecar and as the inline trailer, with a `:` in the text and
//     non-zero a/b/c
//   * the ZIP wrapper, and the byte-ceiling refusal
//   * the CLI: required arguments, refusals write nothing, the grep-able
//     summary, and the sidecar landing under the name `eden_import` expects
//
// The CLI half shells out to `./eden_export`, which `build_server.sh` builds
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

#include "eden_export.h"
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

// ── helpers ─────────────────────────────────────────────────────────────────

struct CellRec {
    int x, y, z;
    unsigned char type, color;
    bool operator<(const CellRec& o) const {
        if (x != o.x) return x < o.x;
        if (y != o.y) return y < o.y;
        if (z != o.z) return z < o.z;
        if (type != o.type) return type < o.type;
        return color < o.color;
    }
    bool operator==(const CellRec& o) const {
        return x == o.x && y == o.y && z == o.z && type == o.type && color == o.color;
    }
};

static std::vector<CellRec> cells_of(const WorldStore& s) {
    std::vector<CellRec> v;
    s.for_each([&](int x, int y, int z, unsigned char t, unsigned char c) {
        v.push_back({x, y, z, t, c});
    });
    std::sort(v.begin(), v.end());
    return v;
}

/// `eden_import`'s emitter, straight into a store — the same path the CLI takes
/// on its way to `eden_world.model`, minus the text round trip.
static WorldStore import_to_store(const std::vector<uint8_t>& raw,
                                  const ImportOptions& io = ImportOptions{}) {
    WorldStore s;
    const EdenWorld w = eden_load(raw);
    eden_scan(w, io, [&](int x, int y, int z, uint8_t t, uint8_t p) {
        s.set(x, y, z, t, p);
    });
    return s;
}

static std::vector<uint8_t> export_bytes(const WorldStore& s,
                                         const std::vector<Sign>& signs,
                                         const ExportOptions& opt, ExportResult& r) {
    std::string err;
    const bool ok = eden_export_world(s, signs, opt, r, err);
    CHECK(ok, ("export failed: " + err).c_str());
    return r.bytes;
}

static ExportOptions named(const char* n) {
    ExportOptions o;
    o.name = n;
    return o;
}

/// The whole exit criterion, as one callable. `signs` ride along so the sign
/// path is exercised by every world that has any.
static void check_round_trip(const char* what, const WorldStore& s,
                             const std::vector<Sign>& signs,
                             const ExportOptions& base = ExportOptions{}) {
    ExportOptions opt = base;
    if (opt.name.empty()) opt.name = what;

    ExportResult r1;
    const std::vector<uint8_t> f1 = export_bytes(s, signs, opt, r1);
    if (f1.empty()) return;

    // import(export(S)) — the cell set the server would host after a round trip.
    WorldStore s2;
    try {
        s2 = import_to_store(f1);
    } catch (const std::exception& e) {
        CHECK(false, (std::string(what) + ": re-import threw: " + e.what()).c_str());
        return;
    }

    // export(import(export(S))) — byte identity, the property that holds even
    // where the cell set legitimately shifts (a painted-base cell re-imports as
    // the explicit block it renders as, and writes the same bytes back).
    ExportResult r2;
    const std::vector<uint8_t> f2 = export_bytes(s2, signs, opt, r2);
    CHECK(f1 == f2, (std::string(what) + ": export(import(export(S))) is not byte-identical"
                                          " to export(S)").c_str());

    // One more turn: the cell set must now be stable.
    WorldStore s3;
    try {
        s3 = import_to_store(f2);
    } catch (const std::exception& e) {
        CHECK(false, (std::string(what) + ": second re-import threw: " + e.what()).c_str());
        return;
    }
    CHECK(cells_of(s2) == cells_of(s3),
          (std::string(what) + ": cell set is not stable across a second round trip").c_str());

    // Signs survive the file and come back in server coordinates unchanged.
    if (!signs.empty() && opt.signs != SignMode::None) {
        std::vector<EdenSign> file_signs;
        if (opt.signs == SignMode::Inline) {
            file_signs = eden_load(f1).signs;
        } else {
            file_signs = eden_parse_signs(r1.sign_sidecar);
        }
        size_t dropped = 0;
        const std::vector<Sign> back = eden_convert_signs(file_signs, dropped);
        CHECK(dropped == 0, (std::string(what) + ": a sign did not convert back").c_str());
        CHECK(back.size() == r1.signs,
              (std::string(what) + ": sign count changed across the round trip").c_str());
        for (size_t i = 0; i < back.size() && i < signs.size(); ++i) {
            const Sign& a = signs[i];
            const Sign& b = back[i];
            CHECK(a.x == b.x && a.y == b.y && a.z == b.z && a.a == b.a && a.b == b.b &&
                      a.c == b.c && a.text == b.text,
                  (std::string(what) + ": a sign changed across the round trip").c_str());
        }
    }
}

// ── fixtures ────────────────────────────────────────────────────────────────

// One flat 64z chunk of pure base terrain at the plane's centre chunk.
static std::vector<uint8_t> fixture_flat_64z() {
    WorldSpec w;
    w.name = "Flat";
    w.chunks.push_back(flat_chunk(4, 4096, 4096));
    return build_world(w);
}

// A 64z chunk with a cave (air where the profile has stone), a tower of placed
// blocks above the surface, and a painted surface block.
static std::vector<uint8_t> fixture_carved_64z() {
    WorldSpec w;
    w.name = "Carved";
    w.chunks.push_back({4096, 4096, build_chunk(4, [](int lx, int ly, int z) -> Voxel {
                            if (lx == 3 && ly == 4 && z >= 5 && z <= 10) return {0, 0};
                            if (lx == 8 && ly == 8 && z >= 33 && z <= 40) return {12, 3};
                            if (lx == 1 && ly == 1 && z == 32) return {8, 7};
                            return base_profile(z);
                        }), 0});
    return build_world(w);
}

// A 256z world: a block well above the 64z ceiling, in two chunks.
static std::vector<uint8_t> fixture_tall_256z() {
    WorldSpec w;
    w.name = "Tall";
    w.version = 5;
    w.chunks.push_back({4096, 4096, build_chunk(16, [](int lx, int ly, int z) -> Voxel {
                            if (lx == 2 && ly == 2 && z == 200) return {12, 0};
                            return base_profile(z);
                        }), 0});
    w.chunks.push_back({4097, 4096, build_chunk(16, [](int lx, int ly, int z) -> Voxel {
                            if (lx == 0 && ly == 0 && z == 33) return {5, 0};
                            return base_profile(z);
                        }), 0});
    return build_world(w);
}

static std::vector<Sign> sample_signs() {
    std::vector<Sign> v;
    v.push_back({65540, 33, 65545, 1, 2, 3, "hello: world"});
    v.push_back({65600, 40, 65600, 0, 0, 0, "second"});
    return v;
}

// ── 1. sentinels and the cell mapping ───────────────────────────────────────

static void test_sentinels() {
    const BaseProfile p = eden_default_profile();
    uint8_t t = 9, q = 9;
    bool on_air = false;

    // Logical air (what CELL_MINED surfaces as) → air, and the colour goes with it.
    eden_cell_to_file(p, 5, 0, 44, t, q, &on_air);
    CHECK(t == 0 && q == 0 && !on_air, "mined air exports as air with no paint");

    // Painted base over stone → the profile's block, recoloured.
    eden_cell_to_file(p, 5, CELL_PAINTED_BASE, 7, t, q, &on_air);
    CHECK(t == EDEN_STONE && q == 7 && !on_air, "painted base takes the profile's block");

    // Painted base at a height the profile leaves empty → air, counted.
    on_air = false;
    eden_cell_to_file(p, 100, CELL_PAINTED_BASE, 7, t, q, &on_air);
    CHECK(t == 0 && q == 0 && on_air, "painted base over air degrades to air and is counted");

    // Everything else verbatim.
    eden_cell_to_file(p, 40, 12, 3, t, q, &on_air);
    CHECK(t == 12 && q == 3, "an ordinary block is written verbatim");

    // The store never surfaces 254, so the writer never sees it: a set() of
    // CELL_MINED is recorded as mined air and counted.
    WorldStore s;
    s.set(65536, 5, 65536, CELL_MINED, 0);
    CHECK(s.reserved_coerced() == 1, "CELL_MINED is coerced by the store, not by the writer");
    ExportResult r;
    export_bytes(s, {}, named("mined"), r);
    const EdenWorld w = eden_load(r.bytes);
    CHECK(w.chunks.size() == 1, "one chunk for one cell");
    CHECK(eden_block(w.data(), w.chunks[0], 0, 0, 5) == 0, "the coerced cell is air in the file");
    check_round_trip("mined", s, {});

    WorldStore pb;
    pb.set(65536, 5, 65536, CELL_PAINTED_BASE, 7);
    pb.set(65537, 40, 65536, CELL_PAINTED_BASE, 7);   // above the profile: no block to paint
    ExportResult rp;
    export_bytes(pb, {}, named("painted"), rp);
    CHECK(rp.painted_base_on_air == 1, "painted-base-over-air is counted");
    const EdenWorld wp = eden_load(rp.bytes);
    CHECK(eden_block(wp.data(), wp.chunks[0], 0, 0, 5) == EDEN_STONE &&
              eden_paint(wp.data(), wp.chunks[0], 0, 0, 5) == 7,
          "painted base writes the profile's block with the stored colour");
    CHECK(eden_block(wp.data(), wp.chunks[0], 1, 0, 40) == 0,
          "painted base over air writes air");
    check_round_trip("painted", pb, {});
}

// ── 2. chunk selection and fill-then-overlay ────────────────────────────────

static void test_chunk_selection() {
    WorldStore s;
    s.set(65536, 40, 65536, 12, 0);     // chunk (4096, 4096)
    s.set(65600, 40, 65700, 12, 0);     // chunk (4100, 4106)
    ExportResult r;
    export_bytes(s, {}, named("two"), r);
    CHECK(r.chunks == 2, "one chunk per populated chunk coordinate, and no others");
    CHECK(r.cells == 2, "both cells landed");

    const EdenWorld w = eden_load(r.bytes);
    CHECK(w.chunks.size() == 2, "the directory holds exactly the emitted chunks");
    CHECK(w.chunk_size == 32768 && w.z_ceiling == 63, "a low world re-detects as 64z");

    // Fill-then-overlay: the rest of an emitted chunk is base terrain, so an
    // untouched column re-imports as nothing at all.
    const WorldStore back = import_to_store(r.bytes);
    CHECK(back.size() == 2, "only the two cells differ from the base profile");
    CHECK(cells_of(back) == cells_of(s), "the two cells come back unchanged");

    // A chunk with no cell is absent — the client synthesizes it.
    bool has_neighbour = false;
    for (const EdenChunk& c : w.chunks)
        if (c.cx == 4097 && c.cy == 4096) has_neighbour = true;
    CHECK(!has_neighbour, "an untouched chunk is not in the directory");
}

// ── 3. the axis rename, backwards ───────────────────────────────────────────

static void test_axis_rename() {
    // Server (x, y = height, z) → file (x, y = plane, z = height).
    WorldStore s;
    s.set(65536 + 3, 40, 65536 + 7, 12, 5);
    ExportResult r;
    export_bytes(s, {}, named("axis"), r);
    const EdenWorld w = eden_load(r.bytes);
    CHECK(w.chunks.size() == 1, "one chunk");
    CHECK(w.chunks[0].cx == 4096 && w.chunks[0].cy == 4096, "cx from x, cy from z");
    CHECK(eden_block(w.data(), w.chunks[0], 3, 7, 40) == 12,
          "lx from x, ly from z, file z from the server's height");
    CHECK(eden_paint(w.data(), w.chunks[0], 3, 7, 40) == 5, "paint follows the block");

    // And the import side agrees, which is the only thing that matters.
    const WorldStore back = import_to_store(r.bytes);
    CHECK(cells_of(back) == cells_of(s), "the rename is its own inverse");

    // Signs take the same swap. `eden_sign_to_file` and `eden_sign_to_server`
    // must compose to the identity.
    const Sign in{65540, 33, 65545, 1, 2, 3, "hello: world"};
    EdenSign f;
    CHECK(eden_sign_to_file(in, 63, f), "the sign converts");
    CHECK(f.x == 65540 && f.y == 65545 && f.z == 33, "sign x stays, y/z swap");
    Sign back_sign;
    CHECK(eden_sign_to_server(f, back_sign), "and converts back");
    CHECK(back_sign.x == in.x && back_sign.y == in.y && back_sign.z == in.z &&
              back_sign.a == 1 && back_sign.b == 2 && back_sign.c == 3 &&
              back_sign.text == in.text,
          "a sign survives the swap, a/b/c and a ':' in the text included");
}

// ── 4. height format ────────────────────────────────────────────────────────

static void test_height_format() {
    WorldStore low;
    low.set(65536, 63, 65536, 12, 0);
    ExportResult r;
    export_bytes(low, {}, named("low"), r);
    CHECK(r.z_ceiling == 63 && r.chunk_size == 32768 && r.z_format() == "64z",
          "nothing above 63 → 64z");
    CHECK(r.bytes.size() == 192 + 32768 + 16, "a one-chunk 64z file is header + chunk + row");
    CHECK(eden_load(r.bytes).chunk_size == 32768, "and re-detects as 64z");

    WorldStore high = low;
    high.set(65536, 64, 65536, 12, 0);
    ExportResult r2;
    export_bytes(high, {}, named("high"), r2);
    CHECK(r2.z_ceiling == 255 && r2.chunk_size == 131072 && r2.z_format() == "256z",
          "one cell above 63 → 256z");
    CHECK(eden_load(r2.bytes).chunk_size == 131072, "and re-detects as 256z");
    CHECK(r2.cells_dropped_height == 0, "nothing is dropped when the format grows");

    // A sign alone is enough to need the taller format.
    std::vector<Sign> tall_sign{{65536, 90, 65536, 0, 0, 0, "up here"}};
    ExportResult r3;
    export_bytes(low, tall_sign, named("signtall"), r3);
    CHECK(r3.z_ceiling == 255, "a sign above 63 forces 256z too");
    CHECK(r3.signs == 1 && r3.signs_dropped == 0, "and the sign is kept");

    // Forcing 64z on the tall world drops the tall cell and counts it.
    ExportOptions forced = named("forced");
    forced.force_z = 64;
    ExportResult r4;
    export_bytes(high, {}, forced, r4);
    CHECK(r4.z_ceiling == 63, "--z 64 forces the short format");
    CHECK(r4.cells_dropped_height == 1 && r4.cells == 1,
          "the out-of-range cell is dropped and counted, not wrapped");
    const WorldStore back = import_to_store(r4.bytes);
    CHECK(back.size() == 1, "and it is really gone, not folded to another height");

    // Forcing 256z on a short world keeps everything.
    ExportOptions tall = named("tall");
    tall.force_z = 256;
    ExportResult r5;
    export_bytes(low, {}, tall, r5);
    CHECK(r5.z_ceiling == 255 && r5.cells == 1 && r5.cells_dropped_height == 0,
          "--z 256 is always lossless");

    int z = -1;
    CHECK(eden_parse_z_format("auto", z) && z == 0, "--z auto parses");
    CHECK(eden_parse_z_format("64", z) && z == 64, "--z 64 parses");
    CHECK(eden_parse_z_format("256z", z) && z == 256, "--z 256z parses");
    CHECK(!eden_parse_z_format("128", z), "--z 128 does not parse");
}

// ── 5. drops: coordinates out of range ──────────────────────────────────────

static void test_coordinate_gate() {
    WorldStore s;
    s.set(65536, 40, 65536, 12, 0);                 // fine
    s.set(EDEN_CHUNK_COORD_LIMIT * 16, 40, 65536, 12, 0);   // cx == 32768: past the gate
    s.set(65536, 40, EDEN_CHUNK_COORD_LIMIT * 16 + 5, 12, 0);
    ExportResult r;
    export_bytes(s, {}, named("gate"), r);
    CHECK(r.cells == 1 && r.cells_dropped_coord == 2,
          "a chunk coordinate outside the directory gate is dropped and counted");
    CHECK(r.chunks == 1, "and contributes no chunk");
    const EdenWorld w = eden_load(r.bytes);
    CHECK(w.chunks.size() == 1, "the written directory holds only addressable chunks");

    // A sign that cannot be expressed goes the same way.
    std::vector<Sign> signs{{65540, 33, 65545, 0, 0, 0, "kept"},
                            {65540, 90, 65545, 0, 0, 0, "too high for 64z"}};
    ExportOptions o = named("gatesign");
    o.force_z = 64;
    ExportResult r2;
    export_bytes(s, signs, o, r2);
    CHECK(r2.signs == 1 && r2.signs_dropped == 1,
          "a sign above the ceiling is dropped and counted");
}

// ── 6. signs: sidecar and inline ────────────────────────────────────────────

static void test_signs() {
    WorldStore s;
    s.set(65540, 33, 65545, 12, 0);
    const std::vector<Sign> signs = sample_signs();

    ExportOptions side = named("sidecar");
    ExportResult r;
    export_bytes(s, signs, side, r);
    CHECK(r.signs == 2, "both signs are written");
    CHECK(!r.sign_sidecar.empty(), "a sidecar body is produced");
    CHECK(eden_load(r.bytes).signs.empty(), "and nothing is appended inside the .eden");
    const std::vector<EdenSign> parsed = eden_parse_signs(r.sign_sidecar);
    CHECK(parsed.size() == 2, "the sidecar parses back through eden_parse_signs");
    CHECK(parsed[0].text == "hello: world", "text with a ':' survives");
    CHECK(parsed[0].a == 1 && parsed[0].b == 2 && parsed[0].c == 3,
          "a/b/c pass through verbatim");

    ExportOptions inl = named("inline");
    inl.signs = SignMode::Inline;
    ExportResult r2;
    export_bytes(s, signs, inl, r2);
    CHECK(r2.sign_sidecar.empty(), "inline mode writes no sidecar");
    const EdenWorld w2 = eden_load(r2.bytes);
    CHECK(w2.signs.size() == 2, "the inline trailer parses back through eden_load");
    CHECK(w2.signs[0].text == "hello: world", "inline text survives too");
    CHECK(w2.chunks.size() == 1, "the trailer is not mistaken for a chunk row");

    ExportOptions none = named("nosigns");
    none.signs = SignMode::None;
    ExportResult r3;
    export_bytes(s, signs, none, r3);
    CHECK(r3.signs == 0 && r3.sign_sidecar.empty() && eden_load(r3.bytes).signs.empty(),
          "--signs none writes no signs anywhere");

    CHECK(eden_sidecar_path("out/carved.eden") == "out/signs_carved.eden.dat",
          "the sidecar name is the one eden_import's --signs default looks for");
    CHECK(eden_sidecar_path("carved.eden") == "signs_carved.eden.dat",
          "and works with no directory part");

    check_round_trip("signs-sidecar", s, signs, side);
    check_round_trip("signs-inline", s, signs, inl);
}

// ── 7. the empty world ──────────────────────────────────────────────────────

static void test_empty_world() {
    WorldStore s;
    ExportResult r;
    ExportOptions o = named("empty");
    o.spawn = {65536.0f, 33.92f, 65536.0f};
    o.have_spawn = true;
    export_bytes(s, {}, o, r);
    CHECK(r.seeded && r.chunks == 1 && r.cells == 0,
          "an empty world gets exactly one base-terrain chunk");
    const EdenWorld w = eden_load(r.bytes);
    CHECK(w.chunks.size() == 1 && w.chunks[0].cx == 4096 && w.chunks[0].cy == 4096,
          "anchored at the spawn's chunk");
    CHECK(import_to_store(r.bytes).size() == 0,
          "and re-imports as the empty world it was");
    check_round_trip("empty", s, {}, o);
}

// ── 8. the round-trip property, over the fixture worlds ─────────────────────

static void test_round_trip_property() {
    struct Case { const char* name; std::vector<uint8_t> raw; };
    std::vector<Case> cases;
    cases.push_back({"flat-64z", fixture_flat_64z()});
    cases.push_back({"carved-64z", fixture_carved_64z()});
    cases.push_back({"tall-256z", fixture_tall_256z()});

    for (const Case& c : cases) {
        const WorldStore s = import_to_store(c.raw);
        check_round_trip(c.name, s, sample_signs());

        // Full fidelity of the *rendered* world, the thing a player sees: import
        // the export and compare against importing the original.
        ExportResult r;
        ExportOptions o = named(c.name);
        export_bytes(s, {}, o, r);
        if (r.bytes.empty()) continue;
        const WorldStore back = import_to_store(r.bytes);
        CHECK(cells_of(back) == cells_of(s),
              (std::string(c.name) + ": import(export(import(f))) != import(f)").c_str());
    }

    // The one documented absorption: a delta that happens to *equal* the base
    // terrain is indistinguishable from no delta once it is in the file. It
    // renders identically, so the bytes still round-trip; only the cell count
    // moves. `docs/export.md` § What is lost.
    WorldStore s;
    s.set(65536, 5, 65536, EDEN_STONE, 0);   // exactly what the profile has at z 5
    ExportResult r;
    export_bytes(s, {}, named("absorbed"), r);
    CHECK(r.cells == 1, "the cell is written");
    CHECK(import_to_store(r.bytes).size() == 0, "and re-imports as no delta at all");
}

// ── 9. the ZIP wrapper, and the byte ceiling ────────────────────────────────

static void test_zip_and_limits() {
    WorldStore s;
    s.set(65536, 40, 65536, 12, 0);

    ExportOptions z = named("zipped");
    z.zip = true;
    ExportResult r;
    export_bytes(s, {}, z, r);
    CHECK(eden_is_zip(r.bytes.data(), r.bytes.size()), "--zip produces a ZIP container");
    CHECK(r.bytes.size() < r.uncompressed_bytes, "a base-terrain world compresses");
    const EdenWorld w = eden_load(r.bytes);
    CHECK(w.chunks.size() == 1, "and eden_load reads it straight back");
    CHECK(cells_of(import_to_store(r.bytes)) == cells_of(s), "with the same cell");
    check_round_trip("zipped", s, {}, z);

    // The ceiling is checked before anything is allocated, and refusing means
    // no bytes at all — the CLI writes nothing on a false return.
    ExportOptions tight = named("tight");
    tight.max_bytes = 1024;
    ExportResult r2;
    std::string err;
    CHECK(!eden_export_world(s, {}, tight, r2, err), "a world past --max-bytes is refused");
    CHECK(!err.empty(), "with a message");
    CHECK(r2.bytes.empty(), "and no bytes produced");
}

// ── 10. the CLI ─────────────────────────────────────────────────────────────

static bool file_exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

static std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static int run(const std::string& cmd, std::string& out) {
    const std::string full = cmd + " 2>/tmp/eden_export_test.err";
    FILE* p = popen(full.c_str(), "r");
    if (!p) return -1;
    char buf[4096];
    out.clear();
    while (std::fgets(buf, sizeof buf, p)) out += buf;
    const int rc = pclose(p);
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

static void test_cli() {
    if (!file_exists("./eden_export")) {
        std::fprintf(stderr, "SKIP: ./eden_export not built; run build_server.sh\n");
        return;
    }
    const std::string dir = "/tmp/eden_export_test_world";
    const std::string out = "/tmp/eden_export_test_out";
    std::string o;
    run("rm -rf " + dir + " " + out + " && mkdir -p " + dir + " " + out, o);

    // A world dir in the legacy text form, plus signs and a spawn — and an
    // eden_players.txt that must be left strictly alone.
    {
        std::ofstream m(dir + "/eden_world.model");
        m << "65536:40:65536:12:0\n65537:41:65536:12:3\n65536:5:65536:0:0\n";
        std::ofstream sg(dir + "/eden_signs.txt");
        sg << "65540:33:65545:1:2:3:hello: world\n";
        std::ofstream sp(dir + "/eden_spawn.txt");
        sp << "65540.00:33.92:65545.00\n";
        std::ofstream pl(dir + "/eden_players.txt");
        pl << "someone:65536.00:33.92:65536.00\n";
    }
    const std::string players_before = slurp(dir + "/eden_players.txt");

    // --out is required, and a usage error is exit 2.
    CHECK(run("./eden_export " + dir, o) == 2, "no --out is a usage error (exit 2)");
    CHECK(run("./eden_export --out " + out + "/x.eden", o) == 2,
          "no world directory is a usage error (exit 2)");
    CHECK(run("./eden_export --nope " + dir + " --out " + out + "/x.eden", o) == 2,
          "an unknown option is a usage error (exit 2)");
    CHECK(!file_exists(out + "/x.eden"), "and nothing is written");

    // A missing world is a refusal (exit 1) with no output file.
    CHECK(run("./eden_export /tmp/eden_export_test_nothere --out " + out + "/x.eden", o) == 1,
          "a missing world directory is a refusal (exit 1)");
    CHECK(!file_exists(out + "/x.eden"), "and writes no file");

    // --dry-run writes nothing.
    CHECK(run("./eden_export " + dir + " --out " + out + "/w.eden --dry-run", o) == 0,
          "--dry-run succeeds");
    CHECK(o.find("cells: 3") != std::string::npos,
          "the summary carries a grep-able `cells: N` line");
    CHECK(!file_exists(out + "/w.eden"), "--dry-run writes no .eden");
    CHECK(!file_exists(out + "/signs_w.eden.dat"), "--dry-run writes no sidecar");

    // The real thing.
    CHECK(run("./eden_export " + dir + " --out " + out + "/w.eden", o) == 0,
          "a real export succeeds");
    CHECK(file_exists(out + "/w.eden"), "the .eden lands");
    CHECK(file_exists(out + "/signs_w.eden.dat"),
          "the sidecar lands under the name eden_import expects");
    CHECK(!file_exists(out + "/w.eden.tmp"), "no temp file is left behind");
    CHECK(o.find("cells: 3") != std::string::npos, "`cells: N` again");
    CHECK(o.find("z-format: 64z") != std::string::npos, "the z format is named");
    CHECK(o.find("signs-mode: sidecar") != std::string::npos, "the sign mode is named");
    CHECK(slurp(dir + "/eden_players.txt") == players_before,
          "eden_players.txt is never touched");

    // Refuses to overwrite without --force, and writes nothing when it refuses.
    const std::string before = slurp(out + "/w.eden");
    CHECK(run("./eden_export " + dir + " --out " + out + "/w.eden", o) == 1,
          "an existing output file is a refusal (exit 1)");
    CHECK(slurp(out + "/w.eden") == before, "and the existing file is untouched");
    CHECK(run("./eden_export " + dir + " --out " + out + "/w.eden --force", o) == 0,
          "--force overwrites");
    CHECK(slurp(out + "/w.eden") == before, "deterministically, to the same bytes");

    // And the file the CLI wrote imports back to the world it came from.
    const std::vector<uint8_t> raw = [&] {
        const std::string b = slurp(out + "/w.eden");
        return std::vector<uint8_t>(b.begin(), b.end());
    }();
    const WorldStore back = import_to_store(raw);
    CHECK(back.size() == 3, "the CLI's own output round-trips to three cells");

    run("rm -rf " + dir + " " + out, o);
}

// ── 11. header fidelity: the origin sidecar (stage 5.6) ─────────────────────

static bool same_f3(const float* a, const float* b) { return std::memcmp(a, b, 12) == 0; }

/// A world whose header carries values no default would reproduce: negative
/// seed, awkward floats, a `home` unlike `pos`, and a non-zero sky palette.
static std::vector<uint8_t> fixture_headered(int32_t version, bool tall) {
    WorldSpec w;
    w.name = "Headered";
    w.version = version;
    w.seed = -77;
    w.yaw = -176.79372f;
    w.pos[0] = 65605.046875f; w.pos[1] = 33.925f; w.pos[2] = 64117.457f;
    w.home[0] = 65476.168f;   w.home[1] = 34.925f; w.home[2] = 64377.152f;
    if (tall) {
        w.chunks.push_back({4096, 4096, build_chunk(16, [](int lx, int ly, int z) -> Voxel {
                                if (lx == 2 && ly == 2 && z == 200) return {12, 0};
                                return base_profile(z);
                            }), 0});
    } else {
        w.chunks.push_back({4096, 4096, build_chunk(4, [](int lx, int ly, int z) -> Voxel {
                                if (lx == 8 && ly == 8 && z == 40) return {12, 3};
                                return base_profile(z);
                            }), 0});
    }
    std::vector<uint8_t> b = build_world(w);
    for (int i = 0; i < 16; ++i) b[size_t(132 + i)] = uint8_t(10 + i);   // sky @132..148
    return b;
}

static void check_header_equal(const char* what, const EdenHeader& a, const EdenHeader& b) {
    const std::string w(what);
    CHECK(a.seed == b.seed, (w + ": seed").c_str());
    CHECK(same_f3(a.pos, b.pos), (w + ": pos is bit-identical").c_str());
    CHECK(same_f3(a.home, b.home), (w + ": home is bit-identical").c_str());
    CHECK(std::memcmp(&a.yaw, &b.yaw, 4) == 0, (w + ": yaw is bit-identical").c_str());
    CHECK(a.version == b.version, (w + ": version").c_str());
    CHECK(a.name == b.name, (w + ": name").c_str());
    CHECK(std::memcmp(a.skycolors, b.skycolors, 16) == 0, (w + ": sky palette").c_str());
}

static void test_origin_grammar() {
    const EdenWorld src = eden_load(fixture_headered(4, false));
    const std::string text = eden_format_origin(src.hdr, src.z_ceiling);

    EdenOrigin o;
    std::vector<std::string> bad;
    eden_parse_origin(text, o, bad);
    CHECK(bad.empty(), "the file the writer produces parses without a complaint");
    CHECK(o.seed && *o.seed == -77, "seed round-trips");
    CHECK(o.yaw && std::memcmp(&*o.yaw, &src.hdr.yaw, 4) == 0, "yaw round-trips bit-for-bit");
    CHECK(o.pos && same_f3(o.pos->data(), src.hdr.pos), "pos round-trips bit-for-bit");
    CHECK(o.home && same_f3(o.home->data(), src.hdr.home), "home round-trips bit-for-bit");
    CHECK(o.version && *o.version == 4, "version round-trips");
    CHECK(o.z && *o.z == 64, "the height format is recorded as 64");
    CHECK(o.sky && std::memcmp(o.sky->data(), src.hdr.skycolors, 16) == 0, "sky round-trips");
    CHECK(eden_format_origin(src.hdr, 255).find("z: 256\n") != std::string::npos,
          "a 256z source records 256");

    // Hand-edited: comments, blanks, CRLF, an unknown key, a later duplicate.
    EdenOrigin h;
    bad.clear();
    eden_parse_origin("# note\r\n\r\nseed: 5\r\nseed: 9\r\nfuture-key: whatever\r\nz: 256\r\n", h, bad);
    CHECK(bad.empty(), "comments, blanks, CRLF and unknown keys are not errors");
    CHECK(h.seed && *h.seed == 9, "a later duplicate wins");
    CHECK(h.z && *h.z == 256, "z: 256 parses");
    CHECK(!h.yaw && !h.pos && !h.home && !h.sky && !h.version, "absent keys stay absent");

    // Malformed known keys are named and skipped; their neighbours still apply.
    EdenOrigin m;
    bad.clear();
    eden_parse_origin("seed: 4x\nyaw: nan\npos: 1:2\nhome: 1:2:3:4\nz: 128\nversion: 4.5\n"
                      "sky: 1 2 3\nsky: 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 300\nno colon here\n"
                      "yaw: 2.5\n", m, bad);
    CHECK(bad.size() == 9, "every malformed line is reported");
    CHECK(!m.seed && !m.pos && !m.home && !m.z && !m.version && !m.sky,
          "and none is applied half-way");
    CHECK(m.yaw && *m.yaw == 2.5f, "a good line after bad ones still applies");
}

static void test_origin_round_trip() {
    // The exit criterion for the header: every parsed field survives
    // import -> origin -> export bit-for-bit, for 64z and 256z sources.
    struct Case { const char* what; int32_t version; bool tall; };
    const Case cases[] = {{"64z", 4, false}, {"256z", 5, true}, {"256z, odd version", 7, true}};
    for (const Case& c : cases) {
        const std::vector<uint8_t> raw = fixture_headered(c.version, c.tall);
        const EdenWorld src = eden_load(raw);
        const WorldStore s = import_to_store(raw);

        EdenOrigin o;
        std::vector<std::string> bad;
        eden_parse_origin(eden_format_origin(src.hdr, src.z_ceiling), o, bad);

        // import wrote eden_spawn.txt from `pos` at two decimals; export reads it back.
        ExportOptions opt = named("Headered");
        opt.spawn = eden_spawn_from(src.hdr, false);
        std::string line = format_spawn_line(opt.spawn);
        Spawn from_file;
        CHECK(parse_spawn_line(line, from_file), "the spawn line parses");
        opt.spawn = from_file;
        opt.have_spawn = true;
        CHECK(!same_f3(&opt.spawn.x, src.hdr.pos), "the two-decimal spawn file alone is lossy");

        CHECK(eden_apply_origin(o, opt, false, false), "the exact pos supplies the spawn");
        ExportResult r;
        const std::vector<uint8_t> out = export_bytes(s, {}, opt, r);
        check_header_equal(c.what, src.hdr, eden_load(out).hdr);
        CHECK(eden_load(out).chunk_size == src.chunk_size, "and the height format matches");
    }

    // No origin at all is exactly the 5.5 behaviour: defaults, home mirrors pos.
    const std::vector<uint8_t> raw = fixture_headered(4, false);
    ExportResult r;
    ExportOptions opt = named("Headered");
    opt.spawn = {65540.0f, 33.92f, 65545.0f};
    opt.have_spawn = true;
    const EdenHeader h = eden_load(export_bytes(import_to_store(raw), {}, opt, r)).hdr;
    CHECK(h.seed == 0 && h.yaw == 0.0f, "no origin: seed and yaw are the defaults");
    CHECK(same_f3(h.home, h.pos), "no origin: home mirrors pos");
    CHECK(h.version == 4, "no origin: version is derived");
}

static void test_origin_precedence() {
    const EdenWorld src = eden_load(fixture_headered(4, false));
    EdenOrigin o;
    std::vector<std::string> bad;
    eden_parse_origin(eden_format_origin(src.hdr, src.z_ceiling), o, bad);

    // Explicit flags beat the file.
    ExportOptions opt;
    opt.seed = 1234;
    opt.yaw = 0.5f;
    eden_apply_origin(o, opt, true, true);
    CHECK(opt.seed == 1234 && opt.yaw == 0.5f, "--seed / --yaw win over the origin");

    // A spawn file the operator has moved beats the origin's pos.
    ExportOptions moved;
    moved.spawn = {70000.0f, 33.92f, 70000.0f};
    moved.have_spawn = true;
    CHECK(!eden_apply_origin(o, moved, false, false), "a moved spawn is not overridden");
    CHECK(moved.spawn.x == 70000.0f, "and stays where the operator put it");
    CHECK(moved.have_home && same_f3(moved.home, src.hdr.home),
          "though home still replays from the origin");

    // No spawn file at all: the origin supplies one (import ran with --spawn none).
    ExportOptions bare;
    CHECK(eden_apply_origin(o, bare, false, false) && bare.have_spawn &&
              same_f3(&bare.spawn.x, src.hdr.pos),
          "with no spawn file, the origin's pos is the spawn");

    // A world with an empty origin is untouched.
    ExportOptions untouched;
    EdenOrigin none;
    CHECK(!eden_apply_origin(none, untouched, false, false) && !untouched.have_spawn &&
              !untouched.have_home && untouched.version == 0 && untouched.z_hint == 0,
          "an empty origin changes nothing");
}

static void test_origin_height_format() {
    // A world that was 256z but has had everything above 63 mined away.
    WorldStore low;
    low.set(65540, 40, 65540, 12, 0);
    ExportOptions opt = named("Was tall");
    opt.z_hint = 256;
    opt.version = 7;
    ExportResult r;
    const std::vector<uint8_t> b = export_bytes(low, {}, opt, r);
    CHECK(r.z_ceiling == 255, "z_hint 256 keeps a now-low world 256z");
    CHECK(eden_load(b).hdr.version == 7, "and its recorded version is replayed");

    // --z 64 wins over the hint, and a version that would lie is not replayed.
    opt.force_z = 64;
    ExportResult r2;
    const std::vector<uint8_t> b2 = export_bytes(low, {}, opt, r2);
    CHECK(r2.z_ceiling == 63, "--z 64 beats the hint");
    CHECK(eden_load(b2).hdr.version == 4, "a 256z version is not written into a 64z file");

    // The hint never lowers the ceiling.
    WorldStore tall;
    tall.set(65540, 200, 65540, 12, 0);
    ExportOptions opt64 = named("Grew");
    opt64.z_hint = 64;
    opt64.version = 4;
    ExportResult r3;
    export_bytes(tall, {}, opt64, r3);
    CHECK(r3.z_ceiling == 255, "a 64z hint does not stop a world that has grown tall");
}

static void test_origin_cli() {
    if (!file_exists("./eden_export") || !file_exists("./eden_import")) {
        std::fprintf(stderr, "SKIP: ./eden_export / ./eden_import not built; run build_server.sh\n");
        return;
    }
    const std::string root = "/tmp/eden_export_test_origin";
    std::string o;
    run("rm -rf " + root + " && mkdir -p " + root, o);

    const std::vector<uint8_t> raw = fixture_headered(4, false);
    {
        std::ofstream f(root + "/src.eden", std::ios::binary);
        f.write(reinterpret_cast<const char*>(raw.data()), std::streamsize(raw.size()));
    }
    const EdenHeader src = eden_load(raw).hdr;

    // The real path: eden_import writes the world dir, eden_export reads it back.
    CHECK(run("./eden_import " + root + "/src.eden --out " + root + "/world --force -y", o) == 0,
          "eden_import succeeds");
    CHECK(file_exists(root + "/world/eden_origin.txt"), "eden_import writes eden_origin.txt");
    CHECK(o.find("eden_origin.txt") != std::string::npos, "and says so in its summary");

    CHECK(run("./eden_export " + root + "/world --out " + root + "/back.eden --name Headered", o) == 0,
          "eden_export succeeds");
    CHECK(o.find("origin: " + root + "/world/eden_origin.txt") != std::string::npos,
          "the summary names the origin file it used");
    const std::string back = slurp(root + "/back.eden");
    check_header_equal("CLI round trip", src,
                       eden_parse_header(reinterpret_cast<const uint8_t*>(back.data()), back.size()));

    // --origin none writes defaults.
    CHECK(run("./eden_export " + root + "/world --out " + root + "/plain.eden --name Headered "
              "--origin none", o) == 0, "--origin none succeeds");
    CHECK(o.find("origin: none") != std::string::npos, "and the summary says none");
    const std::string plain = slurp(root + "/plain.eden");
    const EdenHeader ph = eden_parse_header(reinterpret_cast<const uint8_t*>(plain.data()), plain.size());
    CHECK(ph.seed == 0 && ph.yaw == 0.0f && ph.skycolors[0] == 0, "and the header carries defaults");

    // --seed beats the file; a named origin that is missing is a refusal.
    CHECK(run("./eden_export " + root + "/world --out " + root + "/seed.eden --seed 42", o) == 0,
          "--seed with an origin succeeds");
    const std::string sd = slurp(root + "/seed.eden");
    CHECK(eden_parse_header(reinterpret_cast<const uint8_t*>(sd.data()), sd.size()).seed == 42,
          "--seed wins over the origin");
    CHECK(run("./eden_export " + root + "/world --out " + root + "/x.eden --origin /nope", o) == 1,
          "a named origin file that is missing is a refusal (exit 1)");
    CHECK(!file_exists(root + "/x.eden"), "and writes nothing");

    // A malformed line warns on stderr and does not stop the export.
    {
        std::ofstream f(root + "/world/eden_origin.txt", std::ios::app);
        f << "yaw: banana\n";
    }
    CHECK(run("./eden_export " + root + "/world --out " + root + "/warn.eden", o) == 0,
          "a malformed origin line does not fail the export");
    CHECK(slurp("/tmp/eden_export_test.err").find("banana") != std::string::npos,
          "but it is named on stderr");

    run("rm -rf " + root, o);
}

int main() {
    test_sentinels();
    test_chunk_selection();
    test_axis_rename();
    test_height_format();
    test_coordinate_gate();
    test_signs();
    test_empty_world();
    test_round_trip_property();
    test_zip_and_limits();
    test_cli();
    test_origin_grammar();
    test_origin_round_trip();
    test_origin_precedence();
    test_origin_height_format();
    test_origin_cli();

    if (g_fail) {
        std::fprintf(stderr, "eden_export_test: %d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("eden_export_test: all checks passed\n");
    return 0;
}
