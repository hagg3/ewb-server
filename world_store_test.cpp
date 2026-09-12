// world_store_test.cpp — offline checks for ROADMAP-SERVER stage 7.6 (the chunked
// world store, its EDMB format and the legacy text fallback).
//
//   clang++ -std=c++17 -O2 -Wall world_store_test.cpp -lz -o world_store_test
//   ./world_store_test
//
// The point of this suite is the **new invariant**, not the container. The old
// model was a sparse `unordered_map` where absence meant "no delta" and a present
// cell with type 0 meant "explicitly mined"; a dense `type[4096]` cannot say both
// with the value 0, so `world_store.h` reserves `CELL_MINED` (254) for mined and
// gives 0 back to "absent". Every check below exists because something outside
// the header — `emit_cell_records()`, `eden_import`'s emitters, the save file —
// would break if that mapping leaked.
//
// Covers, without a socket, a lock or a world file:
//   * absent vs mined vs typed, and that the 254 sentinel never escapes an accessor
//   * a differential check against a reference `unordered_map` model: same cells,
//     same logical types, same colours, over a randomised edit stream
//   * the same differential through `emit_cell_records()`, so the *wire* output of
//     the two models is record-for-record identical (the regression that matters)
//   * box scans: same cells as a brute-force filter over the reference model, and
//     materially fewer slots visited than the world has cells
//   * EDMB round-trip, byte determinism, magic sniffing, and rejection of a
//     truncated / over-claiming / wrong-version file
//   * the legacy text reader: field rules, 4-field lines, junk lines
//   * a real shipped world (`testdata/carved_64z.model`) loading through the
//     legacy path and surviving an EDMB round-trip unchanged

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "region_query.h"
#include "world_store.h"

static int g_fail = 0;

#define CHECK(cond, msg)                                                             \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__);    \
            ++g_fail;                                                                \
        }                                                                            \
    } while (0)

// The model 7.6 replaces, kept here as the differential oracle.
struct RefCell { unsigned char type, color; };
static uint64_t refkey(int x, int y, int z) {
    return (((uint64_t)(uint32_t)x & 0xFFFFFFull) << 40)
         | (((uint64_t)(uint32_t)z & 0xFFFFFFull) << 16)
         |  ((uint64_t)(uint32_t)y & 0xFFFFull);
}
using RefWorld = std::unordered_map<uint64_t, RefCell>;

// --- the invariant -----------------------------------------------------------

static void test_three_states() {
    ewb::WorldStore w;
    ewb::WorldCell c;

    CHECK(!w.get(10, 20, 30, c), "an untouched cell is absent");
    CHECK(!w.contains(10, 20, 30), "an untouched cell is not contained");
    CHECK(w.size() == 0, "an untouched cell costs nothing");

    // Explicitly mined: present, and it must read back as logical air (0), which
    // is what every `cell.type == SV_AIR` test in the server relies on.
    w.set(10, 20, 30, 0, 0);
    CHECK(w.contains(10, 20, 30), "a mined cell is present");
    CHECK(w.get(10, 20, 30, c) && c.type == 0, "a mined cell reads back as logical air");
    CHECK(c.type != ewb::CELL_MINED, "the mined sentinel never escapes get()");
    CHECK(w.size() == 1, "a mined cell counts against the cell cap");

    // A neighbour in the same chunk stays absent — the dense array's default is
    // absence, not air.
    CHECK(!w.contains(11, 20, 30), "a neighbour in the same chunk is still absent");

    w.set(11, 20, 30, 7, 3);
    CHECK(w.get(11, 20, 30, c) && c.type == 7 && c.color == 3, "a typed cell round-trips");
    w.set(11, 20, 30, 0, 0);
    CHECK(w.get(11, 20, 30, c) && c.type == 0, "mining a placed block leaves logical air");
    CHECK(w.size() == 2, "overwriting a cell does not double-count it");

    // 255 (painted base) is a real stored type and must survive verbatim.
    w.set(12, 20, 30, ewb::CELL_PAINTED_BASE, 9);
    CHECK(w.get(12, 20, 30, c) && c.type == 255 && c.color == 9, "painted-base 255 is stored verbatim");

    // 254 is reserved by the store. It cannot come from ACTION (types cap at 127)
    // or from paint (255); a legacy file carrying one is loaded as air and counted.
    w.set(13, 20, 30, ewb::CELL_MINED, 0);
    CHECK(w.reserved_coerced() == 1, "a reserved-254 cell is counted, not silent");
    CHECK(w.get(13, 20, 30, c) && c.type == 0, "a reserved-254 cell loads as air");

    size_t sentinels = 0;
    w.for_each([&](int, int, int, unsigned char t, unsigned char) {
        if (t == ewb::CELL_MINED) ++sentinels;
    });
    CHECK(sentinels == 0, "for_each never yields the sentinel");
}

