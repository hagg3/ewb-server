// burn_test.cpp — offline checks for ROADMAP-SERVER stage 10: the burn rules the
// server shares with the game (block_rules.h), fireworks and the golden cube in a
// blast (10.2a), the expansion-block fill (10.2b), and the pure half of the TNT /
// fire switches — the verdict, the dry-run preview a refused burn is restored
// from, and its timing (burn_guard.h, 10.4).
//
//   c++ -std=c++17 -O2 -Wall burn_test.cpp -o burn_test
//   ./burn_test

#include <cstdio>
#include <set>
#include <tuple>
#include <unordered_map>

#include "block_rules.h"
#include "burn_guard.h"
#include "explode.h"

using namespace ewb;

static int g_fail = 0;

#define CHECK(cond, msg)                                                          \
    do {                                                                          \
        if (!(cond)) {                                                            \
            std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_fail;                                                             \
        }                                                                         \
    } while (0)

// A map-backed world. Unstored cells are air above `ground` and stone at or below
// it (ground < 0: all air), standing in for the base profile.
struct Fake {
    std::unordered_map<uint64_t, ExplodeCell> cells;
    std::set<std::tuple<int, int, int>> zone;      // protected cells
    std::set<std::tuple<int, int, int>> refused;   // ...that the hook was asked about
    size_t sets = 0;
    int ground = -1;
    ExplodeWorld w;

    Fake() {
        w.get = [this](int x, int y, int z, ExplodeCell& out) {
            auto it = cells.find(zone_cell_key(x, y, z));
            if (it == cells.end()) return false;
            out = it->second;
            return true;
        };
        w.set = [this](int x, int y, int z, int type, int color) {
            ++sets;
            cells[zone_cell_key(x, y, z)] = {(unsigned char)type, (unsigned char)color};
            return true;
        };
        w.protectedAt = [this](int x, int y, int z) {
            if (!zone.count({x, y, z})) return false;
            refused.insert({x, y, z});
            return true;
        };
        w.baseType = [this](int y) { return y <= ground ? (int)BLK_STONE : 0; };
    }
    void put(int x, int y, int z, int type, int color = 0) {
        cells[zone_cell_key(x, y, z)] = {(unsigned char)type, (unsigned char)color};
    }
    int type(int x, int y, int z) const {
        auto it = cells.find(zone_cell_key(x, y, z));
        if (it == cells.end()) return y <= ground ? (int)BLK_STONE : 0;
        return it->second.type;
    }
    int color(int x, int y, int z) const {
        auto it = cells.find(zone_cell_key(x, y, z));
        return it == cells.end() ? 0 : it->second.color;
    }
    // Cells stored with type t.
    size_t count(int t) const {
        size_t n = 0;
        for (const auto& kv : cells) n += kv.second.type == t;
        return n;
    }
};

// --- block_rules.h -------------------------------------------------------------

static void test_block_rules() {
    CHECK(is_expansion(81) && is_expansion(111) && !is_expansion(80) && !is_expansion(112),
          "expansion blocks are 81..111");
    CHECK(expansion_material(84) == BLK_STONE && expansion_material(87) == BLK_TNT &&
              expansion_material(107) == BLK_WATER && expansion_material(111) == BLK_STEEL &&
              expansion_material(106) == BLK_WEAVE && expansion_material(103) == BLK_WOOD,
          "blockTntMap spot checks");
    CHECK(expansion_material(81) == BLK_AIR, "TYPE_BLOCK_TNT has no material");
    for (int t = 82; t <= 111; ++t) CHECK(expansion_material(t) > 0, "every other expansion has a material");
    CHECK(expansion_ramp(102) == BLK_STONE_RAMP1 && expansion_ramp(105) == BLK_SHINGLE_RAMP1 &&
              expansion_ramp(84) == 0,
          "only the side variants have a ramp ring");

    for (int t : {5, 6, 7, 9, 18, 21, 28, 31, 44, 47, 65, 66, 70, 73, 81, 111})
        CHECK(is_flammable(t), "flammable per the game's IS_FLAMMABLE");
    for (int t : {0, 1, 2, 3, 8, 20, 24, 32, 40, 48, 71, 72, 74})
        CHECK(!is_flammable(t), "not flammable");

    // getRampType2: two adjacent solid faces make a side block, three a full block.
    const bool negXnegZ[4] = {false, false, true, true};
    CHECK(ramp_for_faces(BLK_STONE_RAMP1, negXnegZ) == 42, "-x and -z solid: stone side 3");
    const bool posXposZ[4] = {true, true, false, false};
    CHECK(ramp_for_faces(BLK_WOOD_RAMP1, posXposZ) == 44, "+x and +z solid: wood side 1");
    const bool three[4] = {true, true, true, false};
    CHECK(ramp_for_faces(BLK_ICE_RAMP1, three) == BLK_ICE, "three solid faces: the full block");
    const bool none[4] = {false, false, false, false};
    CHECK(ramp_for_faces(BLK_SHINGLE_RAMP1, none) == BLK_SHINGLE_RAMP1, "no solid face: unrotated ramp");
    CHECK(face_solid(BLK_STONE, 0) && !face_solid(0, 0) && !face_solid(-1, 0), "plain block / air / unknown");
    CHECK(face_solid(24, 3) && !face_solid(24, 0), "a ramp is solid only on its solid faces");
}

