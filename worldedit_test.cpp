// worldedit_test.cpp — offline checks for ROADMAP-SERVER stage 3.3: the Tier 2
// player command surface (worldedit.h).
//
//   clang++ -std=c++17 -O2 -Wall worldedit_test.cpp -o worldedit_test
//   ./worldedit_test
//
// Everything under test is pure: the dispatch table and its permission floors,
// the line grammar, coordinate/block/colour parsing, the selection cap, the
// shape predicates, the byte-bounded undo store, and the clipboard rotation.
// Applying an edit, taking the world lock and broadcasting are wire behaviour of
// server_posix.cpp and belong in the live socket test.
//
// The security-critical assertions here are the ones that would have caught the
// defects the community patch shipped with (plan §0.5.7): a selection is capped
// at the point it is read (defect 1), no row is reachable below its level
// (defect 2), and undo is bounded by bytes rather than batch count (defect 5).

#include <cstdio>
#include <string>
#include <vector>

#include "worldedit.h"

static int g_fail = 0;

#define CHECK(cond, msg)                                                             \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__);    \
            ++g_fail;                                                                \
        }                                                                            \
    } while (0)

// --- table + permissions -----------------------------------------------------

static void test_table() {
    CHECK(ewb::we_find("//set") != nullptr, "//set is a known verb");
    CHECK(ewb::we_find("/set") == nullptr, "single-slash //set is not a verb");
    CHECK(ewb::we_find("//nonsense") == nullptr, "unknown verb not found");

    // Every row in the vocabulary the plan (§0.5.6) told us to adopt verbatim.
    for (const char* v : {"/help", "/msg", "/r", "/tp", "/searchblocks", "/searchcolors",
                          "/id", "/resync", "//pos1", "//pos2", "//set", "//walls",
                          "//replace", "//replacenear", "//paint", "//unpaint", "//strip",
                          "//undo", "//redo", "//copy", "//paste", "//rotate", "//up",
                          "//sphere", "//cyl", "//hsphere", "//hcyl"})
        CHECK(ewb::we_find(v) != nullptr, v);

    // Every row carries a usage string, or /help would print a blank line.
    for (const ewb::WeSpec& s : ewb::we_specs()) {
        CHECK(s.usage && s.usage[0], "row has usage text");
        CHECK(s.min_level >= ewb::WE_LEVEL_VISITOR && s.min_level <= ewb::WE_LEVEL_OPERATOR,
              "row level in range");
        CHECK(s.min_args >= 0 && (s.max_args < 0 || s.max_args >= s.min_args), "row arity sane");
    }
}

static void test_permissions() {
    // Defect 2: every mutating command must be above visitor level. This is the
    // assertion that fails if someone adds a `//` row and forgets the floor.
    for (const ewb::WeSpec& s : ewb::we_specs()) {
        const std::string n = s.name;
        if (n.rfind("//", 0) == 0)
            CHECK(s.min_level >= ewb::WE_LEVEL_BUILDER, "every // command is above visitor");
    }
    CHECK(ewb::we_find("//set")->min_level == ewb::WE_LEVEL_BUILDER, "//set needs builder");
    CHECK(ewb::we_find("/help")->min_level == ewb::WE_LEVEL_VISITOR, "/help is open");
    CHECK(ewb::we_find("/resync")->min_level == ewb::WE_LEVEL_VISITOR, "/resync is open");

    // Defect 8: /tp to a coordinate is self-scoped, /tp to a player discloses
    // that player's exact position.
    CHECK(ewb::we_tp_required_level(3) == ewb::WE_LEVEL_BUILDER, "/tp x y z is builder");
    CHECK(ewb::we_tp_required_level(1) == ewb::WE_LEVEL_OPERATOR, "/tp <player> is operator");

    // A visitor's help lists nothing they cannot run.
    const auto visitor = ewb::we_help_lines(ewb::WE_LEVEL_VISITOR);
    CHECK(!visitor.empty(), "visitors see some help");
    for (const std::string& line : visitor)
        CHECK(line.rfind("//", 0) != 0, "visitor help lists no // command");
    CHECK(ewb::we_help_lines(ewb::WE_LEVEL_OPERATOR).size() > visitor.size(),
          "an operator sees more than a visitor");
}

