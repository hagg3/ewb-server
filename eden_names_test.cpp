// eden_names_test.cpp — offline checks for ROADMAP-SERVER stage 3.6: the
// community block / paint / character name tables (eden_names.h).
//
//   clang++ -std=c++17 -O2 -Wall eden_names_test.cpp -o eden_names_test
//   ./eden_names_test
//
// The tables are pure data harvested from server_posix_modded.cpp. These checks
// pin the round-trip (name -> id -> name), the range edges, the documented
// aliases and sentinels, and the substring search used by /searchblocks and
// /searchcolors. They do NOT assert the names are *correct* — plan §0.5.1
// records the unresolved conflicts with the companion editor's list.

#include <cstdio>
#include <string>

#include "eden_names.h"

static int g_fail = 0;

#define CHECK(cond, msg)                                                             \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__);    \
            ++g_fail;                                                                \
        }                                                                            \
    } while (0)

static void test_blocks() {
    int n = 0;
    ewb::eden_block_table(n);
    CHECK(n == 112, "block table covers ids 0..111");

    CHECK(ewb::eden_block_id("air") == 0, "air is 0");
    CHECK(ewb::eden_block_id("stone") == 2, "stone is 2");
    CHECK(ewb::eden_block_id("tnt") == 9, "tnt is 9 (corroborates SV_TNT)");
    CHECK(ewb::eden_block_id("firework") == 65, "firework is 65 (SV_FIREWORK)");
    CHECK(ewb::eden_block_id("metal") == 74, "metal is 74 (SV_STEEL)");
    CHECK(ewb::eden_block_id("expand_metal") == 111, "top of the table");

    CHECK(ewb::eden_block_id("STONE") == 2, "lookup is case-insensitive");
    CHECK(ewb::eden_block_id("adminium") == 1, "adminium aliases bedrock");
    CHECK(ewb::eden_block_id("bedrock") == 1, "bedrock is 1");

    CHECK(ewb::eden_block_id("notablock") == -1, "unknown name is -1");
    CHECK(ewb::eden_block_id("42") == -1, "numeric strings are not resolved here");

    // round-trip
    for (int i = 0; i < n; ++i) {
        const char* name = ewb::eden_block_name(i);
        CHECK(*name != '\0', "every id in range has a name");
        // 71 deliberately does not round-trip (it shares "gem" with 16 in the
        // source; exposed as "gem71").
        if (i != 71)
            CHECK(ewb::eden_block_id(name) == i, "block name round-trips to its id");
    }
    CHECK(ewb::eden_block_id("gem") == 16, "the shared name 'gem' resolves to 16");
    CHECK(std::string(ewb::eden_block_name(71)) == "gem71", "71 is exposed as gem71");

    CHECK(std::string(ewb::eden_block_name(112)) == "", "just past the table has no name");
    CHECK(std::string(ewb::eden_block_name(-1)) == "", "negative id has no name");
}

static void test_paints() {
    int n = 0;
    ewb::eden_paint_table(n);
    CHECK(n == 55, "paint table is the 0 sentinel + 54 colours");

    CHECK(ewb::eden_paint_id("unpaint") == 0, "unpaint is the 0 sentinel");
    CHECK(ewb::eden_paint_id("none") == 0, "none aliases the sentinel");
    CHECK(ewb::eden_paint_id("base") == 0, "base aliases the sentinel");
    CHECK(ewb::eden_paint_id("original") == 0, "original aliases the sentinel");
    CHECK(ewb::eden_paint_id("scrape") == 0, "scrape aliases the sentinel");

    // modded getColorId returns COLOR_MAP[name] + 1
    CHECK(ewb::eden_paint_id("pale_red") == 1, "pale_red is 1 (COLOR_MAP 0 + 1)");
    CHECK(ewb::eden_paint_id("white") == 9, "white closes the pale row");
    CHECK(ewb::eden_paint_id("red") == 19, "base red is 19");
    CHECK(ewb::eden_paint_id("grey") == 27, "base grey closes the base row");
    CHECK(ewb::eden_paint_id("black") == 54, "black is the last colour");

    CHECK(ewb::eden_paint_id("RED") == 19, "lookup is case-insensitive");
    CHECK(ewb::eden_paint_id("chartreuse") == -1, "unknown colour is -1");

    for (int i = 1; i < n; ++i) {
        const char* name = ewb::eden_paint_name(i);
        CHECK(*name != '\0', "every colour id has a name");
        CHECK(ewb::eden_paint_id(name) == i, "colour name round-trips to its id");
    }
    CHECK(std::string(ewb::eden_paint_name(55)) == "", "past the palette has no name");
}

static void test_chars() {
    int n = 0;
    ewb::eden_char_table(n);
    CHECK(n == 7, "seven character types (plan §0.5.3)");
    CHECK(std::string(ewb::eden_char_name(0)) == "Moof", "type 0 is Moof");
    CHECK(std::string(ewb::eden_char_name(6)) == "Stalker", "type 6 is Stalker");
    CHECK(std::string(ewb::eden_char_name(7)) == "", "type 7 is out of range");
    CHECK(std::string(ewb::eden_char_name(17)) == "", "the JOIN-field-2 value 17 is not a type index");
}

static void test_search() {
    int n = 0;
    const char* const* blocks = ewb::eden_block_table(n);

    std::string hits;
    int c = ewb::eden_search(blocks, n, "slope_nw", false, 400, hits);
    CHECK(c == 4, "four *_slope_nw blocks");
    CHECK(hits.find("stone_slope_nw=40") != std::string::npos, "search reports name=id");

    hits.clear();
    c = ewb::eden_search(blocks, n, "STONE", false, 4096, hits);
    CHECK(c > 0 && hits.find("stone=2") != std::string::npos, "search is case-insensitive");

    hits.clear();
    c = ewb::eden_search(blocks, n, "zzznope", false, 4096, hits);
    CHECK(c == 0 && hits.empty(), "no matches leaves the buffer empty");

    // budget truncation
    hits.clear();
    c = ewb::eden_search(blocks, n, "expand", false, 40, hits);
    CHECK(hits.size() <= 45 && hits.find("...") != std::string::npos, "search truncates on budget");

    int m = 0;
    const char* const* paints = ewb::eden_paint_table(m);
    hits.clear();
    ewb::eden_search(paints, m, "pale", true, 4096, hits);
    CHECK(hits.find("unpaint") == std::string::npos, "colour search skips the 0 sentinel");
    CHECK(hits.find("pale_red=1") != std::string::npos, "colour search emits palette ids");
}

int main() {
    test_blocks();
    test_paints();
    test_chars();
    test_search();
    if (g_fail) { std::fprintf(stderr, "%d check(s) failed\n", g_fail); return 1; }
    std::puts("eden_names_test: all checks passed");
    return 0;
}
