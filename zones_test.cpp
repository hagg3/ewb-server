// zones_test.cpp — offline checks for ROADMAP-SERVER stage 8.1: the pure
// anti-grief zone model in zones.h (grammar parsing, normalisation, caps,
// blocking()/intersects() against brute-force references, and the
// parse<->serialize round trip).
//
//   c++ -std=c++17 -O2 -Wall zones_test.cpp -o zones_test
//   ./zones_test

#include <cstdio>
#include <cstdlib>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "zones.h"

using namespace ewb;

static int g_fail = 0;

#define CHECK(cond, msg)                                                          \
    do {                                                                          \
        if (!(cond)) {                                                            \
            std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_fail;                                                             \
        }                                                                         \
    } while (0)

static void test_parse_line_valid() {
    Zone z;
    CHECK(zone_parse_line("spawn:65500:0:65500:65572:255:65572:all", z), "basic line parses");
    CHECK(z.name == "spawn", "name");
    CHECK(z.x0 == 65500 && z.x1 == 65572, "x normalised (already ordered)");
    CHECK(z.y0 == 0 && z.y1 == 255, "y normalised");
    CHECK(z.z0 == 65500 && z.z1 == 65572, "z normalised");
    CHECK(z.enforced, "flag 'all' enforces");
    CHECK(!z.hasLevel, "no trailing level field");

    // reversed corners normalise
    Zone z2;
    CHECK(zone_parse_line("box:10:20:30:1:2:3:all", z2), "reversed-corner line parses");
    CHECK(z2.x0 == 1 && z2.x1 == 10, "x min/max");
    CHECK(z2.y0 == 2 && z2.y1 == 20, "y min/max");
    CHECK(z2.z0 == 3 && z2.z1 == 30, "z min/max");

    Zone z3;
    CHECK(zone_parse_line("off-zone:0:0:0:10:10:10:off", z3), "flag 'off' parses");
    CHECK(!z3.enforced, "flag 'off' does not enforce");

    Zone z4;
    CHECK(zone_parse_line("lvl:0:0:0:5:5:5:all:2", z4), "trailing level parses");
    CHECK(z4.hasLevel && z4.level == 2, "level stored");
    Zone z5;
    CHECK(zone_parse_line("lvl0:0:0:0:5:5:5:all:0", z5) && z5.hasLevel && z5.level == 0,
          "level 0 (any logged-in player) parses");

}

static void test_parse_line_invalid() {
    Zone z;
    CHECK(!zone_parse_line("bad name!:0:0:0:1:1:1:all", z), "invalid characters in name rejected");
    CHECK(!zone_parse_line(std::string(33, 'a') + ":0:0:0:1:1:1:all", z), "name over 32 chars rejected");
    CHECK(!zone_parse_line(":0:0:0:1:1:1:all", z), "empty name rejected");
    CHECK(!zone_parse_line("x:0:0:0:1:1:all", z), "too few fields rejected");
    CHECK(!zone_parse_line("x:0:0:0:1:1:1:1:all:1:2", z), "too many fields rejected");
    CHECK(!zone_parse_line("x:0:0:0:1:1:1:everything", z), "unknown flag rejected — hard error, never silently ignored");
    CHECK(!zone_parse_line("x:0:0:0:1:1:1:nobuild", z), "flag other than all/off rejected (v1)");
    CHECK(!zone_parse_line("x:0:a:0:1:1:1:all", z), "non-numeric coordinate rejected");
    CHECK(!zone_parse_line("x:0:0:0:1:1:1:all:notanumber", z), "non-numeric level rejected");
    CHECK(!zone_parse_line("x::0:0:1:1:1:all", z), "empty coordinate field rejected");
    CHECK(!zone_parse_line("x:0:0:0:1:1:1:all:3", z), "level above 2 rejected (op levels are 0..2)");
    CHECK(!zone_parse_line("x:0:0:0:1:1:1:all:-1", z), "negative level rejected");

    // out-of-range coordinates
    CHECK(!zone_parse_line("x:-1:0:0:1:1:1:all", z), "x below 0 rejected");
    CHECK(!zone_parse_line("x:0:0:0:16777216:1:1:all", z), "x above 0xFFFFFF rejected");
    CHECK(!zone_parse_line("x:0:0:0:1:256:1:all", z), "y above 255 rejected");
    CHECK(!zone_parse_line("x:0:-1:0:1:1:1:all", z), "y below 0 rejected");
    CHECK(!zone_parse_line("x:0:0:-1:1:1:1:all", z), "z below 0 rejected");
    CHECK(!zone_parse_line("x:0:0:0:1:1:16777216:all", z), "z above 0xFFFFFF rejected");

    // boundary is inclusive and accepted
    Zone ok;
    CHECK(zone_parse_line("edge:0:0:0:16777215:255:16777215:all", ok), "max in-range box accepted");
}