static void test_arity() {
    const ewb::WeSpec* set = ewb::we_find("//set");
    CHECK(!ewb::we_args_ok(*set, 0), "//set needs a block");
    CHECK(ewb::we_args_ok(*set, 1) && ewb::we_args_ok(*set, 2), "//set takes 1 or 2");
    CHECK(!ewb::we_args_ok(*set, 3), "//set refuses 3");

    // `//paint` requires its colour; `//unpaint` is the command that means 0, so
    // a bare `//paint` must not become a silent strip.
    const ewb::WeSpec* paint = ewb::we_find("//paint");
    CHECK(!ewb::we_args_ok(*paint, 0) && ewb::we_args_ok(*paint, 1), "//paint needs a colour");
    CHECK(ewb::we_args_ok(*ewb::we_find("//unpaint"), 0), "//unpaint takes none");

    const ewb::WeSpec* msg = ewb::we_find("/msg");
    CHECK(ewb::we_args_ok(*msg, 9), "/msg is unbounded");
    CHECK(!ewb::we_args_ok(*msg, 1), "/msg needs a target and text");
}

// --- grammar -----------------------------------------------------------------

static void test_split() {
    auto a = ewb::we_split_args("//set 2 7");
    CHECK(a.size() == 3 && a[0] == "//set" && a[2] == "7", "simple split");

    auto b = ewb::we_split_args("  //paint   12  ");
    CHECK(b.size() == 2 && b[0] == "//paint" && b[1] == "12", "runs of spaces collapse");

    CHECK(ewb::we_split_args("").empty(), "empty line");
    CHECK(ewb::we_split_args("   ").empty(), "whitespace-only line");

    // /msg keeps its tail byte for byte, including the double space.
    CHECK(ewb::we_rest_after("/msg Bob hello  world", 2) == "hello  world", "rest keeps spacing");
    CHECK(ewb::we_rest_after("/r  hi there", 1) == "hi there", "rest after one token");
    CHECK(ewb::we_rest_after("/resync", 1).empty(), "no rest");
    CHECK(ewb::we_rest_after("/msg Bob", 2).empty(), "rest past the end is empty");
}

static void test_parse_int() {
    int v = 0;
    CHECK(ewb::we_parse_int("42", v) && v == 42, "plain int");
    CHECK(ewb::we_parse_int("-7", v) && v == -7, "negative");
    CHECK(ewb::we_parse_int("+3", v) && v == 3, "explicit plus");
    CHECK(!ewb::we_parse_int("5x", v), "trailing garbage refused (stoi would accept)");
    CHECK(!ewb::we_parse_int("", v), "empty refused");
    CHECK(!ewb::we_parse_int("-", v), "lone sign refused");
    CHECK(!ewb::we_parse_int("99999999999", v), "overflow refused");
    CHECK(!ewb::we_parse_int(" 4", v), "leading space refused");
}

static void test_parse_coord() {
    float out = 0;
    CHECK(ewb::we_parse_coord("~", 65536.0f, out) && out == 65536.0f, "~ is current");
    CHECK(ewb::we_parse_coord("~5", 100.0f, out) && out == 105.0f, "~n is relative");
    CHECK(ewb::we_parse_coord("~-5", 100.0f, out) && out == 95.0f, "~-n is relative");
    CHECK(ewb::we_parse_coord("33.92", 0.0f, out) && out > 33.9f && out < 33.93f, "absolute float");
    CHECK(!ewb::we_parse_coord("", 0.0f, out), "empty refused");
    CHECK(!ewb::we_parse_coord("~~1", 0.0f, out), "double tilde refused");
    CHECK(!ewb::we_parse_coord("abc", 0.0f, out), "name refused");
    CHECK(!ewb::we_parse_coord("1.2.3", 0.0f, out), "two dots refused");
}