// --- 10.2a: fireworks and the golden cube ---------------------------------------

static void test_firework_root() {
    Fake f;
    f.put(100, 100, 100, BLK_FIREWORK);
    f.put(101, 100, 100, BLK_STONE);
    const ExplodeResult r = simBurn(f.w, 100, 100, 100);
    CHECK(r.explosions == 0, "a lit firework does not blast");
    CHECK(f.type(100, 100, 100) == 0 && f.type(101, 100, 100) == BLK_STONE, "it removes only its own cell");
    CHECK(f.sets == 1, "one write");
}

static void test_firework_in_blast() {
    // TNT, a firework 3 away, and a TNT 3 past the firework (6 from the root, outside
    // its radius-5 blast). The firework used to chain into a blast that reached it.
    Fake f;
    f.put(100, 100, 100, BLK_TNT);
    f.put(103, 100, 100, BLK_FIREWORK);
    f.put(106, 100, 100, BLK_TNT);
    const ExplodeResult r = simBurn(f.w, 100, 100, 100);
    CHECK(r.explosions == 1, "a firework in a blast does not chain");
    CHECK(f.type(103, 100, 100) == 0, "the firework is gone");
    CHECK(f.type(106, 100, 100) == BLK_TNT, "the TNT past it is untouched");
}

static void test_golden_cube_survives() {
    Fake f;
    f.put(100, 100, 100, BLK_TNT);
    f.put(102, 100, 100, BLK_GOLDEN_CUBE);
    f.put(100, 102, 100, BLK_STEEL);
    f.put(100, 98, 100, BLK_BEDROCK);
    f.put(98, 100, 100, BLK_WOOD);
    simBurn(f.w, 100, 100, 100);
    CHECK(f.type(102, 100, 100) == BLK_GOLDEN_CUBE, "the golden cube survives a blast");
    CHECK(f.type(100, 102, 100) == BLK_STEEL && f.type(100, 98, 100) == BLK_BEDROCK, "so do steel and bedrock");
    CHECK(f.type(98, 100, 100) == 0, "wood does not (10.3 will burn it instead)");
}

static void test_burn_other_roots() {
    Fake f;
    f.put(5, 50, 5, BLK_WOOD);
    simBurn(f.w, 5, 50, 5);
    CHECK(f.type(5, 50, 5) == 0, "a lit wood block is gone");
    const ExplodeResult r = simBurn(f.w, 9, 50, 9);   // nothing stored
    CHECK(f.sets == 1 && r.explosions == 0 && r.expansions == 0, "a burn on nothing does nothing");
}

// --- 10.2b: expansion blocks ------------------------------------------------------

static void test_expansion_isolated() {
    // In open air: the full 5x5x5 box fills, and the centre ends as the material
    // too (the re-lit centre fires on its emptied cell), unpainted.
    Fake f;
    f.put(100, 100, 100, 84 /* BTSTONE */, 7);
    const ExplodeResult r = simBurn(f.w, 100, 100, 100);
    CHECK(r.expansions == 1 && r.explosions == 0, "one expansion, no blast");
    CHECK(f.count(BLK_STONE) == 125, "the 5x5x5 box is stone");
    CHECK(f.type(102, 102, 102) == BLK_STONE && f.type(98, 98, 98) == BLK_STONE, "corners filled");
    CHECK(f.type(103, 100, 100) == 0 && f.type(100, 103, 100) == 0, "nothing past 2");
    CHECK(f.color(101, 100, 100) == 7, "the fill takes the block's colour");
    CHECK(f.type(100, 100, 100) == BLK_STONE && f.color(100, 100, 100) == 0, "the centre is stone, unpainted");
    CHECK(r.visits <= EXPAND_VISITS_PER_LINK, "one link reads no more than its budgeted share");
    CHECK(r.maxDepth == 0, "the root is depth 0");
}