// --- differential against the sparse model -----------------------------------

// A randomised edit stream in the shape the server actually produces: builds,
// mines, paints and painted-base cells, clustered so chunks are shared.
static void build_pair(RefWorld& ref, ewb::WorldStore& w, unsigned seed, int n) {
    std::mt19937 rng(seed);
    for (int i = 0; i < n; ++i) {
        const int x = 65400 + (int)(rng() % 300);
        const int z = 65400 + (int)(rng() % 300);
        const int y = 20 + (int)(rng() % 60);
        unsigned char type, color = 0;
        switch (rng() % 4) {
            case 0: type = 0; break;                                   // mined
            case 1: type = (unsigned char)(1 + rng() % 127); break;    // placed
            case 2: type = (unsigned char)(1 + rng() % 127);
                    color = (unsigned char)(1 + rng() % 54); break;    // placed + painted
            default: type = ewb::CELL_PAINTED_BASE;
                     color = (unsigned char)(1 + rng() % 54); break;   // painted base
        }
        ref[refkey(x, y, z)] = {type, color};
        w.set(x, y, z, type, color);
    }
}

static void test_differential_contents() {
    RefWorld ref; ewb::WorldStore w;
    build_pair(ref, w, 1234, 40000);

    CHECK(w.size() == ref.size(), "store and sparse map hold the same cell count");

    size_t seen = 0;
    bool same = true;
    w.for_each([&](int x, int y, int z, unsigned char t, unsigned char col) {
        ++seen;
        auto it = ref.find(refkey(x, y, z));
        if (it == ref.end() || it->second.type != t || it->second.color != col) same = false;
    });
    CHECK(seen == ref.size(), "for_each visits every cell exactly once");
    CHECK(same, "every cell matches the sparse model, type and colour");

    // ...and the other direction, which is what catches an absent/air mix-up.
    for (const auto& kv : ref) {
        const int x = (int)((kv.first >> 40) & 0xFFFFFF);
        const int z = (int)((kv.first >> 16) & 0xFFFFFF);
        const int y = (int)(kv.first & 0xFFFF);
        ewb::WorldCell c;
        if (!w.get(x, y, z, c) || c.type != kv.second.type || c.color != kv.second.color) {
            same = false; break;
        }
    }
    CHECK(same, "every sparse-model cell is present in the store");
}

