// region_cache_test.cpp — offline checks for ROADMAP-SERVER stage 12.1b, the
// encoded-region cache (`region_cache.h`).
//
//   clang++ -std=c++17 -O2 -Wall region_cache_test.cpp -lz -o region_cache_test
//   ./region_cache_test
//
// Covers, with no socket, no threads and no world lock:
//   * the key: box corners + the empty-frame switch, nothing else
//   * column generations: an edit inside the box, on each of its four edges, and
//     one block outside each edge; the 4096-block tile fold (a spurious miss, never
//     a stale hit); bump_all
//   * the stamp race: an edit between the scan and the insert — the entry is
//     filed, but the first lookup finds it stale and drops it, so it is never served
//   * LRU by bytes: hits refresh recency, the least recently used goes first, the
//     byte total is exact through inserts, replacements, evictions and stale drops
//   * the oversize rule (> 1/4 of the bound is never stored), off (0 bytes) stores
//     nothing, and an older scan never replaces a newer one
//   * end to end against a real WorldStore: a hit is byte-identical to a fresh
//     scan + encode, and after an edit in the box the next answer is fresh and
//     carries the edit

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "region_cache.h"
#include "region_query.h"
#include "snapz_codec.h"
#include "world_store.h"

using ewb::CachedRegion;
using ewb::ColumnGens;
using ewb::RegionBox;
using ewb::RegionCache;
using ewb::RegionCacheKey;

static int g_fail = 0;

#define CHECK(cond, msg)                                                             \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__);    \
            ++g_fail;                                                                \
        }                                                                            \
    } while (0)

static std::shared_ptr<const CachedRegion> entry(size_t bytes, uint64_t stamp, size_t records = 1) {
    auto e = std::make_shared<CachedRegion>();
    e->add(std::string(bytes, 'x'), records);
    e->stamp = stamp;
    return e;
}

static RegionCacheKey key_at(int cx, int cz, bool empty = true) {
    return RegionCacheKey::of(ewb::region_box(cx, cz), empty);
}

static void test_key() {
    const RegionBox b = ewb::region_box(65536, 65536);
    CHECK(RegionCacheKey::of(b, true) == RegionCacheKey::of(b, true), "same box, same key");
    CHECK(!(RegionCacheKey::of(b, true) == RegionCacheKey::of(b, false)), "empty-frame switch is in the key");
    // Request points that snap to the same chunk-aligned box share an entry.
    CHECK(key_at(65536, 65536) == key_at(65540, 65551), "points in one chunk share a key");
    CHECK(!(key_at(65536, 65536) == key_at(65552, 65536)), "the next chunk over does not");
    CHECK(!(RegionCacheKey::of(ewb::region_box(65536, 65536, 224), true) ==
            RegionCacheKey::of(ewb::region_box(65536, 65536, 160), true)), "radius is in the corners");
    ewb::RegionCacheKeyHash h;
    CHECK(h(key_at(65536, 65536)) == h(key_at(65540, 65551)), "equal keys hash equal");
}

static void test_column_gens() {
    const RegionBox b = ewb::region_box(65536, 65536);   // x/z [65312 .. 65775]
    struct P { int x, z; bool inside; const char* what; };
    const P pts[] = {
        {65536, 65536, true,  "centre"},
        {b.x0,  65536, true,  "west edge"},
        {b.x1,  65536, true,  "east edge"},
        {65536, b.z0,  true,  "north edge"},
        {65536, b.z1,  true,  "south edge"},
        {b.x0 - 1, 65536, false, "one block west of the box"},
        {b.x1 + 1, 65536, false, "one block east of the box"},
        {65536, b.z0 - 1, false, "one block north of the box"},
        {65536, b.z1 + 1, false, "one block south of the box"},
    };
    for (const P& p : pts) {
        ColumnGens g;
        g.bump(70000, 70000);   // unrelated history
        const uint64_t stamp = g.now();
        CHECK(g.unchanged_since(b, stamp), "fresh stamp is valid");
        g.bump(p.x, p.z);
        char msg[128];
        std::snprintf(msg, sizeof msg, "edit %s %s the entry", p.what, p.inside ? "invalidates" : "keeps");
        CHECK(g.unchanged_since(b, stamp) == !p.inside, msg);
    }
    // The tile fold: 4096 blocks away aliases (spurious miss, never a stale hit).
    {
        ColumnGens g;
        const uint64_t stamp = g.now();
        g.bump(65536 + 4096, 65536);
        CHECK(!g.unchanged_since(b, stamp), "an edit one tile away aliases: conservative miss");
        ColumnGens g2;
        g2.bump(65536 + 3000, 65536 + 3000);
        CHECK(g2.unchanged_since(b, stamp), "an edit well away (not on the fold) does not");
    }
    // bump_all invalidates everything stamped before it.
    {
        ColumnGens g;
        const uint64_t stamp = g.now();
        g.bump_all();
        CHECK(!g.unchanged_since(b, stamp), "bump_all invalidates");
        CHECK(g.unchanged_since(b, g.now()), "but a stamp taken after it is valid");
    }
    // A box wider than the tile still checks every slot (the sweep is capped, not skipped).
    {
        ColumnGens g;
        const uint64_t stamp = g.now();
        g.bump(65536 + 5000, 65536 + 5000);
        CHECK(!g.unchanged_since(RegionBox{60000, 72000, 60000, 72000}, stamp), "a huge box sees any edit in it");
    }
    // Near the origin a box goes negative; that must not crash or miss.
    {
        ColumnGens g;
        const RegionBox o = ewb::region_box(5, 5);
        const uint64_t stamp = g.now();
        g.bump(3, 3);
        CHECK(o.x0 < 0 && !g.unchanged_since(o, stamp), "edit in a box that crosses 0");
    }
}