static void test_parse_block_color() {
    int v = 0;
    CHECK(ewb::we_parse_block("0", v) && v == 0, "air is a legal block argument");
    CHECK(ewb::we_parse_block("127", v) && v == 127, "top of the block range");
    CHECK(!ewb::we_parse_block("128", v), "128 refused");
    CHECK(!ewb::we_parse_block("255", v), "the painted-base sentinel is not placeable");
    CHECK(!ewb::we_parse_block("-1", v), "negative refused");
    // Names resolve through the stage 3.6 tables (eden_names.h); an unknown name
    // is refused, not silently taken as 0. eden_names_test.cpp covers the tables.
    CHECK(ewb::we_parse_block("stone", v) && v == 2, "a block name resolves");
    CHECK(ewb::we_parse_block("STONE", v) && v == 2, "block names are case-insensitive");
    CHECK(ewb::we_parse_block("air", v) && v == 0, "the name 'air' is a legal block argument");
    CHECK(!ewb::we_parse_block("notablock", v), "an unknown block name is refused");

    CHECK(ewb::we_parse_color("0", v) && v == 0, "0 is the no-paint sentinel");
    CHECK(ewb::we_parse_color("54", v) && v == 54, "top of the palette");
    CHECK(ewb::we_parse_color("red", v) && v == 19, "a colour name resolves");
    CHECK(ewb::we_parse_color("unpaint", v) && v == 0, "'unpaint' resolves to the sentinel");
    CHECK(!ewb::we_parse_color("mauve", v), "an unknown colour name is refused");
    CHECK(!ewb::we_parse_color("55", v), "past the palette refused");
    CHECK((int)ewb::MAX_PAINT_INDEX == 54 && (int)ewb::MAX_BLOCK_TYPE == 127,
          "parser ranges track hardening.h");
}

// --- bounds ------------------------------------------------------------------

static void test_volume() {
    CHECK(ewb::we_volume(0, 0, 0, 0, 0, 0) == 1, "one cell");
    CHECK(ewb::we_volume(0, 0, 0, 1, 1, 1) == 8, "2x2x2");
    CHECK(ewb::we_volume(10, 0, 10, 0, 0, 0) == 11 * 1 * 11, "corners in either order");

    // Defect 1's arithmetic half: the modded patch multiplied ints. A selection
    // spanning the 24-bit key range must not wrap into a small positive number.
    const long long huge = ewb::we_volume(0, 0, 0, 0xFFFFFF, 255, 0xFFFFFF);
    CHECK(huge > ewb::WE_MAX_EDIT_CELLS, "a world-sized box is refused, not wrapped");
    CHECK(huge > 0, "world-sized volume stays positive");
}

static void test_selection_cap() {
    ewb::WeBox box;
    long long vol = 0;

    ewb::Selection sel;
    CHECK(!sel.box(box, ewb::WE_MAX_EDIT_CELLS, vol), "no points => no box");
    sel.set1(100, 40, 100);
    CHECK(!sel.box(box, ewb::WE_MAX_EDIT_CELLS, vol), "one point => no box");

    sel.set2(109, 40, 109);
    CHECK(sel.box(box, ewb::WE_MAX_EDIT_CELLS, vol), "two points => a box");
    CHECK(vol == 100, "10x1x10");
    CHECK(box.x0 == 100 && box.x1 == 109 && box.y0 == 40 && box.y1 == 40, "normalised");

    // Corners given "backwards" normalise to the same box.
    ewb::Selection rev;
    rev.set1(109, 40, 109);
    rev.set2(100, 40, 100);
    ewb::WeBox rbox;
    long long rvol = 0;
    CHECK(rev.box(rbox, ewb::WE_MAX_EDIT_CELLS, rvol) && rvol == vol &&
          rbox.x0 == box.x0 && rbox.z1 == box.z1, "corner order does not matter");

    // ⚠️ Defect 1, the disqualifying one: `//copy` in the community patch read
    // the selection with no cap at all and then iterated the box under the world
    // lock. Reading is where the cap lives now, so there is no accessor a new
    // command could use to reintroduce it.
    ewb::Selection wide;
    wide.set1(0, 0, 0);
    wide.set2(9999, 255, 9999);
    long long wvol = 0;
    CHECK(!wide.box(box, ewb::WE_MAX_EDIT_CELLS, wvol), "an oversized selection is refused");
    CHECK(wvol == 10000LL * 256LL * 10000LL, "...and reports the size it refused");
    CHECK(wide.complete(), "the points are still set — only reading is refused");
}