// The regression that matters: the *wire* must be identical. `emit_cell_records`
// turns a cell into SNAPZ records and branches on `type == 0` meaning air, so an
// absent/mined confusion shows up here as a missing or extra `flag 1` record.
static void test_differential_wire() {
    RefWorld ref; ewb::WorldStore w;
    build_pair(ref, w, 99, 30000);
    // ...plus a second cluster far outside the reply box, so the box scan has
    // chunks it must *not* visit (the whole point of the index).
    {
        std::mt19937 rng(4242);
        for (int i = 0; i < 5000; ++i) {
            const int x = 70000 + (int)(rng() % 300);
            const int z = 70000 + (int)(rng() % 300);
            const int y = 20 + (int)(rng() % 60);
            const unsigned char t = (unsigned char)(rng() % 2 ? 0 : 1 + rng() % 127);
            ref[refkey(x, y, z)] = {t, 0};
            w.set(x, y, z, t, 0);
        }
    }

    const ewb::RegionBox box = ewb::region_box(65500, 65500);

    std::vector<ewb::SnapRec> fromRef, fromStore;
    for (const auto& kv : ref) {
        const int x = (int)((kv.first >> 40) & 0xFFFFFF);
        const int z = (int)((kv.first >> 16) & 0xFFFFFF);
        const int y = (int)(kv.first & 0xFFFF);
        if (!box.contains(x, z)) continue;
        ewb::emit_cell_records(x, y, z, kv.second.type, kv.second.color, fromRef);
    }
    const auto st = w.for_each_in_box(box.x0, box.x1, box.z0, box.z1,
                                      [&](int x, int y, int z, unsigned char t, unsigned char c) {
        ewb::emit_cell_records(x, y, z, t, c, fromStore);
    });

    ewb::sort_records(fromRef);
    ewb::sort_records(fromStore);
    CHECK(fromRef.size() == fromStore.size(), "box scan emits the same record count as the map scan");
    bool same = fromRef.size() == fromStore.size();
    for (size_t i = 0; same && i < fromRef.size(); ++i) {
        const ewb::SnapRec& a = fromRef[i]; const ewb::SnapRec& b = fromStore[i];
        if (a.x != b.x || a.y != b.y || a.z != b.z || a.flag != b.flag || a.type != b.type)
            same = false;
    }
    CHECK(same, "box scan emits byte-identical SNAPZ records to the full-world scan");
    CHECK(st.cells_emitted > 0, "the box actually covered some cells");

    // The whole reason for the chunk store: a box scan must look at far fewer
    // slots than the world has cells (this world is deliberately spread wider
    // than one reply box).
    CHECK(st.chunks_visited < st.chunks_total, "a box visits only some chunks");
}

static void test_box_edges() {
    ewb::WorldStore w;
    // One cell on each side of a chunk boundary, so a half-covered chunk has to
    // be filtered per cell rather than accepted wholesale.
    w.set(1599, 40, 1600, 5, 0);
    w.set(1600, 40, 1600, 6, 0);
    w.set(1615, 40, 1600, 7, 0);
    w.set(1616, 40, 1600, 8, 0);

    std::vector<int> got;
    w.for_each_in_box(1600, 1615, 1600, 1600, [&](int x, int, int, unsigned char t, unsigned char) {
        got.push_back((int)t); (void)x;
    });
    CHECK(got.size() == 2, "a box on exactly one chunk takes only that chunk's cells");
    bool ok = got.size() == 2 && ((got[0] == 6 && got[1] == 7) || (got[0] == 7 && got[1] == 6));
    CHECK(ok, "the two in-box cells are the right ones");

    // A box of one block.
    size_t n = 0;
    w.for_each_in_box(1600, 1600, 1600, 1600, [&](int, int, int, unsigned char, unsigned char) { ++n; });
    CHECK(n == 1, "a one-block box takes one cell");

    // A mined cell inside a box is emitted, exactly as a present type-0 cell was.
    w.set(1601, 40, 1600, 0, 0);
    n = 0;
    w.for_each_in_box(1601, 1601, 1600, 1600, [&](int, int, int, unsigned char t, unsigned char) {
        if (t == 0) ++n;
    });
    CHECK(n == 1, "a mined cell inside the box is emitted as logical air");
}

// --- EDMB --------------------------------------------------------------------

static bool stores_equal(const ewb::WorldStore& a, const ewb::WorldStore& b) {
    if (a.size() != b.size()) return false;
    bool same = true;
    a.for_each([&](int x, int y, int z, unsigned char t, unsigned char c) {
        ewb::WorldCell o;
        if (!b.get(x, y, z, o) || o.type != t || o.color != c) same = false;
    });
    return same;
}