static void test_stamp_race() {
    ColumnGens g;
    RegionCache c(1 << 20);
    const RegionCacheKey k = key_at(65536, 65536);
    const uint64_t stamp = g.now();   // the scan
    g.bump(65536, 65536);             // an edit lands while the writer is still encoding
    CHECK(c.insert(k, entry(100, stamp)), "the writer files it (it has no world lock)");
    CHECK(c.find(k, g) == nullptr, "...but it is never served");
    CHECK(c.entries() == 0 && c.bytes() == 0 && c.stats().stale == 1, "and the stale lookup dropped it");
    // An edit elsewhere does not stop the entry being served.
    const uint64_t stamp2 = g.now();
    CHECK(c.insert(k, entry(100, stamp2)), "a fresh scan's entry");
    g.bump(80000, 80000);
    CHECK(c.find(k, g) != nullptr && c.stats().hits == 1, "edits outside the box leave it valid");
}

static void test_lru_by_bytes() {
    ColumnGens g;
    RegionCache c(1000);                 // entries up to 250 bytes
    const RegionCacheKey a = key_at(10000, 10000), b = key_at(20000, 20000),
                         d = key_at(30000, 30000), e = key_at(40000, 40000), f = key_at(50000, 50000);
    CHECK(c.insert(a, entry(250, 0)) && c.insert(b, entry(250, 0)) && c.insert(d, entry(250, 0)) &&
              c.insert(e, entry(250, 0)), "fill to the bound");
    CHECK(c.bytes() == 1000 && c.entries() == 4, "exactly full");
    CHECK(c.find(a, g) != nullptr, "touch a: now most recently used");
    CHECK(c.insert(f, entry(200, 0)), "one more");
    CHECK(c.find(b, g) == nullptr, "b (least recently used) was evicted");
    CHECK(c.find(a, g) != nullptr && c.find(d, g) != nullptr && c.find(e, g) != nullptr &&
              c.find(f, g) != nullptr, "the rest survive");
    CHECK(c.bytes() == 950 && c.entries() == 4 && c.stats().evictions == 1, "bytes exact after eviction");

    CHECK(!c.insert(key_at(60000, 60000), entry(251, 0)), "over a quarter of the bound: never stored");
    CHECK(c.stats().oversize == 1 && c.bytes() == 950, "...counted, nothing evicted for it");

    // Replacement: a newer scan replaces, an older (or equal) one does not.
    CHECK(c.insert(a, entry(100, 5)), "newer stamp replaces");
    CHECK(c.bytes() == 800, "bytes follow the replacement");
    CHECK(!c.insert(a, entry(50, 4)) && !c.insert(a, entry(50, 5)), "older or equal stamp does not");
    CHECK(c.find(a, g) && c.find(a, g)->stamp == 5, "the newer entry is kept");

    c.clear();
    CHECK(c.bytes() == 0 && c.entries() == 0 && c.find(a, g) == nullptr, "clear empties it");

    RegionCache off(0);
    CHECK(!off.enabled() && !off.insert(a, entry(1, 0)) && off.find(a, g) == nullptr, "0 bytes = off");

    RegionCache shrink(1000);
    shrink.insert(a, entry(250, 0)); shrink.insert(b, entry(250, 0));
    shrink.set_max_bytes(300);
    CHECK(shrink.bytes() <= 300 && shrink.entries() == 1, "shrinking the bound evicts down to it");
}