static void test_shapes() {
    CHECK(ewb::we_in_sphere(0, 0, 0, 4), "centre is inside");
    CHECK(ewb::we_in_sphere(4, 0, 0, 4), "surface is inside");
    CHECK(!ewb::we_in_sphere(5, 0, 0, 4), "outside is outside");
    CHECK(!ewb::we_in_sphere(3, 3, 0, 4), "corner of the box is outside the ball");

    // A hollow shell is the solid ball minus the next radius in — so every shell
    // cell is a ball cell, and the centre never is.
    CHECK(!ewb::we_in_sphere_shell(0, 0, 0, 4), "shell is hollow at the centre");
    CHECK(ewb::we_in_sphere_shell(4, 0, 0, 4), "shell includes the surface");
    for (int dx = -5; dx <= 5; ++dx)
        for (int dy = -5; dy <= 5; ++dy)
            for (int dz = -5; dz <= 5; ++dz)
                if (ewb::we_in_sphere_shell(dx, dy, dz, 4))
                    CHECK(ewb::we_in_sphere(dx, dy, dz, 4), "shell is a subset of the ball");

    CHECK(ewb::we_in_disc(0, 0, 3) && ewb::we_in_disc(3, 0, 3) && !ewb::we_in_disc(4, 0, 3),
          "cylinder cross-section");
    CHECK(!ewb::we_in_ring(0, 0, 3) && ewb::we_in_ring(3, 0, 3), "hollow cylinder wall");
}

// --- undo / redo -------------------------------------------------------------

static std::vector<ewb::WeEdit> make_batch(size_t n, unsigned char newType) {
    std::vector<ewb::WeEdit> b;
    b.reserve(n);
    for (size_t i = 0; i < n; ++i)
        b.push_back({(int)i, 40, 0, 0, 0, newType, 0});
    return b;
}

static void test_undo_roundtrip() {
    ewb::UndoStore store;
    std::vector<ewb::WeEdit> out;
    CHECK(!store.take_undo(out), "nothing to undo");
    CHECK(!store.take_redo(out), "nothing to redo");

    store.record(make_batch(4, 2));
    store.record(make_batch(4, 3));
    CHECK(store.undo.size() == 2 && store.redo.empty(), "two batches recorded");

    CHECK(store.take_undo(out) && out[0].newType == 3, "undo takes the newest batch");
    CHECK(store.undo.size() == 1 && store.redo.size() == 1, "batch moved to redo");
    CHECK(store.take_redo(out) && out[0].newType == 3, "redo takes it back");
    CHECK(store.undo.size() == 2 && store.redo.empty(), "and it lands back on undo");

    // A fresh edit invalidates the redo future.
    store.take_undo(out);
    CHECK(store.redo.size() == 1, "redo populated");
    store.record(make_batch(1, 9));
    CHECK(store.redo.empty(), "a new edit clears redo");

    const auto fwd = make_batch(3, 5);
    const auto back = ewb::we_invert(fwd);
    CHECK(back.size() == fwd.size(), "inverse is the same length");
    CHECK(back[0].newType == fwd[0].oldType && back[0].oldType == fwd[0].newType,
          "inverse swaps before and after");
    CHECK(ewb::we_invert(back)[0].newType == fwd[0].newType, "inverting twice is identity");
}