static void test_zoneset_add_dup_cap() {
    ZoneSet set;
    Zone z;
    CHECK(zone_parse_line("a:0:0:0:1:1:1:all", z), "parses");
    CHECK(set.add(z), "first add ok");
    CHECK(!set.add(z), "duplicate name rejected");

    // Fill to the cap with distinct names, one already used ("a").
    ZoneSet cap;
    for (size_t i = 0; i < ZONE_MAX; ++i) {
        Zone zi;
        std::string line = "z" + std::to_string(i) + ":0:0:0:1:1:1:all";
        CHECK(zone_parse_line(line, zi), "cap-fill line parses");
        CHECK(cap.add(zi), "add under cap ok");
    }
    CHECK(cap.size() == ZONE_MAX, "set reached ZONE_MAX");
    Zone over;
    CHECK(zone_parse_line("one-too-many:0:0:0:1:1:1:all", over), "257th line itself parses fine");
    CHECK(!cap.add(over), "257th zone rejected by the cap");
}

static void test_load_stream_rejects_whole_file() {
    // A malformed line anywhere in the file must leave the ZoneSet untouched
    // (load-time all-or-nothing), not partially populated.
    {
        std::istringstream in(
            "spawn:0:0:0:10:10:10:all\n"
            "broken:0:0:0:10:10:10:notaflag\n");
        ZoneSet set;
        std::string err; int line = 0;
        CHECK(!ZoneSet::load(in, set, &err, &line), "load fails on bad flag");
        CHECK(line == 2, "error line number reported");
        CHECK(set.size() == 0, "set left untouched on failure");
    }
    {
        std::istringstream in(
            "spawn:0:0:0:10:10:10:all\n"
            "spawn:20:20:20:30:30:30:all\n");  // duplicate name
        ZoneSet set;
        std::string err;
        CHECK(!ZoneSet::load(in, set, &err), "load fails on duplicate name");
        CHECK(set.size() == 0, "set left untouched on duplicate");
    }
    {
        std::istringstream in(
            "# comment\n"
            "\n"
            "  \n"
            "spawn:0:0:0:10:10:10:all\n");
        ZoneSet set;
        std::string err;
        CHECK(ZoneSet::load(in, set, &err), "comments/blank lines skipped");
        CHECK(set.size() == 1, "exactly one real zone loaded");
    }
}

static void test_blocking_and_off() {
    ZoneSet set;
    Zone a, b;
    CHECK(zone_parse_line("a:0:0:0:10:10:10:all", a), "parses");
    CHECK(zone_parse_line("b:5:5:5:15:15:15:off", b), "parses");
    CHECK(set.add(a) && set.add(b), "both added");

    CHECK(set.blocking(1, 1, 1) == set.find("a"), "cell inside 'a' is blocked by 'a'");
    CHECK(set.blocking(20, 20, 20) == nullptr, "cell outside every zone is not blocked");
    CHECK(set.blocking(12, 12, 12) == nullptr,
          "cell only inside the 'off' zone is not blocked");
    CHECK(set.blocking(8, 8, 8) == set.find("a"),
          "cell inside both an enforcing and an off zone is blocked by the enforcing one");
}

