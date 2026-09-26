// topmap_test.cpp — offline checks for ROADMAP-SERVER stage 8.5: topmap.h's
// request parsing, the per-column surface rule against the base profile, the
// chunk-column walk, and WorldStore::for_each_in_column — the whole verb checked
// against a brute-force reference over a random world.
//
//   c++ -std=c++17 -O2 -Wall topmap_test.cpp -o topmap_test
//   ./topmap_test

#include <cstdio>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "topmap.h"
#include "world_store.h"

using namespace ewb;

static int g_fail = 0;

#define CHECK(cond, msg)                                                          \
    do {                                                                          \
        if (!(cond)) {                                                            \
            std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_fail;                                                             \
        }                                                                         \
    } while (0)

static std::vector<std::string> F(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream in(s);
    while (std::getline(in, cur, ':')) out.push_back(cur);
    return out;
}

static void test_parse() {
    TopmapReq r;
    std::string err;
    CHECK(topmap_parse(F("100:200:110:205:1"), r, err), "small box parses");
    CHECK(r.x0 == 100 && r.z0 == 200 && r.w == 11 && r.h == 6 && r.step == 1, "inclusive sample counts");
    CHECK(topmap_parse(F("110:205:100:200:1"), r, err) && r.x0 == 100 && r.z0 == 200 && r.w == 11,
          "reversed corners normalise");
    CHECK(topmap_parse(F("0:0:10:10:4"), r, err) && r.w == 3 && r.lastX() == 8,
          "a box not a multiple of step: last sample is inside the box, header reports it");
    CHECK(topmap_parse(F("5:5:5:5:7"), r, err) && r.w == 1 && r.h == 1, "a single column");

    CHECK(topmap_parse(F("0:0:255:255:1"), r, err) && r.w == 256 && r.h == 256, "256x256 is the cap");
    CHECK(!topmap_parse(F("0:0:256:0:1"), r, err), "257 samples refused");
    CHECK(err.find("use step >= 2") != std::string::npos, "the refusal names a step that fits");
    // The suggested step must actually fit, for awkward spans too.
    for (int span : {256, 300, 1000, 65535, 100000, 16777215}) {
        TopmapReq a;
        std::string e;
        const std::string box = "0:0:" + std::to_string(span) + ":0:1";
        if (topmap_parse(F(box), a, e)) continue;
        const size_t p = e.find("step >= ");
        const int need = std::atoi(e.c_str() + p + 8);
        CHECK(topmap_parse(F("0:0:" + std::to_string(span) + ":0:" + std::to_string(need)), a, e),
              "the suggested step fits");
        if (need > 1)
            CHECK(!topmap_parse(F("0:0:" + std::to_string(span) + ":0:" + std::to_string(need - 1)), a, e),
                  "and it is the smallest that does");
    }

    CHECK(!topmap_parse(F("0:0:10:10:0"), r, err), "step 0 refused");
    CHECK(!topmap_parse(F("0:0:10:10:-3"), r, err), "negative step refused");
    CHECK(!topmap_parse(F("-1:0:10:10:1"), r, err), "negative coordinate refused");
    CHECK(!topmap_parse(F("0:0:16777216:10:1"), r, err), "coordinate past 24 bits refused");
    CHECK(!topmap_parse(F("0:0:1x:10:1"), r, err), "non-number refused");
    CHECK(!topmap_parse(F("0:0:10:1"), r, err), "four fields refused");
    CHECK(!topmap_parse(F("0:0:10:10:1:9"), r, err), "six fields refused");
}

static void test_tokens() {
    const BaseProfile base = eden_default_profile();
    TopmapReq r;
    std::string err;
    topmap_parse(F("0:0:7:0:1"), r, err);
    TopmapGrid g(r, base);
    // 0: untouched.
    // 1: grass mined -> dirt below.
    g.feed(1, 0, 32, 0, 0);
    // 2: a block built on top.
    g.feed(2, 0, 33, 5, 0);
    // 3: grass painted -> grass (8), colour 12.
    g.feed(3, 0, 32, 255, 12);
    // 4: a tower with a hole in the middle; the top wins.
    g.feed(4, 0, 40, 7, 3);
    g.feed(4, 0, 38, 0, 0);
    g.feed(4, 0, 36, 7, 0);
    // 5: a column mined out to nothing.
    for (int y = 0; y <= 32; ++y) g.feed(5, 0, y, 0, 0);
    // 6: something changed underground only — the surface is still grass.
    g.feed(6, 0, 10, 0, 0);
    // 7: a floating mined cell above the surface (a build that was removed).
    g.feed(7, 0, 50, 0, 0);

    CHECK(g.token(0, 0) == "-", "untouched column");
    CHECK(g.token(1, 0) == "31,3,0", "mined grass reads as the dirt beneath");
    CHECK(g.token(2, 0) == "33,5,0", "a built block on top");
    CHECK(g.token(3, 0) == "32,8,12", "painted base: base type + paint, never 255");
    CHECK(g.token(4, 0) == "40,7,3", "the highest solid cell wins");
    CHECK(g.token(5, 0) == ".", "nothing solid left");
    CHECK(g.token(6, 0) == "32,8,0", "underground change: surface is the grass");
    CHECK(g.token(7, 0) == "32,8,0", "stored air above the surface doesn't hide it");

    const std::string rep = g.render();
    CHECK(rep == "ok: topmap 0 0 7 0 1 8 1\n- 31,3,0 33,5,0 32,8,12 40,7,3 . 32,8,0 32,8,0\n",
          "render: header + one row");

    // Out-of-grid feeds are ignored, not written somewhere else.
    g.feed(8, 0, 40, 1, 0);
    g.feed(0, 1, 40, 1, 0);
    g.feed(0, 0, 300, 1, 0);
    CHECK(g.token(0, 0) == "-", "out-of-range feeds are dropped");
}