static void test_edmb_round_trip() {
    RefWorld ref; ewb::WorldStore w;
    build_pair(ref, w, 7, 20000);
    w.set(65536, 0, 65536, 0, 0);         // a mined cell at y = 0, the awkward one

    const std::string blob = w.to_edmb();
    CHECK(ewb::WorldStore::is_edmb(blob.data(), blob.size()), "a save is sniffable as EDMB");
    CHECK(blob.compare(0, 4, "EDMB") == 0, "the magic is first");
    CHECK(blob == w.to_edmb(), "serialising twice gives identical bytes");

    ewb::WorldStore back;
    std::string err;
    CHECK(back.load_edmb(blob.data(), blob.size(), err), "a save loads back");
    CHECK(err.empty(), "a clean load reports no error");
    CHECK(stores_equal(w, back), "EDMB round-trips every cell, type and colour");
    CHECK(back.reserved_coerced() == 0, "a round-trip coerces nothing");

    // Cheaper than the text format it replaces: ~4 B/cell + 16 B/chunk.
    CHECK(blob.size() < w.size() * 8, "EDMB is under 8 bytes per cell");
    std::printf("  EDMB: %zu cells, %zu chunks -> %zu bytes (%.2f B/cell)\n",
                w.size(), w.chunk_count(), blob.size(), double(blob.size()) / double(w.size()));
}

static void test_edmb_rejections() {
    ewb::WorldStore w;
    w.set(100, 40, 100, 5, 0);
    const std::string blob = w.to_edmb();
    std::string err;

    ewb::WorldStore t1;
    CHECK(!t1.load_edmb("NOPE1234", 8, err), "a non-EDMB blob is refused");
    CHECK(!ewb::WorldStore::is_edmb("NOPE", 4), "the sniff says no to the legacy format");
    CHECK(!ewb::WorldStore::is_edmb("ED", 2), "the sniff says no to a 2-byte file");

    std::string badver = blob;
    badver[4] = (char)9;
    ewb::WorldStore t2;
    CHECK(!t2.load_edmb(badver.data(), badver.size(), err), "an unknown version is refused");

    for (size_t cut = 4; cut < blob.size(); ++cut) {
        ewb::WorldStore t;
        std::string e;
        CHECK(!t.load_edmb(blob.data(), cut, e) || t.size() <= w.size(),
              "a truncated file is refused or partial, never out of bounds");
    }

    // A header claiming more chunks than the file could hold must not make us
    // allocate or read past the end.
    std::string liar = blob;
    for (int i = 0; i < 8; ++i) liar[8 + i] = (char)0xFF;
    ewb::WorldStore t3;
    CHECK(!t3.load_edmb(liar.data(), liar.size(), err), "an over-claiming chunk count is refused");
}

// --- the legacy text reader --------------------------------------------------

static void test_legacy_text() {
    const std::string text =
        "100:40:200:5:0\n"
        "101:40:200:0:0\n"          // explicitly mined
        "102:40:200:255:7\n"        // painted base
        "103:40:200:9\n"            // 4 fields: colour defaults to 0
        "\n"                        // blank
        "garbage\n"                 // < 4 fields: skipped
        "104:40:200:11:2:99\n";     // extra field ignored
    std::istringstream in(text);
    ewb::WorldStore w;
    const size_t n = ewb::world_load_text(in, w);
    CHECK(n == 5, "five well-formed lines are taken");
    CHECK(w.size() == 5, "and each becomes one cell");

    ewb::WorldCell c;
    CHECK(w.get(100, 40, 200, c) && c.type == 5 && c.color == 0, "a placed block loads");
    CHECK(w.get(101, 40, 200, c) && c.type == 0, "a mined cell loads as present logical air");
    CHECK(w.contains(101, 40, 200), "a mined cell is present, not absent");
    CHECK(w.get(102, 40, 200, c) && c.type == 255 && c.color == 7, "a painted base block loads");
    CHECK(w.get(103, 40, 200, c) && c.type == 9 && c.color == 0, "a 4-field line defaults colour to 0");
    CHECK(w.get(104, 40, 200, c) && c.type == 11 && c.color == 2, "a 6-field line ignores the extra");
    CHECK(!w.contains(105, 40, 200), "nothing else was invented");
}