// --- end to end against the real store -----------------------------------------

static std::shared_ptr<CachedRegion> serve_fresh(const ewb::WorldStore& w, const RegionBox& b,
                                                 uint64_t stamp, int level) {
    std::vector<ewb::SnapRec> recs;
    w.for_each_in_box(b.x0, b.x1, b.z0, b.z1, [&](int x, int y, int z, unsigned char t, unsigned char c) {
        ewb::emit_cell_records(x, y, z, t, c, recs);
    });
    auto e = std::make_shared<CachedRegion>();
    e->stamp = stamp;
    ewb::SnapzEncoder enc(level);
    if (recs.empty()) e->add(enc.encode(nullptr, 0), 0);
    for (size_t off = 0; off < recs.size(); off += ewb::SNAPZ_FRAME_RECORDS) {
        const size_t n = std::min(ewb::SNAPZ_FRAME_RECORDS, recs.size() - off);
        e->add(enc.encode(recs.data() + off, n), n);
    }
    return e;
}

static std::string wire(const CachedRegion& e) {
    std::string s;
    for (const auto& f : e.frames) s += f.line;
    return s;
}

static void test_end_to_end() {
    ewb::WorldStore w;
    ColumnGens g;
    auto set = [&](int x, int y, int z, unsigned char t, unsigned char c) {
        w.set(x, y, z, t, c);
        g.bump(x, z);   // what worldSet() does
    };
    for (int x = 65400; x < 65700; x += 3)
        for (int z = 65400; z < 65700; z += 5)
            for (int y = 30; y < 38; ++y) set(x, y, z, (unsigned char)(1 + (x + y + z) % 20), (x % 7) ? 0 : 9);

    RegionCache cache(64u << 20);
    const RegionBox b = ewb::region_box(65536, 65536);
    const RegionCacheKey k = RegionCacheKey::of(b, true);

    // Miss -> scan + encode -> file it.
    CHECK(cache.find(k, g) == nullptr, "first request misses");
    const auto first = serve_fresh(w, b, g.now(), 1);
    CHECK(first->records > 6000 && first->frames.size() > 2, "a multi-frame reply");
    CHECK(cache.insert(k, first), "filed");

    // Hit == what a fresh scan would send now.
    const auto hit = cache.find(k, g);
    CHECK(hit != nullptr, "repeat request hits");
    CHECK(hit && wire(*hit) == wire(*serve_fresh(w, b, g.now(), 1)), "a hit is byte-identical to a fresh reply");

    // An edit in the box: the next request is a miss, and the fresh reply has the edit.
    set(65500, 40, 65500, 77, 0);
    CHECK(cache.find(k, g) == nullptr, "after an edit in the box: miss");
    const auto second = serve_fresh(w, b, g.now(), 1);
    CHECK(wire(*second) != wire(*first), "the fresh reply differs");
    bool sawEdit = false;
    for (const auto& f : second->frames) {
        const std::string body = f.line.substr(0, f.line.size() - 1);
        const size_t c2 = body.find(':', body.find(':') + 1);
        const auto z = ewb::b64_decode(body.substr(c2 + 1));
        const auto raw = ewb::raw_inflate(z.data(), z.size());
        for (size_t i = 0; i + 20 <= raw.size(); i += 20) {
            const uint8_t* p = raw.data() + i;
            if (ewb::get_le_i32(p) == 65500 && ewb::get_le_i32(p + 4) == 40 &&
                ewb::get_le_i32(p + 8) == 65500 && ewb::get_le_i32(p + 16) == 77)
                sawEdit = true;
        }
    }
    CHECK(sawEdit, "...and carries the edit");
    CHECK(cache.insert(k, second) && cache.find(k, g) != nullptr, "re-filed and served again");

    // An empty box is cached as its one SNAPZ:0: frame.
    const RegionBox far = ewb::region_box(200000, 200000);
    const RegionCacheKey fk = RegionCacheKey::of(far, true);
    const auto empty = serve_fresh(w, far, g.now(), 1);
    CHECK(empty->frames.size() == 1 && empty->records == 0, "empty box: one SNAPZ:0: frame");
    CHECK(cache.insert(fk, empty) && cache.find(fk, g) != nullptr, "cached like any other");
}

int main() {
    test_key();
    test_column_gens();
    test_stamp_race();
    test_lru_by_bytes();
    test_end_to_end();
    if (g_fail) {
        std::fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