static void test_chunk_walk() {
    // Every sample visited exactly once, in the chunk column it claims.
    for (const char* box : {"0:0:255:255:1", "3:5:1000:900:7", "65530:65530:65600:65540:16",
                            "100:100:100:100:1", "0:0:4000:4000:17"}) {
        TopmapReq r;
        std::string err;
        CHECK(topmap_parse(F(box), r, err), "walk box parses");
        std::set<std::pair<int, int>> seen;
        bool inChunk = true;
        std::set<std::pair<int, int>> chunks;
        size_t calls = 0;
        topmap_for_each_chunk_column(r, [&](int cx, int cz, const std::vector<TopmapSample>& s) {
            ++calls;
            CHECK(!s.empty(), "no empty chunk-column batches");
            CHECK(chunks.insert({cx, cz}).second, "each chunk column visited once");
            for (const TopmapSample& t : s) {
                if ((t.x >> 4) != cx || (t.z >> 4) != cz) inChunk = false;
                if (t.x != r.x0 + t.i * r.step || t.z != r.z0 + t.j * r.step) inChunk = false;
                seen.insert({t.i, t.j});
            }
        });
        CHECK(inChunk, "every sample lies in the chunk column it was batched under");
        CHECK(seen.size() == size_t(r.w) * size_t(r.h), "every sample visited exactly once");
        (void)calls;
    }
}

static void test_column_visitor() {
    WorldStore ws;
    ws.set(20, 5, 30, 3, 0);
    ws.set(20, 40, 30, 0, 0);     // mined
    ws.set(20, 200, 30, 7, 9);
    ws.set(21, 40, 30, 5, 0);     // neighbour column — must not appear
    ws.set(20, 40, 31, 5, 0);     // neighbour column — must not appear
    std::vector<int> ys;
    std::vector<int> types;
    ws.for_each_in_column(20, 30, [&](int y, unsigned char t, unsigned char) { ys.push_back(y); types.push_back(t); });
    CHECK((ys == std::vector<int>{5, 40, 200}), "column visitor: this column only, bottom to top");
    CHECK((types == std::vector<int>{3, 0, 7}), "column visitor: logical types (mined reads 0)");
    int n = 0;
    ws.for_each_in_column(-1, 30, [&](int, unsigned char, unsigned char) { ++n; });
    ws.for_each_in_column(20, 0x1000000, [&](int, unsigned char, unsigned char) { ++n; });
    CHECK(n == 0, "out-of-world columns visit nothing");
}

// The whole verb, as the server runs it, against a brute-force reference.
static void test_against_brute_force() {
    const BaseProfile base = eden_default_profile();
    std::mt19937 rng(42);
    WorldStore ws;
    std::uniform_int_distribution<int> dx(65500, 65700), dy(0, 80), dt(0, 12), dc(0, 5);
    for (int k = 0; k < 40000; ++k) {
        const int x = dx(rng), y = dy(rng), z = dx(rng);
        int t = dt(rng);
        if (t == 12) t = 255;              // some painted base
        if (t == 11) t = 0;                // some mined
        ws.set(x, y, z, (unsigned char)t, (unsigned char)(t == 255 ? 1 + dc(rng) : dc(rng)));
    }
    for (const char* box : {"65490:65490:65710:65710:1", "65490:65490:65710:65710:3", "65600:65600:65600:65600:1"}) {
        TopmapReq r;
        std::string err;
        CHECK(topmap_parse(F(box), r, err), "box parses");
        TopmapGrid g(r, base);
        topmap_for_each_chunk_column(r, [&](int, int, const std::vector<TopmapSample>& s) {
            for (const TopmapSample& t : s)
                ws.for_each_in_column(t.x, t.z, [&](int y, unsigned char ty, unsigned char c) { g.feed(t.i, t.j, y, ty, c); });
        });
        bool agree = true;
        for (int j = 0; j < r.h && agree; ++j)
            for (int i = 0; i < r.w && agree; ++i) {
                const int x = r.x0 + i * r.step, z = r.z0 + j * r.step;
                bool touched = false;
                int topY = -1;
                unsigned topT = 0, topC = 0;
                for (int y = 0; y < TOPMAP_HEIGHT; ++y) {
                    WorldCell c;
                    unsigned t, col;
                    if (ws.get(x, y, z, c)) {
                        touched = true;
                        t = c.type == 255 ? base.at(y).type : c.type;
                        col = c.color;
                    } else {
                        t = base.at(y).type;
                        col = base.at(y).paint;
                    }
                    if (t != 0) { topY = y; topT = t; topC = col; }
                }
                std::string want;
                if (!touched) want = "-";
                else if (topY < 0) want = ".";
                else want = std::to_string(topY) + "," + std::to_string(topT) + "," + std::to_string(topC);
                if (g.token(i, j) != want) {
                    std::fprintf(stderr, "  mismatch at %d,%d: got %s want %s\n", x, z,
                                 g.token(i, j).c_str(), want.c_str());
                    agree = false;
                }
            }
        CHECK(agree, "topmap agrees with a brute-force surface scan");
    }
}

int main() {
    test_parse();
    test_tokens();
    test_chunk_walk();
    test_column_visitor();
    test_against_brute_force();
    if (g_fail) {
        std::fprintf(stderr, "%d check(s) FAILED\n", g_fail);
        return 1;
    }
    std::printf("All topmap_test checks passed.\n");
    return 0;
}