static void test_expansion_on_ground() {
    // On the ground (stone at y <= 32): it grows up and out, not down.
    Fake f;
    f.ground = 32;
    f.put(100, 33, 100, 85 /* BTDIRT */);
    simBurn(f.w, 100, 33, 100);
    CHECK(f.count(BLK_DIRT) == 75, "5x5x3: the layer it sits on and two above");
    CHECK(f.type(100, 32, 100) == BLK_STONE, "the ground below is untouched");
}

static void test_expansion_against_a_wall() {
    // A block on its +x side: no growth in +x at all.
    Fake f;
    f.put(100, 100, 100, 84);
    f.put(101, 100, 100, BLK_BRICK);
    simBurn(f.w, 100, 100, 100);
    CHECK(f.type(102, 100, 100) == 0 && f.type(101, 101, 100) == 0, "nothing on the walled side");
    CHECK(f.type(98, 100, 100) == BLK_STONE, "the other side fills");
    CHECK(f.count(BLK_STONE) == 3 * 5 * 5, "x 98..100 by 5 by 5");
}

static void test_expansion_side_variant() {
    Fake f;
    f.put(100, 100, 100, 102 /* BTSTONESIDE */);
    simBurn(f.w, 100, 100, 100);
    // Each corner column is a side block facing the box.
    CHECK(f.type(102, 100, 102) == 42, "+x+z corner: faces -x and -z");
    CHECK(f.type(98, 100, 98) == 40, "-x-z corner: faces +x and +z");
    CHECK(f.type(102, 101, 98) == 41, "+x-z corner");
    CHECK(f.type(98, 99, 102) == 43, "-x+z corner");
    CHECK(f.count(BLK_STONE) == 125 - 4 * 5, "the rest of the box is stone");
}

static void test_expansion_plain_block() {
    // TYPE_BLOCK_TNT (81) has no material: it just goes.
    Fake f;
    f.put(100, 100, 100, 81);
    simBurn(f.w, 100, 100, 100);
    CHECK(f.cells.size() == 1 && f.type(100, 100, 100) == 0, "only its own cell, now air");
}

static void test_expansion_chain() {
    Fake f;
    f.put(100, 100, 100, 84);
    f.put(102, 100, 100, 85);   // inside the first's box: lit by it
    f.put(110, 100, 100, 85);   // far away: not
    const ExplodeResult r = simBurn(f.w, 100, 100, 100);
    CHECK(r.expansions == 2, "a lit expansion lights the expansion blocks in its box");
    CHECK(r.maxDepth == 1, "the second is one link deep");
    CHECK(f.type(102, 100, 100) == BLK_DIRT, "the second fired and ended as its material");
    CHECK(f.type(110, 100, 100) == 85, "the far one did not");
}

static void test_expansion_in_blast() {
    // A blast lights an expansion block instead of destroying it; it fills after.
    Fake f;
    f.put(100, 100, 100, BLK_TNT);
    f.put(103, 100, 100, 84);
    const ExplodeResult r = simBurn(f.w, 100, 100, 100);
    CHECK(r.explosions == 1 && r.expansions == 1, "one blast, then one fill");
    CHECK(f.type(103, 100, 100) == BLK_STONE && f.type(101, 100, 100) == BLK_STONE,
          "the fill lands in the crater");
    CHECK(f.type(100, 100, 100) == 0, "the TNT's own cell is air");
}

static void test_expansion_tnt_fill_is_unlit() {
    Fake f;
    f.put(100, 100, 100, BLK_BT_TNT);
    const ExplodeResult r = simBurn(f.w, 100, 100, 100);
    CHECK(r.explosions == 0 && f.count(BLK_TNT) == 125, "87 fills with TNT, which does not go off");
}

static void test_expansion_zone() {
    // A protected cell in the box is not filled, and is reported; a protected
    // expansion block is not lit.
    Fake f;
    f.put(100, 100, 100, 84);
    f.put(102, 100, 102, 85);
    f.zone.insert({101, 100, 100});
    f.zone.insert({102, 100, 102});
    const ExplodeResult r = simBurn(f.w, 100, 100, 100);
    CHECK(f.type(101, 100, 100) == 0, "the protected cell stays air");
    CHECK(f.refused.count({101, 100, 100}) == 1, "and is reported for the restore");
    CHECK(f.type(102, 100, 102) == 85 && r.expansions == 1, "a protected expansion is not lit");
    CHECK(r.protectedHits >= 2, "both counted");
}