static void test_round_trip() {
    ZoneSet set;
    Zone a, b, c;
    CHECK(zone_parse_line("alpha:0:0:0:10:10:10:all", a), "parses");
    CHECK(zone_parse_line("beta:5:20:5:50:90:50:off", b), "parses");
    CHECK(zone_parse_line("gamma:100:0:100:200:255:200:all:2", c), "parses");
    CHECK(set.add(a) && set.add(b) && set.add(c), "all added");

    std::string text = set.serialize();
    std::istringstream in(text);
    ZoneSet reloaded;
    std::string err;
    CHECK(ZoneSet::load(in, reloaded, &err), "serialized text reloads");
    CHECK(reloaded.size() == set.size(), "same zone count after round trip");
    for (const Zone& z : set.zones()) {
        const Zone* r = reloaded.find(z.name);
        CHECK(r != nullptr, "each zone survives round trip by name");
        if (!r) continue;
        CHECK(r->x0 == z.x0 && r->y0 == z.y0 && r->z0 == z.z0 &&
              r->x1 == z.x1 && r->y1 == z.y1 && r->z1 == z.z1, "box survives round trip");
        CHECK(r->enforced == z.enforced, "flag survives round trip");
        CHECK(r->hasLevel == z.hasLevel && r->level == z.level, "level survives round trip");
    }
}

// --- brute-force reference checks over random boxes ------------------------

struct RefZone { int64_t x0, y0, z0, x1, y1, z1; bool enforced; };

static const RefZone* ref_blocking(const std::vector<RefZone>& zs, int64_t x, int64_t y, int64_t z) {
    for (const auto& r : zs)
        if (r.enforced && x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1 && z >= r.z0 && z <= r.z1)
            return &r;
    return nullptr;
}

static bool ref_intersects(const std::vector<RefZone>& zs,
                            int64_t bx0, int64_t by0, int64_t bz0,
                            int64_t bx1, int64_t by1, int64_t bz1) {
    for (const auto& r : zs) {
        if (!r.enforced) continue;
        if (r.x0 <= bx1 && bx0 <= r.x1 && r.y0 <= by1 && by0 <= r.y1 && r.z0 <= bz1 && bz0 <= r.z1)
            return true;
    }
    return false;
}

static void test_random_boxes_against_brute_force() {
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int64_t> xz(0, 200);
    std::uniform_int_distribution<int64_t> y(0, 255);
    std::uniform_int_distribution<int> onoff(0, 4);  // ~20% "off"

    ZoneSet set;
    std::vector<RefZone> ref;
    for (int i = 0; i < 60; ++i) {
        int64_t ax = xz(rng), az = xz(rng), bx = xz(rng), bz = xz(rng);
        int64_t ay = y(rng), by = y(rng);
        bool enforced = onoff(rng) != 0;
        RefZone r{std::min(ax, bx), std::min(ay, by), std::min(az, bz),
                  std::max(ax, bx), std::max(ay, by), std::max(az, bz), enforced};
        ref.push_back(r);

        Zone z;
        z.name = "z" + std::to_string(i);
        z.x0 = r.x0; z.y0 = r.y0; z.z0 = r.z0;
        z.x1 = r.x1; z.y1 = r.y1; z.z1 = r.z1;
        z.enforced = r.enforced;
        CHECK(set.add(z), "random zone added");
    }

    std::uniform_int_distribution<int64_t> pxz(0, 200);
    std::uniform_int_distribution<int64_t> py(0, 255);
    bool anyBlockChecked = false, anyIntersectChecked = false;
    for (int i = 0; i < 2000; ++i) {
        int64_t x = pxz(rng), z = pxz(rng), yy = py(rng);
        const RefZone* rz = ref_blocking(ref, x, yy, z);
        const Zone* zz = set.blocking(x, yy, z);
        anyBlockChecked = true;
        if ((rz == nullptr) != (zz == nullptr)) {
            CHECK(false, "blocking() disagrees with brute force on null-ness");
            continue;
        }
        if (rz && zz) {
            CHECK(zz->contains(x, yy, z) && zz->enforced,
                  "blocking() result actually contains the cell and enforces");
        }
    }
    CHECK(anyBlockChecked, "blocking fuzz ran");

    for (int i = 0; i < 500; ++i) {
        int64_t ax = pxz(rng), az = pxz(rng), bx = pxz(rng), bz = pxz(rng);
        int64_t ay = py(rng), by = py(rng);
        int64_t x0 = std::min(ax, bx), x1 = std::max(ax, bx);
        int64_t y0 = std::min(ay, by), y1 = std::max(ay, by);
        int64_t z0 = std::min(az, bz), z1 = std::max(az, bz);
        bool expect = ref_intersects(ref, x0, y0, z0, x1, y1, z1);
        bool got = set.intersects(x0, y0, z0, x1, y1, z1);
        anyIntersectChecked = true;
        CHECK(expect == got, "intersects() agrees with brute force over random boxes");
    }
    CHECK(anyIntersectChecked, "intersects fuzz ran");
}