static void test_undo_budget() {
    // ⚠️ Defect 5: the community patch capped undo at 10 *batches*, so a player
    // could hold ~28 MB and 64 of them ~1.8 GB. The bound is bytes now, across
    // the undo and redo stacks together, evicting oldest-first.
    ewb::UndoStore store;
    store.budget_bytes = 10 * sizeof(ewb::WeEdit);

    for (int i = 0; i < 20; ++i) store.record(make_batch(3, (unsigned char)i));
    CHECK(store.bytes() <= store.budget_bytes, "budget respected after many batches");
    CHECK(store.undo.size() == 3, "oldest batches evicted, newest kept");
    CHECK(store.undo.back()[0].newType == 19, "the newest batch survives");

    // The redo stack counts against the same budget — otherwise undoing
    // everything would double a player's footprint.
    ewb::UndoStore two;
    two.budget_bytes = 6 * sizeof(ewb::WeEdit);
    two.record(make_batch(3, 1));
    two.record(make_batch(3, 2));
    std::vector<ewb::WeEdit> out;
    two.take_undo(out);
    CHECK(two.bytes() <= two.budget_bytes, "undo+redo share one budget");

    // One batch bigger than the whole budget is kept, not silently dropped: it
    // is already bounded by WE_MAX_EDIT_CELLS, and losing the undo for the edit
    // you just made is the worse failure.
    ewb::UndoStore tiny;
    tiny.budget_bytes = sizeof(ewb::WeEdit);
    tiny.record(make_batch(50, 7));
    CHECK(tiny.undo.size() == 1, "an over-budget batch is still undoable");

    // The defaults have to be consistent with each other, or a single full-cap
    // edit would evict itself.
    CHECK(ewb::WE_UNDO_BUDGET_BYTES >= (size_t)(ewb::WE_MAX_EDIT_CELLS * (long long)sizeof(ewb::WeEdit)),
          "the default budget holds at least one full-cap batch");
}

// --- clipboard ---------------------------------------------------------------

static void test_rotate() {
    std::vector<ewb::ClipCell> clip = {{1, 0, 0, 2, 0}, {0, 0, 1, 3, 0}};
    ewb::we_rotate_clipboard(clip, 1);
    CHECK(clip[0].rx == 0 && clip[0].rz == 1, "+x rotates to +z");
    CHECK(clip[1].rx == -1 && clip[1].rz == 0, "+z rotates to -x");

    // Four quarter turns are the identity — the property that holds whichever
    // way `we_rotate_slope` turns out to point (plan §0.5.4 is unsettled; stage
    // 3.6 decides the direction).
    std::vector<ewb::ClipCell> ramp = {{3, 1, -2, 25, 4}, {-1, 0, 5, 41, 0}, {2, 2, 2, 7, 9}};
    const std::vector<ewb::ClipCell> orig = ramp;
    ewb::we_rotate_clipboard(ramp, 4);
    for (size_t i = 0; i < ramp.size(); ++i)
        CHECK(ramp[i].rx == orig[i].rx && ramp[i].ry == orig[i].ry &&
              ramp[i].rz == orig[i].rz && ramp[i].type == orig[i].type,
              "four 90-degree steps are the identity");

    ewb::we_rotate_clipboard(ramp, 0);
    CHECK(ramp[0].rx == orig[0].rx, "zero steps is a no-op");

    // 180 twice is also the identity, and y is never touched.
    ewb::we_rotate_clipboard(ramp, 2);
    ewb::we_rotate_clipboard(ramp, 2);
    for (size_t i = 0; i < ramp.size(); ++i)
        CHECK(ramp[i].rx == orig[i].rx && ramp[i].ry == orig[i].ry && ramp[i].rz == orig[i].rz,
              "180 is an involution and height is preserved");

    // Slope ids cycle within their own family of four; non-slopes are untouched.
    for (int start : {24, 28, 32, 36, 40, 44, 48, 52})
        for (int off = 0; off < 4; ++off) {
            const int r = ewb::we_rotate_slope(start + off);
            CHECK(r >= start && r <= start + 3, "a slope stays in its family");
            CHECK(ewb::we_rotate_slope(ewb::we_rotate_slope(ewb::we_rotate_slope(r))) == start + off,
                  "four steps return the original id");
        }
    CHECK(ewb::we_rotate_slope(2) == 2, "stone is not a slope");
    CHECK(ewb::we_rotate_slope(56) == 56, "past the slope range is untouched");
}

int main() {
    test_table();
    test_permissions();
    test_arity();
    test_split();
    test_parse_int();
    test_parse_coord();
    test_parse_block_color();
    test_volume();
    test_selection_cap();
    test_shapes();
    test_undo_roundtrip();
    test_undo_budget();
    test_rotate();

    if (g_fail) {
        std::fprintf(stderr, "\n%d check(s) failed.\n", g_fail);
        return 1;
    }
    std::printf("worldedit_test: all checks passed.\n");
    return 0;
}