static void test_expansion_reach() {
    // A line of expansion blocks every 2 cells, far longer than the reach box: the
    // chain stops at it, says so, and writes nothing outside it.
    Fake f;
    const int x0 = 1000, n = 200;
    for (int i = 0; i < n; ++i) f.put(x0 + 2 * i, 100, 1000, 84);
    const ExplodeResult r = simBurn(f.w, x0, 100, 1000);
    CHECK(r.truncated > 0, "a chain past burn_reach() is cut and counted");
    int maxOff = 0;
    for (const auto& kv : f.cells) {
        if (kv.second.type == 84) continue;   // a block of the line that never fired
        const int x = int(kv.first >> 40);
        maxOff = std::max(maxOff, std::abs(x - x0));
    }
    CHECK(maxOff <= burn_reach(), "no write outside burn_reach()");
    CHECK(maxOff >= burn_reach() - 2 * EXPAND_EXTENT, "and it does get close to it");
}

static void test_expansion_budget() {
    // Blast-lit expansions share the burn's budget: with one blast's worth, the
    // blast runs and every expansion it lit is dropped.
    Fake f;
    f.put(100, 100, 100, BLK_TNT);
    f.put(103, 100, 100, 84);
    f.put(97, 100, 100, 84);
    const ExplodeResult r = simBurn(f.w, 100, 100, 100, explode_visits_per_blast());
    CHECK(r.explosions == 1 && r.expansions == 0 && r.truncated == 2, "no budget left: both dropped");
    // A root expansion always runs, like a root blast.
    Fake g;
    g.put(100, 100, 100, 84);
    const ExplodeResult q = simBurn(g.w, 100, 100, 100, 1);
    CHECK(q.expansions == 1, "the root expansion runs whatever the budget");
}

static void test_expand_box_footprint() {
    std::set<std::tuple<int, int, int>> box;
    expand_for_each_box_cell(100, 100, 100, 0, 256, [&](int x, int y, int z) { box.insert({x, y, z}); });
    Fake f;
    f.put(100, 100, 100, 84);
    simBurn(f.w, 100, 100, 100);
    bool inside = true;
    for (const auto& kv : f.cells) {
        const int x = int(kv.first >> 40), y = int(kv.first & 0xFFFF), z = int((kv.first >> 16) & 0xFFFFFF);
        inside = inside && box.count({x, y, z});
    }
    CHECK(box.size() == 125 && inside, "the box footprint covers a fill");
}

// --- burn_guard.h ------------------------------------------------------------------

static void test_switches() {
    bool b = true;
    CHECK(parse_on_off("off", b) && !b && parse_on_off("on", b) && b, "on / off");
    CHECK(!parse_on_off("OFF", b) && !parse_on_off("0", b) && !parse_on_off("", b), "nothing else");
    CHECK(is_tnt_build(9) && is_tnt_build(87) && !is_tnt_build(84) && !is_tnt_build(65), "TNT builds");

    CHECK(burn_verdict(true, true, true) == BurnVerdict::Allow, "both on: allowed");
    CHECK(burn_verdict(true, false, false) == BurnVerdict::RefuseFire, "fire off: every burn");
    CHECK(burn_verdict(false, false, true) == BurnVerdict::RefuseFire, "both off: the fire reason");
    CHECK(burn_verdict(false, true, true) == BurnVerdict::RefuseTnt, "TNT off: a burn that reaches TNT");
    CHECK(burn_verdict(false, true, false) == BurnVerdict::Allow, "TNT off: a plain fire is fine");
    CHECK(std::string(burn_refusal_notice(BurnVerdict::RefuseTnt)) == "TNT is disabled on this server." &&
              std::string(burn_refusal_notice(BurnVerdict::RefuseFire)) == "Burning is disabled on this server.",
          "notice text");

    CHECK(burn_restore_late_sec(0, 0) == BURN_LIFE_SEC + BURN_RESTORE_MARGIN_SEC, "a lone block: its burn time");
    CHECK(burn_restore_late_sec(3, 2) > burn_restore_late_sec(3, 1) &&
              burn_restore_late_sec(4, 0) > burn_restore_late_sec(3, 0),
          "later for a wider fire or a deeper chain");
    CHECK(burn_restore_late_sec(100000, 0) == BURN_RESTORE_LATE_MAX_SEC, "capped");
}