// --- a real shipped world ----------------------------------------------------

// The critical regression check: a world this repo actually ships must load
// through the legacy reader into exactly the cells the sparse map would have
// held, and survive an EDMB save/load unchanged.
static void test_shipped_world(const char* path) {
    std::ifstream f(path);
    if (!f) { std::printf("  (skipped: %s not found)\n", path); return; }

    RefWorld ref;
    {   // the pre-7.6 loader, verbatim
        std::ifstream g(path);
        std::string line;
        while (std::getline(g, line)) {
            if (line.empty()) continue;
            int v[5] = {0, 0, 0, 0, 0}, n = 0;
            std::stringstream ss(line); std::string p;
            while (n < 5 && std::getline(ss, p, ':')) v[n++] = atoi(p.c_str());
            if (n >= 4) ref[refkey(v[0], v[1], v[2])] = {(unsigned char)v[3], (unsigned char)v[4]};
        }
    }

    ewb::WorldStore w;
    ewb::world_load_text(f, w);
    CHECK(w.size() == ref.size(), "the shipped world loads to the same cell count");
    CHECK(w.reserved_coerced() == 0, "the shipped world has no reserved-254 cells");

    size_t airCells = 0;
    bool same = true;
    for (const auto& kv : ref) {
        const int x = (int)((kv.first >> 40) & 0xFFFFFF);
        const int z = (int)((kv.first >> 16) & 0xFFFFFF);
        const int y = (int)(kv.first & 0xFFFF);
        ewb::WorldCell c;
        if (!w.get(x, y, z, c) || c.type != kv.second.type || c.color != kv.second.color) same = false;
        if (kv.second.type == 0) ++airCells;
    }
    CHECK(same, "every cell of the shipped world matches the sparse model");

    // Same wire output, whole-world, for both models.
    std::vector<ewb::SnapRec> a, b;
    for (const auto& kv : ref) {
        const int x = (int)((kv.first >> 40) & 0xFFFFFF);
        const int z = (int)((kv.first >> 16) & 0xFFFFFF);
        const int y = (int)(kv.first & 0xFFFF);
        ewb::emit_cell_records(x, y, z, kv.second.type, kv.second.color, a);
    }
    w.for_each([&](int x, int y, int z, unsigned char t, unsigned char c) {
        ewb::emit_cell_records(x, y, z, t, c, b);
    });
    ewb::sort_records(a); ewb::sort_records(b);
    CHECK(a.size() == b.size(), "the shipped world emits the same record count");
    bool wire = a.size() == b.size();
    for (size_t i = 0; wire && i < a.size(); ++i)
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z ||
            a[i].flag != b[i].flag || a[i].type != b[i].type) wire = false;
    CHECK(wire, "the shipped world emits identical SNAPZ records under the chunk store");

    const std::string blob = w.to_edmb();
    ewb::WorldStore back;
    std::string err;
    CHECK(back.load_edmb(blob.data(), blob.size(), err), "the shipped world saves and reloads as EDMB");
    CHECK(stores_equal(w, back), "the EDMB round-trip changes nothing");
    std::printf("  %s: %zu cells (%zu explicitly mined), %zu chunks, EDMB %zu B\n",
                path, w.size(), airCells, w.chunk_count(), blob.size());
}

int main() {
    test_three_states();
    test_differential_contents();
    test_differential_wire();
    test_box_edges();
    test_edmb_round_trip();
    test_edmb_rejections();
    test_legacy_text();
    test_shipped_world("testdata/carved_64z.model");
    test_shipped_world("worlds/ari/eden_world.model");
    if (g_fail) {
        std::fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