// Stage 8.6: a zone's level is bypassed only by a logged-in session at or above it;
// -1 (not logged in) bypasses nothing, and a zone with no level is never bypassed.
static void test_bypass_levels() {
    ZoneSet set;
    Zone a, b, c;
    CHECK(zone_parse_line("nolevel:0:0:0:9:9:9:all", a), "parse nolevel");
    CHECK(zone_parse_line("ops:20:0:0:29:9:9:all:2", b), "parse ops");
    CHECK(zone_parse_line("anyone:40:0:0:49:9:9:all:0", c), "parse anyone");
    set.add(a); set.add(b); set.add(c);
    CHECK(set.hasBypassLevels(), "a set with a levelled zone says so");

    CHECK(set.blocking(5, 5, 5, 2) == set.find("nolevel"), "no level: even a logged-in op is blocked");
    CHECK(set.blocking(25, 5, 5, -1) == set.find("ops"), "level 2 zone: not-logged-in is blocked");
    CHECK(set.blocking(25, 5, 5, 1) == set.find("ops"), "level 2 zone: logged-in builder is blocked");
    CHECK(set.blocking(25, 5, 5, 2) == nullptr, "level 2 zone: logged-in op bypasses");
    CHECK(set.blocking(45, 5, 5, -1) == set.find("anyone"), "level 0 zone: not-logged-in is still blocked");
    CHECK(set.blocking(45, 5, 5, 0) == nullptr, "level 0 zone: any logged-in player bypasses");

    // Overlap: a bypassed zone must not hide an enforcing one under it.
    ZoneSet ov;
    Zone lo, hi;
    CHECK(zone_parse_line("outer:0:0:0:99:99:99:all:0", lo), "parse outer");
    CHECK(zone_parse_line("inner:10:10:10:20:20:20:all", hi), "parse inner");
    ov.add(lo); ov.add(hi);
    CHECK(ov.blocking(15, 15, 15, 2) == ov.find("inner"),
          "bypassing the first zone still reports an overlapping zone that is not bypassed");

    ZoneSet plain;
    plain.add(a);
    CHECK(!plain.hasBypassLevels(), "no levelled zone: hasBypassLevels() is false");
    Zone off;
    CHECK(zone_parse_line("offlvl:0:0:0:1:1:1:off:2", off), "parse off+level");
    plain.add(off);
    CHECK(!plain.hasBypassLevels(), "a levelled zone that is 'off' does not count");
}

int main() {
    test_bypass_levels();
    test_parse_line_valid();
    test_parse_line_invalid();
    test_zoneset_add_dup_cap();
    test_load_stream_rejects_whole_file();
    test_blocking_and_off();
    test_round_trip();
    test_random_boxes_against_brute_force();

    if (g_fail) {
        std::fprintf(stderr, "%d check(s) FAILED\n", g_fail);
        return 1;
    }
    std::printf("All zones_test checks passed.\n");
    return 0;
}