static void test_preview_fire() {
    // A 4-ladder column with stone beside it: the fire takes the column only.
    Fake f;
    for (int y = 100; y < 104; ++y) f.put(50, y, 50, BLK_LADDER);
    f.put(51, 100, 50, BLK_STONE);
    const size_t before = f.cells.size();
    const BurnPreview p = preview_burn(f.w, 50, 101, 50, EXPLODE_DEFAULT_MAX_VISITS);
    CHECK(p.flammable && !p.explosive && !p.touchesTnt, "a ladder: flammable, harmless");
    CHECK(p.cells.size() == 4 && p.cells.contains(50, 103, 50) && !p.cells.contains(51, 100, 50),
          "the whole column, not the stone");
    CHECK(p.cells.cells()[0] == (RevertCell{50, 101, 50}), "the lit cell first");
    CHECK(p.fireDepth == 2, "two spreads to reach both ends");
    CHECK(f.cells.size() == before && f.type(50, 101, 50) == BLK_LADDER && f.sets == 0,
          "the dry run changes nothing");
}

static void test_preview_reaches_tnt() {
    // Wood touching TNT: the fire reaches it, and its blast is in the restore.
    Fake f;
    f.put(50, 100, 50, BLK_WOOD);
    f.put(51, 100, 50, BLK_TNT);
    const BurnPreview p = preview_burn(f.w, 50, 100, 50, EXPLODE_DEFAULT_MAX_VISITS);
    CHECK(p.touchesTnt && p.explosions == 1, "the fire sets the TNT off");
    CHECK(p.cells.contains(51 + EXPLODE_RADIUS, 100, 50), "the blast sphere is restored");
    CHECK(p.chainDepth >= 1, "the chain counts towards the late restore");
    CHECK(f.sets == 0, "nothing written");
}

static void test_preview_roots() {
    Fake f;
    f.put(50, 100, 50, BLK_STONE);
    const BurnPreview stone = preview_burn(f.w, 50, 100, 50, EXPLODE_DEFAULT_MAX_VISITS);
    CHECK(!stone.flammable && stone.cells.empty(), "stone does not burn: nothing to restore");
    const BurnPreview sky = preview_burn(f.w, 60, 100, 60, EXPLODE_DEFAULT_MAX_VISITS);
    CHECK(!sky.flammable && sky.cells.empty(), "nor does air");

    f.put(70, 100, 70, BLK_TNT);
    const BurnPreview tnt = preview_burn(f.w, 70, 100, 70, EXPLODE_DEFAULT_MAX_VISITS);
    CHECK(tnt.explosive && tnt.touchesTnt && tnt.cells.size() > 100, "lit TNT: its sphere");

    f.put(80, 100, 80, BLK_FIREWORK);
    const BurnPreview fw = preview_burn(f.w, 80, 100, 80, EXPLODE_DEFAULT_MAX_VISITS);
    CHECK(fw.explosive && !fw.touchesTnt && fw.cells.size() == 1, "a firework: its own cell");

    f.put(90, 100, 90, 84);
    const BurnPreview ex = preview_burn(f.w, 90, 100, 90, EXPLODE_DEFAULT_MAX_VISITS);
    CHECK(ex.explosive && !ex.touchesTnt && ex.cells.size() == 125, "an expansion: its fill");
    f.put(90, 150, 90, BLK_BT_TNT);
    const BurnPreview bt = preview_burn(f.w, 90, 150, 90, EXPLODE_DEFAULT_MAX_VISITS);
    CHECK(bt.touchesTnt && bt.explosions == 0, "the TNT expansion makes TNT");
    CHECK(f.sets == 0 && f.type(90, 100, 90) == 84, "and none of it was written");
}

static void test_preview_cap() {
    // A forest bigger than the restore cap: cut, and flagged.
    Fake f;
    for (int x = 0; x < 40; ++x)
        for (int z = 0; z < 40; ++z) f.put(1000 + x, 100, 1000 + z, BLK_LEAVES);
    const BurnPreview p = preview_burn(f.w, 1000, 100, 1000, EXPLODE_DEFAULT_MAX_VISITS, 500);
    CHECK(p.truncated && p.cells.size() == 500, "the preview stops at its cap and says so");
}

int main() {
    test_block_rules();
    test_firework_root();
    test_firework_in_blast();
    test_golden_cube_survives();
    test_burn_other_roots();
    test_expansion_isolated();
    test_expansion_on_ground();
    test_expansion_against_a_wall();
    test_expansion_side_variant();
    test_expansion_plain_block();
    test_expansion_chain();
    test_expansion_in_blast();
    test_expansion_tnt_fill_is_unlit();
    test_expansion_zone();
    test_expansion_reach();
    test_expansion_budget();
    test_expand_box_footprint();
    test_switches();
    test_preview_fire();
    test_preview_reaches_tnt();
    test_preview_roots();
    test_preview_cap();
    if (g_fail) {
        std::fprintf(stderr, "%d check(s) FAILED\n", g_fail);
        return 1;
    }
    std::printf("All burn_test checks passed.\n");
    return 0;
}
