// protocol_test.cpp — offline checks for ROADMAP-SERVER stages 1.5, 1.7 and 1.10:
// the sign sidecar / `SIGNP` wire formats (including a player's sign write and its
// slot upsert, and removing signs with their block — stage 7.2), and the hardening primitives (username validation, `ACTION`
// payload validation, the world cell cap headroom, token bucket, per-IP connect
// limiter, constant-time password compare, per-IP failed-auth limiter, text
// sanitisation) — plus the explode.h TNT/paint chain worklist and its
// explosion chain fan-out + work budget (stages 7.7 / 7.19), the movement-field validator
// and spawn formatter of stage 7.16, and the `eden_motd.txt` welcome-message
// sidecar (motd_store.h). Stage 8.2 adds the explosion chain's protectedAt hook
// (protected cells untouched, a protected TNT not chained, EXPLODE_REACH tight) and
// zone_guard.h: the restore wire, the capped cell set, the revert queue's
// coalescing, and the audit folding.
//
//   clang++ -std=c++17 -O2 -Wall protocol_test.cpp -o protocol_test
//   ./protocol_test
//
// Everything under test is pure and takes its clock as a parameter, so nothing
// here sleeps or opens a socket. The join *sequence* (1.3), `PONG` (1.4) and the
// chat kill-switch removal (1.6) are wire-order properties of handleClient and are
// covered by the live socket test in the stage 1.8 ladder, not here.

#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <random>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include <cstdint>
#include <unordered_map>

#include "explode.h"
#include "world_store.h"
#include "zones.h"
#include "zone_guard.h"
#include "hardening.h"
#include "motd_store.h"
#include "sign_store.h"
#include "spawn_store.h"

static int g_fail = 0;

#define CHECK(cond, msg)                                                             \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__);    \
            ++g_fail;                                                                \
        }                                                                            \
    } while (0)

// --- signs (stage 1.5) -------------------------------------------------------

static void test_sign_parse() {
    ewb::Sign s;
    bool skip = false;

    // The capture's own shape: SIGNP:server:65467:34:65115:0:27:0:step 3: burn the door
    CHECK(ewb::parse_sign_line("65467:34:65115:0:27:0:step 3: burn the door", s, skip),
          "capture-shaped line parses");
    CHECK(s.x == 65467 && s.y == 34 && s.z == 65115, "coords");
    CHECK(s.a == 0 && s.b == 27 && s.c == 0, "a/b/c verbatim");
    // The whole point of text-last: only the first six fields are split.
    CHECK(s.text == "step 3: burn the door", "text keeps its own ':'");

    // Round-trip back onto the wire, `server` sender token included.
    CHECK(ewb::format_signp(s) == "SIGNP:server:65467:34:65115:0:27:0:step 3: burn the door\n",
          "format_signp round-trips the capture line");

    // Negative a/b/c are legal (semantics unknown — we emit whatever the file holds).
    CHECK(ewb::parse_sign_line("100:0:200:-1:-2:-3:x", s, skip) && s.a == -1 && s.c == -3,
          "negative a/b/c survive");

    // Single-character texts appear in the capture (q, a, s, d, o, t).
    CHECK(ewb::parse_sign_line("1:1:1:0:0:0:q", s, skip) && s.text == "q", "single-char text");

    // Empty text is accepted — a sign with no text still occupies a cell.
    CHECK(ewb::parse_sign_line("1:1:1:0:0:0:", s, skip) && s.text.empty(), "empty text");

    // CRLF sidecar (an operator editing on Windows).
    CHECK(ewb::parse_sign_line("1:2:3:4:5:6:hello\r", s, skip) && s.text == "hello",
          "CRLF line strips the \\r");

    // Blank lines and comments are skipped without complaint.
    for (const char* l : {"", "   ", "# a comment", "\t# indented comment"}) {
        CHECK(!ewb::parse_sign_line(l, s, skip), "comment/blank does not parse");
        CHECK(skip, "comment/blank is a skip, not an error");
    }

    // Malformed lines are errors (skip == false), so the loader can warn.
    for (const char* l : {"1:2:3:4:5", "1:2:3:4:5:6", "a:2:3:4:5:6:t", "1:2:3:4:5:x:t",
                          "1::3:4:5:6:t", "1:2:3:4:5:6.5:t"}) {
        CHECK(!ewb::parse_sign_line(l, s, skip), "malformed line rejected");
        CHECK(!skip, "malformed line is an error, not a skip");
    }

    // Coordinate ranges match what ACTION/REGION accept.
    CHECK(!ewb::parse_sign_line("-1:0:0:0:0:0:t", s, skip), "negative x rejected");
    CHECK(!ewb::parse_sign_line("0:0:-1:0:0:0:t", s, skip), "negative z rejected");
    CHECK(!ewb::parse_sign_line("16777216:0:0:0:0:0:t", s, skip), "x past 24 bits rejected");
    CHECK(!ewb::parse_sign_line("0:256:0:0:0:0:t", s, skip), "y past the world height rejected");
    CHECK(ewb::parse_sign_line("16777215:255:16777215:0:0:0:t", s, skip), "the edges are legal");
}

static void test_sign_text_cannot_break_framing() {
    // A sidecar written by something other than a human — a control byte in the
    // text would desynchronise the client's '\n' line framing.
    ewb::Sign s;
    bool skip = false;
    CHECK(ewb::parse_sign_line(std::string("1:2:3:0:0:0:evil\x01") + "\x7F" + "text", s, skip),
          "line with control bytes still parses");
    CHECK(s.text == "eviltext", "control bytes are stripped at parse");

    // ...and again at format time, since format_signp is the only thing that writes
    // a SIGNP to a socket and must not trust its input.
    ewb::Sign forged;
    forged.x = 1; forged.y = 2; forged.z = 3;
    forged.text = "a\nSIGNP:server:0:0:0:0:0:0:forged";
    const std::string line = ewb::format_signp(forged);
    CHECK(line.find('\n') == line.size() - 1, "format_signp emits exactly one newline, at the end");

    // Over-long text is bounded.
    forged.text = std::string(ewb::SIGN_TEXT_MAX + 100, 'x');
    CHECK(ewb::format_signp(forged).size() < ewb::SIGN_TEXT_MAX + 64, "text is length-capped");
}

static void test_sign_burst() {
    std::vector<ewb::Sign> signs;
    for (int i = 0; i < 3; ++i) signs.push_back({i, 1, 2, 0, 0, 0, "s" + std::to_string(i)});
    const std::string blob = ewb::format_sign_burst(signs);
    size_t lines = 0;
    for (char c : blob) if (c == '\n') ++lines;
    CHECK(lines == 3, "one line per sign");
    CHECK(blob.rfind("SIGNP:server:0:1:2:0:0:0:s0\n", 0) == 0, "first line");
    // No terminator: the capture shows a burst and then nothing. LISTP's `END` is a
    // different subprotocol — do not generalize it here.
    CHECK(blob.find("END") == std::string::npos, "the burst has no terminator");
    CHECK(ewb::format_sign_burst({}).empty(), "no signs means an empty burst, not a marker");
}

// --- player sign writes -----------------------------------------------------

static void test_client_signp_parse() {
    ewb::Sign s;
    // The retail client's own line (LIVE-FINDINGS 2026-09-08): no sender field.
    CHECK(ewb::parse_client_signp("SIGNP:65543:32:65546:3:2:0:Testing phase b", s),
          "retail-client sign write parses");
    CHECK(s.x == 65543 && s.y == 32 && s.z == 65546 && s.a == 3 && s.b == 2 && s.c == 0,
          "client sign coords and a/b/c");
    CHECK(s.text == "Testing phase b", "client sign text");

    CHECK(ewb::parse_client_signp("SIGNP:1:2:3:0:0:0:time 12:30: meet here", s) &&
          s.text == "time 12:30: meet here", "client sign text keeps its own ':'");
    CHECK(ewb::parse_client_signp("SIGNP:1:2:3:0:0:0:evil\x01text\r", s) && s.text == "eviltext",
          "client sign text is sanitised");

    // Nobody writes a sign as `server`, and a write needs all seven fields.
    CHECK(!ewb::parse_client_signp("SIGNP:server:1:2:3:0:0:0:forged", s), "server-sender line refused");
    for (const char* l : {"SIGNP", "SIGNP:", "SIGNP:1:2:3", "SIGNQ:1:2:3:0:0:0:t", "1:2:3:0:0:0:t",
                          "SIGNP:# comment", "SIGNP:1:256:3:0:0:0:t", "SIGNP:-1:2:3:0:0:0:t"})
        CHECK(!ewb::parse_client_signp(l, s), "malformed client sign write refused");
}

static void test_sign_file_line_round_trip() {
    const ewb::Sign in{65414, 40, 65584, 1, 27, 2, "FLOOR 1: RESTAURANT"};
    const std::string line = ewb::format_sign_file_line(in);
    CHECK(line == "65414:40:65584:1:27:2:FLOOR 1: RESTAURANT\n", "sidecar line format");
    ewb::Sign out;
    bool skip = false;
    CHECK(ewb::parse_sign_line(line, out, skip) && out.x == in.x && out.a == in.a &&
          out.c == in.c && out.text == in.text, "sidecar line round-trips");
}

static void test_sign_upsert() {
    std::vector<ewb::Sign> v;
    ewb::Sign a{10, 34, 20, 0, 5, 0, "hello"};
    CHECK(ewb::upsert_sign(v, a, 10) == ewb::SignUpsert::Added && v.size() == 1, "new slot appends");
    CHECK(ewb::upsert_sign(v, a, 10) == ewb::SignUpsert::Unchanged && v.size() == 1,
          "an identical re-send is unchanged");

    a.text = "edited";
    CHECK(ewb::upsert_sign(v, a, 10) == ewb::SignUpsert::Replaced && v.size() == 1 &&
          v[0].text == "edited", "an edit replaces the sign in its slot");
    a.b = 9;
    CHECK(ewb::upsert_sign(v, a, 10) == ewb::SignUpsert::Replaced && v[0].b == 9,
          "a changed b is an edit, not a new sign");

    // Two signs on one block, different faces — the shape real imported worlds hold.
    ewb::Sign otherFace{10, 34, 20, 5, 38, 0, "other face"};
    CHECK(ewb::upsert_sign(v, otherFace, 10) == ewb::SignUpsert::Added && v.size() == 2,
          "another face of the same block is a separate sign");
    CHECK(v[0].text == "edited", "...and does not clobber the first");

    // A hand-edited sidecar with duplicates in one slot converges on one sign.
    v.push_back({10, 34, 20, 0, 1, 1, "stale dupe"});
    CHECK(ewb::upsert_sign(v, a, 10) == ewb::SignUpsert::Replaced && v.size() == 2,
          "duplicates in the slot are dropped");

    // The cap refuses new slots but never an edit.
    std::vector<ewb::Sign> full{{1, 1, 1, 0, 0, 0, "x"}};
    CHECK(ewb::upsert_sign(full, {2, 2, 2, 0, 0, 0, "y"}, 1) == ewb::SignUpsert::Full &&
          full.size() == 1, "a full list refuses a new slot");
    CHECK(ewb::upsert_sign(full, {1, 1, 1, 0, 0, 0, "z"}, 1) == ewb::SignUpsert::Replaced,
          "a full list still takes an edit");
}

// --- world cell cap ---------------------------------------------------------

static void test_cell_cap_headroom() {
    // Small worlds get the floor; big ones a quarter again; always a round number.
    CHECK(ewb::recommended_max_world_cells(0) == 1000000, "empty world: the headroom floor");
    CHECK(ewb::recommended_max_world_cells(3433) == 1100000, "rounded up to 100,000");
    CHECK(ewb::recommended_max_world_cells(2200053) == 3300000, "xen7-sized: +1M floor");
    CHECK(ewb::recommended_max_world_cells(13920369) == 17500000, "13.9M: +25%");
    for (size_t n : {size_t(0), size_t(1), size_t(3999999), size_t(4000000), size_t(13920369)}) {
        const size_t r = ewb::recommended_max_world_cells(n);
        CHECK(r >= n + ewb::WORLD_CELL_HEADROOM_MIN, "always leaves at least the floor");
        CHECK(r % 100000 == 0, "always a round 100,000");
        CHECK(ewb::cell_cap_state(n, r) == ewb::CellCapState::Ok, "the recommendation is not Low");
    }

    // The launch bug: the cap set to exactly the imported cell count.
    CHECK(ewb::cell_cap_state(13920369, 13920369) == ewb::CellCapState::Full, "cap == cells is Full");
    CHECK(ewb::cell_cap_state(5, 4) == ewb::CellCapState::Full, "over the cap is Full");
    CHECK(ewb::cell_cap_state(3700000, 4000000) == ewb::CellCapState::Low, "under a tenth left is Low");
    CHECK(ewb::cell_cap_state(3500000, 4000000) == ewb::CellCapState::Ok, "an eighth left is Ok");
    CHECK(ewb::cell_cap_state(0, 1) == ewb::CellCapState::Ok, "tiny cap, empty world");
}

// --- TNT/paint explosion chain (stage 7.7) -----------------------------------

// Minimal fake world: a map from packed key to a stored cell, nothing stored
// means "untouched terrain" — mirrors g_world's semantics without pulling in
// server_posix.cpp.
struct FakeWorld {
    std::unordered_map<uint64_t, ewb::ExplodeCell> cells;
    static uint64_t key(int x, int y, int z) {
        return (uint64_t(uint32_t(x)) << 40) | (uint64_t(uint32_t(z)) << 16) | uint64_t(uint16_t(y));
    }
};

static void test_explode_single_tnt_golden() {
    FakeWorld fw;
    size_t setCalls = 0;
    ewb::ExplodeWorld w;
    w.get = [&](int x, int y, int z, ewb::ExplodeCell& out) -> bool {
        auto it = fw.cells.find(FakeWorld::key(x, y, z));
        if (it == fw.cells.end()) return false;
        out = it->second;
        return true;
    };
    w.set = [&](int x, int y, int z, int type, int color) -> bool {
        ++setCalls;
        fw.cells[FakeWorld::key(x, y, z)] = {(unsigned char)type, (unsigned char)color};
        return true;  // no cap in this test
    };
    fw.cells[FakeWorld::key(100, 100, 100)] = {(unsigned char)w.tntType, 0};

    const ewb::ExplodeResult r = ewb::simExplode(w, 100, 100, 100);
    // Golden: an isolated TNT in an empty world, R=6 sphere, no chain — 923
    // distinct cells touched (1 explicit center-consume call, then every other
    // distinct cell the sphere's samples land on; repeat visits to an
    // already-air cell are skipped). A change here means the destroyed-cell
    // geometry changed, not just this refactor.
    CHECK(r.refused == 0 && r.truncated == 0, "an unbounded single TNT refuses nothing");
    CHECK(setCalls == 923, "an isolated TNT's blast touches the same cell count as before 7.7");
    CHECK(r.explosions == 1, "one blast");
    CHECK(r.visits == ewb::EXPLODE_VISITS_PER_BLAST && r.visits == 1037,
          "one blast reads exactly EXPLODE_VISITS_PER_BLAST cells");
}

// A dense field where every queried cell reports back another TNT block: the
// blast chains into an unbounded number of neighbours. Before 7.7 this was
// bounded only by recursion depth (still a huge, uncapped fan-out per level);
// 7.7 capped the blasts; 7.19 expresses that cap as a budget of cell visits.
static void test_explode_chain_budget() {
    size_t gets = 0;
    ewb::ExplodeWorld w;
    w.get = [&](int, int, int, ewb::ExplodeCell& out) -> bool {
        ++gets;
        out = {(unsigned char)9 /* SV_TNT */, 0};
        return true;
    };
    w.set = [](int, int, int, int, int) -> bool { return true; };
    w.tntType = 9;

    const size_t per = ewb::EXPLODE_VISITS_PER_BLAST;
    for (size_t budget : {per * 50, per * 50 + per - 1, ewb::EXPLODE_DEFAULT_MAX_VISITS}) {
        gets = 0;
        const ewb::ExplodeResult r = ewb::simExplode(w, 65536, 100, 65536, budget);
        CHECK(r.visits <= budget, "a chain never reads more cells than its budget");
        CHECK(r.visits == gets, "visits counts every read");
        CHECK(r.explosions == budget / per, "the budget is spent in whole blasts");
        CHECK(r.truncated > 0, "a chain cut short by the budget reports it");
        CHECK(r.refused == 0, "truncation is not reported as a cap refusal");
    }

    // A budget below one blast still runs the root: a single TNT always works.
    const ewb::ExplodeResult one = ewb::simExplode(w, 65536, 100, 65536, 1);
    CHECK(one.explosions == 1 && one.visits == per, "the root blast always runs");
}

// A small cluster finishes inside the default budget with nothing truncated,
// including the re-explosions of already-consumed TNT that the client's
// collect-then-recurse order produces.
static void test_explode_cluster_completes() {
    FakeWorld fw;
    ewb::ExplodeWorld w;
    w.get = [&](int x, int y, int z, ewb::ExplodeCell& out) -> bool {
        auto it = fw.cells.find(FakeWorld::key(x, y, z));
        if (it == fw.cells.end()) return false;
        out = it->second;
        return true;
    };
    w.set = [&](int x, int y, int z, int type, int color) -> bool {
        fw.cells[FakeWorld::key(x, y, z)] = {(unsigned char)type, (unsigned char)color};
        return true;
    };
    for (int x = 0; x < 3; ++x)
        for (int y = 0; y < 3; ++y)
            for (int z = 0; z < 3; ++z) fw.cells[FakeWorld::key(1000 + x, 100 + y, 1000 + z)] = {9, 0};

    const ewb::ExplodeResult r = ewb::simExplode(w, 1001, 101, 1001);
    CHECK(r.truncated == 0, "a 3x3x3 TNT block runs to completion under the default budget");
    CHECK(r.explosions > 27, "re-explosions of consumed TNT are kept (client order)");
    size_t left = 0;
    for (const auto& kv : fw.cells) left += kv.second.type == 9;
    CHECK(left == 0, "every TNT in the block is consumed");
}

// 7.22: a blast near the top (or bottom) of the world writes nothing outside
// [0, WS_WORLD_HEIGHT), and every cell it did write is visible to the REGION
// sweep. Backed by the real chunk store so the check is the one that matters:
// a cell `set` took but `for_each_in_box` cannot see is a permanent leak.
static void test_explode_stays_in_world() {
    CHECK(ewb::ExplodeWorld{}.yMax == ewb::WS_WORLD_HEIGHT,
          "the explode default clips at the height the chunk store sweeps");
    for (int cy : {ewb::WS_WORLD_HEIGHT - 1, 0}) {
        ewb::WorldStore store;
        int yLo = 1 << 30, yHi = -(1 << 30);
        size_t setCalls = 0;
        ewb::ExplodeWorld w;
        w.get = [&](int x, int y, int z, ewb::ExplodeCell& out) -> bool {
            ewb::WorldCell c;
            if (!store.get(x, y, z, c)) return false;
            out.type = c.type; out.color = c.color;
            return true;
        };
        w.set = [&](int x, int y, int z, int type, int color) -> bool {
            ++setCalls;
            yLo = std::min(yLo, y); yHi = std::max(yHi, y);
            return store.set(x, y, z, (unsigned char)type, (unsigned char)color);
        };
        store.set(65536, cy, 65536, (unsigned char)w.tntType, 0);
        const ewb::ExplodeResult r = ewb::simExplode(w, 65536, cy, 65536);
        CHECK(yLo >= 0 && yHi < ewb::WS_WORLD_HEIGHT, "a blast at the world's edge writes only inside it");
        CHECK(r.refused == 0 && store.out_of_range() == 0, "nothing was refused as out of range");
        size_t seen = 0;
        store.for_each_in_box(65536 - 16, 65536 + 16, 65536 - 16, 65536 + 16,
                              [&](int, int, int, int, int) { ++seen; });
        CHECK(seen == store.size(), "every cell the blast wrote is visible to a REGION sweep");
        CHECK(setCalls < 923, "the half-sphere outside the world is clipped, not written");
    }
}

// --- protected zones: the explosion hook (stage 8.2) -------------------------

// A FakeWorld-backed ExplodeWorld with a zone set wired to protectedAt, collecting
// every cell the hook refused — the shape the server's SvExplodeWorld has.
struct ZonedBlast {
    FakeWorld fw;
    ewb::ZoneSet zones;
    std::set<std::tuple<int, int, int>> refused;   // distinct protected cells reached
    ewb::ExplodeWorld w;
    ZonedBlast() {
        w.get = [this](int x, int y, int z, ewb::ExplodeCell& out) -> bool {
            auto it = fw.cells.find(FakeWorld::key(x, y, z));
            if (it == fw.cells.end()) return false;
            out = it->second;
            return true;
        };
        w.set = [this](int x, int y, int z, int type, int color) -> bool {
            fw.cells[FakeWorld::key(x, y, z)] = {(unsigned char)type, (unsigned char)color};
            return true;
        };
        w.protectedAt = [this](int x, int y, int z) -> bool {
            if (!zones.blocking(x, y, z)) return false;
            refused.insert({x, y, z});
            return true;
        };
    }
    void zone(const char* line) {
        ewb::Zone z;
        CHECK(ewb::zone_parse_line(line, z) && zones.add(z), "test zone parses");
    }
    const ewb::ExplodeCell* at(int x, int y, int z) const {
        auto it = fw.cells.find(FakeWorld::key(x, y, z));
        return it == fw.cells.end() ? nullptr : &it->second;
    }
};

static void test_explode_protected_cells_untouched() {
    // A TNT at x 1000, a zone covering x >= 1003: the blast's x 994..1002 goes, the
    // part of its sphere inside the zone is refused and reported, and none of it is
    // written. A block placed in the zone survives.
    ZonedBlast b;
    b.zone("wall:1003:0:900:1100:255:1100:all");
    b.fw.cells[FakeWorld::key(1000, 100, 1000)] = {9, 0};
    b.fw.cells[FakeWorld::key(1004, 100, 1000)] = {5, 0};   // a placed block, protected
    b.fw.cells[FakeWorld::key(998, 100, 1000)] = {5, 0};    // one outside the zone
    const ewb::ExplodeResult r = ewb::simExplode(b.w, 1000, 100, 1000);
    CHECK(r.explosions == 1 && r.protectedHits > 0, "the blast ran and was refused some cells");
    bool inZoneWritten = false, outsideWritten = false;
    for (const auto& kv : b.fw.cells) {
        const int x = int(kv.first >> 40), y = int(kv.first & 0xFFFF), z = int((kv.first >> 16) & 0xFFFFFF);
        if (x >= 1003 && !(x == 1004 && y == 100 && z == 1000)) inZoneWritten = true;
        if (x < 1003) outsideWritten = true;
    }
    CHECK(!inZoneWritten, "nothing inside the zone was written");
    CHECK(outsideWritten, "the unprotected part of the blast still applied");
    const ewb::ExplodeCell* kept = b.at(1004, 100, 1000);
    CHECK(kept && kept->type == 5, "a placed block inside the zone survives the blast");
    const ewb::ExplodeCell* gone = b.at(998, 100, 1000);
    CHECK(gone && gone->type == 0, "a placed block outside it does not");

    // The refused list is exactly the sphere's cells that fall in the zone.
    std::set<std::tuple<int, int, int>> expect;
    ewb::explode_for_each_blast_cell(1000, 100, 1000, 0, 256, [&](int x, int y, int z) {
        if (x >= 1003) expect.insert({x, y, z});
    });
    CHECK(!expect.empty() && b.refused == expect, "the refused cells are exactly the sphere's cells in the zone");
}

static void test_explode_protected_tnt_does_not_chain() {
    // Two TNT 4 apart. Unprotected, the first chains into the second.
    {
        ZonedBlast b;
        b.fw.cells[FakeWorld::key(2000, 100, 2000)] = {9, 0};
        b.fw.cells[FakeWorld::key(2004, 100, 2000)] = {9, 0};
        const ewb::ExplodeResult r = ewb::simExplode(b.w, 2000, 100, 2000);
        CHECK(r.explosions >= 2, "control: an unprotected TNT in the blast chains");
        const ewb::ExplodeCell* far = b.at(2010, 100, 2000);   // only the second blast reaches it
        CHECK(far && far->type == 0, "control: the chained blast reaches past the first");
    }
    // The second inside a zone: it is not set off, not written, and so nothing
    // beyond the first blast's own sphere is touched.
    {
        ZonedBlast b;
        b.zone("museum:2004:100:2000:2004:100:2000:all");
        b.fw.cells[FakeWorld::key(2000, 100, 2000)] = {9, 0};
        b.fw.cells[FakeWorld::key(2004, 100, 2000)] = {9, 0};
        const ewb::ExplodeResult r = ewb::simExplode(b.w, 2000, 100, 2000);
        CHECK(r.explosions == 1, "a protected TNT is not chained");
        const ewb::ExplodeCell* t = b.at(2004, 100, 2000);
        CHECK(t && t->type == 9, "the protected TNT is still there");
        CHECK(b.at(2010, 100, 2000) == nullptr, "nothing past the first sphere was touched");
        CHECK(b.refused.size() == 1 && b.refused.count({2004, 100, 2000}), "the TNT is the one refused cell");
    }
    // A protected root runs no blast at all.
    {
        ZonedBlast b;
        b.zone("root:3000:100:3000:3000:100:3000:all");
        b.fw.cells[FakeWorld::key(3000, 100, 3000)] = {9, 0};
        const ewb::ExplodeResult r = ewb::simExplode(b.w, 3000, 100, 3000);
        CHECK(r.explosions == 0 && r.visits == 0 && b.fw.cells.size() == 1, "a protected root is not set off");
    }
    // An "off" zone protects nothing.
    {
        ZonedBlast b;
        b.zone("idle:2004:100:2000:2004:100:2000:off");
        b.fw.cells[FakeWorld::key(2000, 100, 2000)] = {9, 0};
        b.fw.cells[FakeWorld::key(2004, 100, 2000)] = {9, 0};
        CHECK(ewb::simExplode(b.w, 2000, 100, 2000).explosions >= 2, "an 'off' zone does not stop the chain");
    }
}

// EXPLODE_REACH is what lets the server hand a chain only the zones near its root,
// so it must bound every cell the chain can touch — and be tight, or it filters
// in zones for nothing. A line of TNT 6 apart chains as deep as the guard allows.
static void test_explode_reach() {
    ZonedBlast b;
    // TNT at depths 0..6; the last link's blast is the one that reaches furthest.
    for (int k = 0; k <= ewb::EXPLODE_MAX_DEPTH; ++k) b.fw.cells[FakeWorld::key(5000 + 6 * k, 100, 5000)] = {9, 0};
    int maxOff = 0;
    auto set = b.w.set;
    b.w.set = [&](int x, int y, int z, int type, int color) -> bool {
        maxOff = std::max({maxOff, std::abs(x - 5000), std::abs(y - 100), std::abs(z - 5000)});
        return set(x, y, z, type, color);
    };
    const ewb::ExplodeResult r = ewb::simExplode(b.w, 5000, 100, 5000);
    // (More blasts than links: the y = cy layer is sampled twice, so each link is
    // queued twice — the client's order, kept since 7.7.)
    CHECK(r.truncated == 0 && r.explosions > size_t(ewb::EXPLODE_MAX_DEPTH), "the chain runs to the depth guard");
    CHECK(maxOff == ewb::EXPLODE_REACH, "the chain reaches exactly EXPLODE_REACH from its root, no further");
}

static void test_explode_blast_footprint() {
    // explode_for_each_blast_cell is the footprint simExplode actually sets: an
    // isolated blast's distinct cells, the 923 of the 7.7 golden.
    std::set<std::tuple<int, int, int>> fp;
    ewb::explode_for_each_blast_cell(100, 100, 100, 0, 256, [&](int x, int y, int z) { fp.insert({x, y, z}); });
    ZonedBlast b;
    b.fw.cells[FakeWorld::key(100, 100, 100)] = {9, 0};
    ewb::simExplode(b.w, 100, 100, 100);
    std::set<std::tuple<int, int, int>> set;
    for (const auto& kv : b.fw.cells)
        set.insert({int(kv.first >> 40), int(kv.first & 0xFFFF), int((kv.first >> 16) & 0xFFFFFF)});
    CHECK(fp.size() == 923 && fp == set, "the blast footprint matches what a blast writes");
    size_t clipped = 0;
    ewb::explode_for_each_blast_cell(100, 0, 100, 0, 256, [&](int, int y, int) { clipped += (y < 0); });
    CHECK(clipped == 0, "the footprint is clipped to the world");
}

// --- protected zones: the restore (stage 8.2) --------------------------------

static void test_zone_restore_wire() {
    auto wire = [](bool present, int type, int color, int y = 40) {
        std::string w;
        ewb::zone_restore_wire(w, 7, y, 9, present, type, color);
        return w;
    };
    CHECK(wire(true, 0, 0) == "ACTION:server:0:7:40:9:1\n", "stored air: a mine");
    CHECK(wire(true, 5, 0) == "ACTION:server:0:7:40:9:1\nACTION:server:0:7:40:9:0:5\n",
          "stored block: mine then build");
    CHECK(wire(true, 5, 12) ==
              "ACTION:server:0:7:40:9:1\nACTION:server:0:7:40:9:0:5\nACTION:server:0:7:40:9:3:12\n",
          "stored painted block: mine, build, paint");
    // Painted natural grass: the relay would be a lone paint, but the refused edit may
    // have removed the block from the screen, so the restore rebuilds it.
    CHECK(wire(true, 255, 12, 32) ==
              "ACTION:server:0:7:32:9:1\nACTION:server:0:7:32:9:0:8\nACTION:server:0:7:32:9:3:12\n",
          "painted natural block: mine, build the natural block, paint");
    CHECK(wire(false, 0, 0, 32) == "ACTION:server:0:7:32:9:1\nACTION:server:0:7:32:9:0:8\n",
          "untouched grass height: the natural grass comes back");
    CHECK(wire(false, 0, 0, 20) == "ACTION:server:0:7:20:9:1\nACTION:server:0:7:20:9:0:3\n",
          "untouched dirt height: dirt");
    CHECK(wire(false, 0, 0, 0) == "ACTION:server:0:7:0:9:1\nACTION:server:0:7:0:9:0:1\n",
          "untouched bedrock height: bedrock");
    CHECK(wire(false, 0, 0, 33) == "ACTION:server:0:7:33:9:1\n", "untouched sky: just a mine");
    CHECK(wire(true, 5, 99) == "ACTION:server:0:7:40:9:1\nACTION:server:0:7:40:9:0:5\n",
          "an out-of-palette colour is not sent");
    // A custom profile is honoured when given.
    ewb::BaseProfile p;
    p.layers.resize(41);
    p.layers[40] = {42, 7};
    std::string w;
    ewb::zone_restore_wire(w, 1, 40, 2, false, 0, 0, p);
    CHECK(w == "ACTION:server:0:1:40:2:1\nACTION:server:0:1:40:2:0:42\nACTION:server:0:1:40:2:3:7\n",
          "an untouched cell restores from the profile it is given");
}

static void test_zone_cell_set() {
    ewb::ZoneCellSet s(3);
    CHECK(s.add(1, 2, 3) && !s.add(1, 2, 3), "a cell is added once");
    CHECK(s.add(4, 5, 6) && s.add(7, 8, 9), "distinct cells are added");
    CHECK(!s.add(10, 11, 12) && s.overflow() == 1 && s.size() == 3, "past the cap: refused and counted");
    CHECK(!s.add(1, 2, 3) && s.overflow() == 1, "a repeat at the cap is a repeat, not an overflow");
    const ewb::RevertCell first{1, 2, 3}, third{7, 8, 9};
    CHECK(s.cells()[0] == first && s.cells()[2] == third, "first-seen order is kept");
}

static void test_revert_queue() {
    ewb::RevertQueue<int> q;
    const std::vector<ewb::RevertCell> one{{65536, 33, 65536}};
    // A hundred mines on one protected block inside the delay: one restore.
    size_t queued = 0;
    for (int i = 0; i < 100; ++i) queued += q.push(10.0 + i * 0.001, 1, 0, one);
    CHECK(queued == 1 && q.jobs() == 1 && q.pending() == 1, "100 denials of one cell coalesce to one restore");
    // Another client denied the same cell gets their own.
    CHECK(q.push(10.5, 2, 0, one) == 1, "coalescing is per client, not global");
    // A burn restore mixed with a cell already queued: only the new cells are queued.
    CHECK(q.push(11.0, 1, 0, {{65536, 33, 65536}, {1, 2, 3}}) == 1, "a batch skips cells already pending");

    ewb::RevertQueue<int>::Job j;
    CHECK(!q.pop_due(9.99, j), "nothing is sent before it is due");
    CHECK(q.pop_due(10.0, j) && j.target == 1 && j.cells.size() == 1, "the earliest job comes first");
    CHECK(!q.pop_due(10.2, j), "the next one is not due yet");
    CHECK(q.pop_due(10.6, j) && j.target == 2, "then the second client's");
    // Once sent, the cell can be restored again — the next denial is a new attempt.
    CHECK(q.push(12.0, 1, 0, one) == 1, "a cell is queueable again once its restore went out");
    CHECK(q.pop_due(100.0, j) && j.due == 11.0 && q.pop_due(100.0, j) && j.due == 12.0 && q.empty(),
          "the rest drain in due order");

    // Equal due times leave in push order.
    q.push(20.0, 7, 70, {{1, 1, 1}});
    q.push(20.0, 8, 80, {{1, 1, 1}});
    CHECK(q.pop_due(20.0, j) && j.payload == 70 && q.pop_due(20.0, j) && j.payload == 80, "ties are FIFO");

    // The pending cap drops, and counts.
    ewb::RevertQueue<int> small(2);
    size_t dropped = 0;
    CHECK(small.push(1.0, 1, 0, {{1, 1, 1}, {2, 2, 2}, {3, 3, 3}}, &dropped) == 2 && dropped == 1,
          "past the pending cap a restore is dropped and counted");
    CHECK(small.push(1.0, 1, 0, {{4, 4, 4}}, &dropped) == 0 && dropped == 2 && small.jobs() == 1,
          "a push that admits nothing queues no job");
}

static void test_zone_audit_agg() {
    ewb::ZoneAuditAgg a(10.0);
    std::string line;
    CHECK(a.note(0.0, "bob", "spawn", "mine", 1, 2, 3, line) &&
              line == "denied mine in zone spawn at 1,2,3",
          "the first denial is written at once");
    int written = 0;
    for (int i = 1; i <= 50; ++i) written += a.note(i * 0.1, "bob", "spawn", "mine", 1, 2, 3, line);
    CHECK(written == 0, "denials inside the window are folded, not written");
    CHECK(a.note(0.5, "bob", "museum", "build", 4, 5, 6, line), "another zone is its own window");
    CHECK(a.note(0.5, "eve", "spawn", "mine", 1, 2, 3, line), "another player is their own window");
    CHECK(a.flush(9.0).empty(), "nothing is flushed while the window is open");
    auto out = a.flush(10.0);
    CHECK(out.size() == 1 && out[0].first == "bob" &&
              out[0].second == "denied 50 more edit(s) in zone spawn (last: mine at 1,2,3)",
          "the folded count is written once the window closes");
    // The flushed line opened a window of its own: a denial right after is folded.
    CHECK(!a.note(10.5, "bob", "spawn", "paint", 1, 2, 3, line), "never two lines in one window");
    out = a.flush(20.0);
    CHECK(out.size() == 1 && out[0].second == "denied 1 more edit(s) in zone spawn (last: paint at 1,2,3)",
          "and it is flushed in the next");
    // Quiet pairs are forgotten, so the table does not grow with names seen once.
    a.flush(40.0);
    CHECK(a.size() == 0, "closed windows with nothing folded are dropped");
    // A folded count still pending when the next denial arrives is carried onto it.
    ewb::ZoneAuditAgg b(10.0);
    b.note(0.0, "p", "z", "mine", 0, 0, 0, line);
    b.note(1.0, "p", "z", "mine", 0, 0, 0, line);
    CHECK(b.note(11.0, "p", "z", "build", 9, 9, 9, line) &&
              line == "denied build in zone z at 9,9,9 (1 more before this)",
          "a count not yet flushed rides on the next line");
}

// --- usernames (stage 1.7) ---------------------------------------------------

// --- world spawn sidecar (stage 5.3) ---------------------------------------

// --- eden_motd.txt (the per-world welcome message) --------------------------

static void test_motd_parse() {
    {
        std::istringstream in("# the rules\n"
                              "Welcome! This server is moderated.\n"
                              "\n"
                              "   Griefing results in a ban.   \n");
        const auto lines = ewb::parse_motd(in);
        CHECK(lines.size() == 2, "comments and blank lines are skipped");
        CHECK(lines[0] == "Welcome! This server is moderated.", "first line kept verbatim");
        CHECK(lines[1] == "Griefing results in a ban.", "surrounding whitespace trimmed");
    }
    {
        std::istringstream in("");
        CHECK(ewb::parse_motd(in).empty(), "an empty file is an empty MOTD");
    }
    {
        std::istringstream in("#only a comment\n   \n");
        CHECK(ewb::parse_motd(in).empty(), "comments-and-blanks only is an empty MOTD");
    }
    {
        // A hand-edited file is operator input but becomes wire output: a line
        // carrying a newline must not be able to smuggle a second wire line in.
        std::istringstream in("hi\x01there\nSIGNP:1:2:3");
        const auto lines = ewb::parse_motd(in);
        CHECK(lines.size() == 2, "two lines");
        CHECK(lines[0] == "hithere", "control characters stripped");
        const std::string wire = ewb::format_motd_line(lines[1]);
        CHECK(wire == "[Server] SIGNP:1:2:3\n",
              "a line that looks like a verb is still a [Server] chat line");
        CHECK(wire.find('\n') == wire.size() - 1, "exactly one newline, at the end");
    }
    {
        std::istringstream in(std::string(ewb::MOTD_MAX_LINE + 50, 'x') + "\n");
        const auto lines = ewb::parse_motd(in);
        CHECK(lines.size() == 1 && lines[0].size() == ewb::MOTD_MAX_LINE,
              "an over-long line is truncated to the chat cap");
    }
    {
        std::string many;
        for (size_t i = 0; i < ewb::MOTD_MAX_LINES + 3; ++i) many += "line\n";
        std::istringstream in(many);
        size_t dropped = 0;
        const auto lines = ewb::parse_motd(in, ewb::MOTD_MAX_LINES, ewb::MOTD_MAX_LINE, &dropped);
        CHECK(lines.size() == ewb::MOTD_MAX_LINES, "line count capped");
        CHECK(dropped == 3, "the dropped lines are counted, not silently lost");
    }
    {
        std::istringstream in("one\ntwo\n");
        const auto burst = ewb::format_motd_burst(ewb::parse_motd(in));
        CHECK(burst.size() == 2, "one wire line per MOTD line");
        CHECK(burst[0] == "[Server] one\n" && burst[1] == "[Server] two\n",
              "wire lines are [Server] chat lines in file order");
    }
}

static void test_spawn_parse() {
    ewb::Spawn s;
    bool skip = true;

    CHECK(ewb::parse_spawn_line("65536.00:33.92:65540.50", s, skip), "a good line parses");
    CHECK(!skip, "a good line is not a skip");
    CHECK(s.x == 65536.0f && s.z == 65540.5f, "coords in server order");

    // Round-trips through the formatter the server and eden_import share.
    ewb::Spawn r;
    CHECK(ewb::parse_spawn_line(ewb::format_spawn_line(s), r) &&
              r.x == s.x && r.y == s.y && r.z == s.z,
          "format -> parse round-trips");

    CHECK(ewb::parse_spawn_line("  1:2:3 \r\n", s, skip) && s.x == 1 && s.y == 2 && s.z == 3,
          "surrounding whitespace and CRLF tolerated");
    CHECK(ewb::parse_spawn_line("-3.5:0:10", s) && s.x == -3.5f, "negative coordinate is fine");

    for (const char* l : {"", "1:2", "1:2:3:4", "a:b:c", "1:2:three", "1 2 3"}) {
        CHECK(!ewb::parse_spawn_line(l, s, skip), "malformed spawn line rejected");
        if (l[0] != '\0')
            CHECK(!skip, "a non-empty malformed line is an error, not a skip");
    }

    ewb::Spawn unused;
    CHECK(!ewb::parse_spawn_line("# a comment", unused, skip) && skip, "'#' comment is a skip");
    CHECK(!ewb::parse_spawn_line("   ", unused, skip) && skip, "blank line is a skip");
}

static void test_username_validation() {
    using ewb::NameVerdict;
    using ewb::validate_username;

    CHECK(validate_username("Player6835") == NameVerdict::Ok, "a captured username is legal");
    CHECK(validate_username("sam_2") == NameVerdict::Ok, "underscores and digits");
    CHECK(validate_username("two words") == NameVerdict::Ok, "an interior space is fine");

    CHECK(validate_username("") == NameVerdict::Empty, "empty");
    CHECK(validate_username(std::string(ewb::USERNAME_MAX + 1, 'a')) == NameVerdict::TooLong, "too long");
    CHECK(validate_username(std::string(ewb::USERNAME_MAX, 'a')) == NameVerdict::Ok, "exactly at the cap");

    // The forgery the rule exists for: chat is `[<user> (T<n>)] <text>`.
    CHECK(validate_username("bob] hi [server (T0)") == NameVerdict::BadChar,
          "a ']' cannot forge chat structure");
    CHECK(validate_username("a[b") == NameVerdict::BadChar, "'[' rejected too");
    CHECK(validate_username("a:b") == NameVerdict::BadChar, "the protocol delimiter is rejected");
    CHECK(validate_username("a\nb") == NameVerdict::BadChar, "a newline cannot be smuggled in");
    CHECK(validate_username(" bob") == NameVerdict::BadChar, "no leading space");
    CHECK(validate_username("bob ") == NameVerdict::BadChar, "no trailing space");
    CHECK(validate_username("caf\xC3\xA9") == NameVerdict::BadChar, "bounded to printable ASCII");

    // The reserved sender token used by SIGNP and ACTION:server:0:.
    CHECK(validate_username("server") == NameVerdict::Reserved, "reserved name");
    CHECK(validate_username("Server") == NameVerdict::Reserved, "reserved name, any case");
    CHECK(validate_username("SERVER") == NameVerdict::Reserved, "reserved name, upper");
    CHECK(validate_username("server2") == NameVerdict::Ok, "only the exact token is reserved");
}

static void test_duplicate_names() {
    using ewb::DupNameAction;
    using ewb::dup_name_action;
    using ewb::next_free_username;
    using Names = std::set<std::string>;

    CHECK(next_free_username("td0", Names{}) == "td0", "a free name is returned untouched");
    CHECK(next_free_username("td0", Names{"other"}) == "td0", "unrelated names do not matter");
    CHECK(next_free_username("td0", Names{"td0"}) == "td0-2", "first collision -> -2");
    CHECK(next_free_username("td0", Names{"td0", "td0-2"}) == "td0-3", "lowest free suffix");
    CHECK(next_free_username("td0", Names{"td0", "td0-3"}) == "td0-2", "a gap is filled, not skipped");
    CHECK(next_free_username("td0", Names{"TD0"}) == "td0", "comparison is exact, like the g_playerPos key");

    // The suffix must still fit, and the result must still be a legal username.
    const std::string full(ewb::USERNAME_MAX, 'a');
    const std::string cut = next_free_username(full, Names{full});
    CHECK(cut.size() == ewb::USERNAME_MAX, "a name at the cap is cut to make room for the suffix");
    CHECK(cut == std::string(ewb::USERNAME_MAX - 2, 'a') + "-2", "the base loses exactly the suffix width");
    CHECK(ewb::validate_username(cut) == ewb::NameVerdict::Ok, "and the cut name validates");

    // Cutting can expose a space, which validate_username refuses.
    const std::string spaced = std::string(ewb::USERNAME_MAX - 3, 'a') + " b";   // the cut lands on the space
    const std::string sp = next_free_username(spaced, Names{spaced});
    CHECK(ewb::validate_username(sp) == ewb::NameVerdict::Ok, "a trailing space left by the cut is dropped");

    Names many{"n"};
    for (int i = 2; i <= 12; ++i) many.insert("n-" + std::to_string(i));
    CHECK(next_free_username("n", many) == "n-13", "double-digit suffixes");
    const std::string wide = next_free_username(full, Names{full, std::string(ewb::USERNAME_MAX - 2, 'a') + "-2"});
    CHECK(wide.size() <= ewb::USERNAME_MAX && wide != full, "still bounded when the first suffix is taken");

    // Eviction needs the same address AND a quiet old socket.
    CHECK(dup_name_action(true, 20.0, 15.0) == DupNameAction::Evict, "same address, silent past the threshold");
    CHECK(dup_name_action(true, 15.0, 15.0) == DupNameAction::Evict, "the threshold itself counts");
    CHECK(dup_name_action(true, 3.0, 15.0) == DupNameAction::Suffix,
          "same address but the old socket is still talking: a live player, not a stale one");
    CHECK(dup_name_action(false, 600.0, 15.0) == DupNameAction::Suffix,
          "a different address never evicts, however quiet");
    CHECK(dup_name_action(true, 600.0, 0.0) == DupNameAction::Suffix, "0 turns eviction off");
    CHECK(dup_name_action(true, 600.0, -1.0) == DupNameAction::Suffix, "negative turns it off too");
}

static void test_format_move() {
    using ewb::format_move_float;
    using ewb::format_move_triplet;

    CHECK(format_move_float(33.92f) == "33.92", "the standing height, two decimals");
    CHECK(format_move_float(65500.0f) == "65500.00", "an integer gains its decimals");
    CHECK(format_move_float(-0.5f) == "-0.50", "sign kept");
    CHECK(format_move_float(-0.001f) == "0.00", "a tiny negative is not '-0.00'");
    CHECK(format_move_float(0.0f) == "0.00", "zero");
    CHECK(format_move_float(16777215.0f) == "16777215.00", "the top of the x/z range fits");
    CHECK(format_move_triplet(1.0f, 2.5f, -3.0f) == "1.00:2.50:-3.00", "triplet joins with ':'");

    // The 7.18 amplifier: an in-range field the sender padded to any length.
    float v = -1.0f;
    const std::string padded = std::string(1300, '0') + "1";
    CHECK(ewb::parse_move_float(padded, v) && v == 1.0f, "the padded field still parses (in range)");
    CHECK(format_move_float(v) == "1.00", "but what goes back out is the number, not the padding");

    // Whatever the parser admits formats to a bounded, re-parseable token.
    const float samples[] = {0.0f, 33.92f, 65536.0f, 16777215.0f, 272.0f, -4096.0f, 4096.0f, 1e-7f};
    for (float x : samples) {
        const std::string t = format_move_float(x);
        float back = 0;
        CHECK(t.size() <= 12, "formatted field is short");
        CHECK(ewb::parse_move_float(t, back), "formatted field is a valid movement token");
    }
}

// --- ACTION payload (stage 1.7) ----------------------------------------------

static void test_action_extra_validation() {
    using ewb::action_extra_valid;

    CHECK(action_extra_valid(0, 13), "build a real block type");
    CHECK(action_extra_valid(0, 0), "build type 0 (air) is legal — air is a type, not an erase");
    CHECK(action_extra_valid(0, ewb::MAX_BLOCK_TYPE), "the top of the block table");
    CHECK(!action_extra_valid(0, ewb::MAX_BLOCK_TYPE + 1), "past the block table");
    CHECK(!action_extra_valid(0, 255), "255 is the painted-base sentinel, never a placeable type");
    CHECK(!action_extra_valid(0, -1), "negative type");

    CHECK(action_extra_valid(3, 0), "paint 0 = unpaint");
    CHECK(action_extra_valid(3, 54), "the top of the palette");
    CHECK(!action_extra_valid(3, 55), "past the palette");
    CHECK(!action_extra_valid(3, 255), "the one that would put 255 in Cell::color");

    // Mine and burn carry no payload, so a trailing field is ignored rather than
    // fatal — a client that sends one is odd, not hostile.
    CHECK(action_extra_valid(1, 9999), "mine ignores extra");
    CHECK(action_extra_valid(2, -5), "burn ignores extra");
}

// --- pre-JOIN admission gate (stage 7.17) ------------------------------------
//
// Five verbs used to skip the handshake check the other four had, so on a
// `--password` server an anonymous peer could edit the world, broadcast chat and
// inject a phantom player. The rule is an allow-list, so the interesting half of
// this test is everything it *refuses* — including a verb nobody has written yet.

static void test_verb_allowed_before_join() {
    using ewb::verb_allowed_before_join;

    CHECK(verb_allowed_before_join("JOIN"), "JOIN is how a connection stops being anonymous");
    CHECK(verb_allowed_before_join("PING"), "PING answers a bare PONG and leaks nothing");

    // The five the finding named.
    CHECK(!verb_allowed_before_join("ACTION"), "ACTION pre-JOIN edits and persists the world");
    CHECK(!verb_allowed_before_join("MSG"), "MSG pre-JOIN broadcasts chat as Player<n>");
    CHECK(!verb_allowed_before_join("POS"), "POS pre-JOIN injects a phantom player");
    CHECK(!verb_allowed_before_join("VEL"), "VEL pre-JOIN is relayed too");
    CHECK(!verb_allowed_before_join("POSVEL"), "POSVEL pre-JOIN is relayed too");

    // The four that already checked, now relying on this gate instead.
    CHECK(!verb_allowed_before_join("REGION"), "REGION pre-JOIN is the amplification vector");
    CHECK(!verb_allowed_before_join("SIGNQ"), "SIGNQ pre-JOIN dumps the sign file");
    CHECK(!verb_allowed_before_join("SIGNP"), "SIGNP pre-JOIN writes world content");

    // Default-deny is the point: an unknown verb, the empty line, and a
    // case-shifted or whitespace-padded spelling of an allowed one all fail. The
    // dispatch below the gate compares verbs exactly, so the gate must too — a
    // looser match here would be a way past it.
    CHECK(!verb_allowed_before_join("FUTUREVERB"), "a verb added later is gated by default");
    CHECK(!verb_allowed_before_join(""), "the empty verb");
    CHECK(!verb_allowed_before_join("join"), "lowercase is not the wire spelling");
    CHECK(!verb_allowed_before_join("JOIN "), "trailing space is not JOIN");
    CHECK(!verb_allowed_before_join("PING\r"), "a CRLF-framed PING is not PING");
    CHECK(!verb_allowed_before_join("JOINX"), "a prefix match is not a match");
}

// --- movement fields (stage 7.16) --------------------------------------------
//
// What `std::stof` let through before this existed: `1e38` reached `SPAWN`'s
// 96-byte formatter, where snprintf's "would have written" return (132) made the
// server read 36 bytes past the buffer and send that stack to the client.

static void test_parse_move_float() {
    using ewb::parse_move_float;
    float v = -1.0f;

    CHECK(parse_move_float("33.92", v) && v == 33.92f, "the ground standing height");
    CHECK(parse_move_float("65536", v) && v == 65536.0f, "an integer field");
    CHECK(parse_move_float("-0.5", v) && v == -0.5f, "a negative (a velocity field)");
    CHECK(parse_move_float("65536.00", v) && v == 65536.0f, "the precision the server itself writes");
    CHECK(parse_move_float("1e2", v) && v == 100.0f, "exponent notation");
    CHECK(parse_move_float("0", v) && v == 0.0f, "zero");

    // The whole token, or nothing: stof stopped at the first unusable byte and
    // returned without throwing, so all of these used to "parse".
    CHECK(!parse_move_float("1\r", v), "a bare CR is not a float (it was relayed verbatim)");
    CHECK(!parse_move_float("1x", v), "trailing junk");
    CHECK(!parse_move_float("1 ", v), "trailing space");
    CHECK(!parse_move_float(" 1", v), "leading space (strtof would skip it)");
    CHECK(!parse_move_float("\t1", v), "leading tab");
    CHECK(!parse_move_float("", v), "empty field");
    CHECK(!parse_move_float("abc", v), "not a number at all");
    CHECK(!parse_move_float(std::string("1\0" "2", 3), v), "an embedded NUL cannot hide a tail");

    // Non-finite, however spelled.
    CHECK(!parse_move_float("nan", v), "nan");
    CHECK(!parse_move_float("NaN", v), "nan, any case");
    CHECK(!parse_move_float("inf", v), "inf");
    CHECK(!parse_move_float("-inf", v), "-inf");
    CHECK(!parse_move_float("infinity", v), "infinity");
    CHECK(!parse_move_float("1e40", v), "past float range -> HUGE_VALF, not a number");

    // 1e38 *is* a finite float — it is the range check, not the finiteness
    // check, that stops the one that reached the SPAWN buffer.
    CHECK(parse_move_float("1e38", v) && v == 1e38f, "1e38 parses; the bound is what refuses it");
    CHECK(!ewb::move_pos_valid(1e38f, 1e38f, 1e38f), "...and the bound refuses it");
}

static void test_move_bounds() {
    using ewb::move_pos_valid;
    using ewb::move_vel_valid;

    // x/z are centred on 65536; a real position is nowhere near either edge.
    CHECK(move_pos_valid(65536.0f, 33.92f, 65536.0f), "a player standing on the ground");
    CHECK(move_pos_valid(0.0f, 0.0f, 0.0f), "the corner of the box is inclusive");
    CHECK(move_pos_valid(ewb::MOVE_XZ_MAX, ewb::MOVE_Y_MAX, ewb::MOVE_XZ_MAX), "the far corner too");
    CHECK(move_pos_valid(65536.0f, 256.92f, 65536.0f), "standing on the topmost block clears 255");

    CHECK(!move_pos_valid(-0.5f, 33.92f, 65536.0f), "x below the box");
    CHECK(!move_pos_valid(65536.0f, 33.92f, -1.0f), "z below the box");
    CHECK(!move_pos_valid(ewb::MOVE_XZ_MAX + 1024.0f, 33.92f, 65536.0f), "x past the 24-bit range");
    CHECK(!move_pos_valid(65536.0f, -1.0f, 65536.0f), "y below the world (fallen out of it)");
    CHECK(!move_pos_valid(65536.0f, ewb::MOVE_Y_MAX + 1.0f, 65536.0f), "y above the headroom");

    const float nan_ = std::nanf("");
    CHECK(!move_pos_valid(nan_, 33.92f, 65536.0f), "nan fails every comparison, so it is refused");
    CHECK(!move_vel_valid(nan_, 0.0f, 0.0f), "nan velocity too");

    // Velocity is signed and symmetric — the bound is a ceiling, not a box.
    CHECK(move_vel_valid(0.0f, -9.8f, 0.0f), "falling");
    CHECK(move_vel_valid(-ewb::MOVE_VEL_MAX, ewb::MOVE_VEL_MAX, 0.0f), "the ceiling is inclusive");
    CHECK(!move_vel_valid(0.0f, -(ewb::MOVE_VEL_MAX + 1.0f), 0.0f), "past the ceiling");
    CHECK(!move_vel_valid(1e38f, 0.0f, 0.0f), "the SPAWN-buffer value as a velocity");
}

static void test_parse_move_triples() {
    float x, y, z;

    CHECK(ewb::parse_move_pos("65536.00", "33.92", "65540.50", x, y, z) &&
              x == 65536.0f && y == 33.92f && z == 65540.5f,
          "a POS the retail client would send");
    // The reported repro: POS:1e38:1e38:1e38, stored and handed back as SPAWN.
    CHECK(!ewb::parse_move_pos("1e38", "1e38", "1e38", x, y, z), "the 7.16 repro is refused");
    CHECK(!ewb::parse_move_pos("65536", "nan", "65536", x, y, z), "one bad field fails the triple");
    CHECK(!ewb::parse_move_pos("65536", "33.92", "65536\r", x, y, z), "a CRLF-framed line is refused");

    // The 7.18 amplifier's shape: long, but it does parse — the length rule is a
    // separate stage; what matters here is that the *value* is in range.
    CHECK(ewb::parse_move_pos(std::string(1300, '0') + "1", "33.92", "65536", x, y, z) && x == 1.0f,
          "leading zeros still parse to an in-range value");

    CHECK(ewb::parse_move_vel("0.00", "-9.81", "0.00", x, y, z) && y == -9.81f, "a VEL triple");
    CHECK(!ewb::parse_move_vel("0", "-inf", "0", x, y, z), "a non-finite velocity field");
}

// The defect itself: a string built from snprintf's return over-reads the buffer.
// `format_spawn_line` is the shared formatter; the server's own `spawnLine` is in
// the single TU, so this stands in for both — same 96 bytes, same %.2f grammar.
static void test_spawn_format_cannot_over_read() {
    const std::string ok = ewb::format_spawn_line({65536.0f, 33.92f, 65540.5f});
    CHECK(ok == "65536.00:33.92:65540.50\n", "an in-range spawn formats exactly");

    // 1e38 at %.2f needs 132 bytes. The result must be clamped to the buffer, not
    // 132 bytes read from a 96-byte array.
    const std::string huge = ewb::format_spawn_line({1e38f, 1e38f, 1e38f});
    CHECK(huge.size() < 96, "an out-of-range spawn is truncated, not over-read");
    CHECK(huge.compare(0, 8, "99999996") == 0, "...and what it does hold is the formatted value");
}

// --- token bucket (stage 1.7) ------------------------------------------------

static void test_token_bucket() {
    ewb::TokenBucket b(4.0, 2.0);   // burst 4, 2/sec

    double t = 100.0;
    for (int i = 0; i < 4; ++i) CHECK(b.allow(t), "the burst is spendable at once");
    CHECK(!b.allow(t), "the fifth is refused");

    // A refused call spends nothing, so a spammer cannot keep the bucket empty
    // by asking harder.
    for (int i = 0; i < 100; ++i) (void)b.allow(t);
    CHECK(b.allow(t + 0.5), "half a second refills exactly one token");
    CHECK(!b.allow(t + 0.5), "...and only one");

    // Refill is capped at the burst size, not accumulated forever.
    CHECK(b.allow(t + 1000.0), "a long idle refills");
    for (int i = 0; i < 3; ++i) CHECK(b.allow(t + 1000.0), "up to the burst");
    CHECK(!b.allow(t + 1000.0), "and no further — idle time does not bank");

    // A costly request (BURN) is all-or-nothing.
    ewb::TokenBucket burn(8.0, 1.0);
    CHECK(!burn.allow(0.0, 9.0), "a cost above the whole burst can never be paid");
    CHECK(burn.allow(0.0, 8.0), "a cost exactly the burst can");
    CHECK(!burn.allow(0.0, 1.0), "...and empties it");

    // Post-hoc charge (stage 7.19): may go into debt, bounded at one burst.
    ewb::TokenBucket chain(8.0, 2.0);
    CHECK(chain.allow(0.0, 4.0), "upfront cost paid");
    chain.charge(0.0, 1000.0);
    CHECK(chain.tokens == -8.0, "debt floored at minus one burst");
    CHECK(!chain.allow(4.0, 1.0), "4 s repays the debt to 0 — still nothing to spend");
    CHECK(chain.allow(4.5, 1.0), "the next token arrives after the debt is repaid");
}

// --- connect limiter (stage 1.7) ---------------------------------------------

static void test_connect_limiter() {
    ewb::ConnectLimiter lim(3, 10.0);

    for (int i = 0; i < 3; ++i) CHECK(lim.allow("10.0.0.1", 0.0), "the window's allowance");
    CHECK(!lim.allow("10.0.0.1", 0.0), "the fourth in the window is refused");

    // Other addresses are unaffected — one flooding host must not lock everyone out.
    CHECK(lim.allow("10.0.0.2", 0.0), "a different IP is independent");

    // The window slides.
    CHECK(!lim.allow("10.0.0.1", 9.9), "still inside the window");
    CHECK(lim.allow("10.0.0.1", 10.1), "past the window, allowed again");

    // Stale entries are reclaimed rather than accumulating forever.
    for (int i = 0; i < 50; ++i) CHECK(lim.allow("172.16.0." + std::to_string(i), 20.0), "50 fresh IPs");
    CHECK(lim.tracked() >= 50, "all tracked while in-window");
    lim.sweep(100.0);
    CHECK(lim.tracked() == 0, "sweep drops everything outside the window");

    // ⚠️ Fails open once its own table is full — the guard must not become the
    // memory exhaustion it exists to prevent.
    ewb::ConnectLimiter small(1, 10.0, /*max_tracked=*/4);
    for (int i = 0; i < 4; ++i) CHECK(small.allow("192.168.1." + std::to_string(i), 0.0), "fills the table");
    CHECK(small.allow("192.168.9.9", 0.0), "an untrackable new IP is allowed, not dropped");
    CHECK(small.tracked() <= 4, "and is not tracked");
}

// --- constant-time compare + auth throttle (stage 1.10) --------------------

// Stage 7.28: the password can come from a file or the environment, not only argv.
static void test_password_sources() {
    using ewb::PasswordSource;
    CHECK(ewb::password_from_file_text("secret\n") == "secret", "trailing LF dropped");
    CHECK(ewb::password_from_file_text("secret\r\n") == "secret", "trailing CRLF dropped");
    CHECK(ewb::password_from_file_text("secret") == "secret", "no newline at all is fine");
    CHECK(ewb::password_from_file_text("two words \nignored\n") == "two words ", "first line only, spaces kept");
    CHECK(ewb::password_from_file_text("\n").empty(), "a blank file has no password");

    const std::string f = "from-file";
    ewb::PasswordChoice c = ewb::choose_password("argv", &f, "env");
    CHECK(c.source == PasswordSource::Argv && c.value == "argv", "explicit --password wins");
    c = ewb::choose_password("", &f, "env");
    CHECK(c.source == PasswordSource::File && c.value == "from-file", "--password-file beats the environment");
    c = ewb::choose_password("", nullptr, "env");
    CHECK(c.source == PasswordSource::Env && c.value == "env", "environment is the last resort");
    c = ewb::choose_password("", nullptr, "");
    CHECK(c.source == PasswordSource::None && c.value.empty(), "an empty EDEN_PASSWORD is an open server");
    c = ewb::choose_password("", nullptr, nullptr);
    CHECK(c.source == PasswordSource::None, "nothing set is an open server");
    // The pre-7.28 unit files pass `--password ${EDEN_PASSWORD}`; an empty one must not shadow the env.
    c = ewb::choose_password("", nullptr, "env");
    CHECK(c.source == PasswordSource::Env, "--password \"\" does not mask EDEN_PASSWORD");
    const std::string empty;
    c = ewb::choose_password("", &empty, "env");
    CHECK(c.source == PasswordSource::File && c.value.empty(),
          "a requested-but-empty file is reported as File so main() can refuse it");
}

static void test_const_time_eq() {
    CHECK(ewb::const_time_eq("hunter2", "hunter2"), "equal strings compare equal");
    CHECK(!ewb::const_time_eq("hunter2", "hunter3"), "one byte different");
    CHECK(!ewb::const_time_eq("hunter2", "hunter22"), "prefix but shorter");
    CHECK(!ewb::const_time_eq("", "x"), "empty vs non-empty");
    CHECK(ewb::const_time_eq("", ""), "empty vs empty");
    CHECK(ewb::const_time_eq(std::string("\x00\x01", 2), std::string("\x00\x01", 2)),
          "embedded NUL is compared, not treated as terminator");
}

static void test_auth_failure_limiter() {
    // threshold 3, 60 s window, 10 s base cooldown doubling to 40 s cap.
    ewb::AuthFailureLimiter lim(3, 60.0, 10.0, 40.0);

    CHECK(!lim.blocked("10.0.0.1", 0.0), "unknown IP is not blocked");
    CHECK(lim.record_failure("10.0.0.1", 0.0) == 1, "first failure counted");
    CHECK(lim.record_failure("10.0.0.1", 1.0) == 2, "second failure counted");
    CHECK(!lim.blocked("10.0.0.1", 1.5), "still under threshold");
    CHECK(lim.record_failure("10.0.0.1", 2.0) == 3, "third failure hits threshold");
    CHECK(lim.blocked("10.0.0.1", 5.0), "blocked at base cooldown");
    CHECK(lim.blocked("10.0.0.1", 11.9), "still blocked just before cooldown ends");
    CHECK(!lim.blocked("10.0.0.1", 12.1), "unblocked after 10 s base cooldown");

    // A different IP is independent.
    CHECK(!lim.blocked("10.0.0.2", 12.1), "other IP unaffected");

    // Next failure doubles the cooldown to 20 s.
    CHECK(lim.record_failure("10.0.0.1", 13.0) >= 3, "failure while window still has hits");
    CHECK(lim.blocked("10.0.0.1", 32.0), "still blocked 19 s later (cooldown doubled to 20 s)");
    CHECK(!lim.blocked("10.0.0.1", 33.1), "unblocked after 20 s");

    // Disabled limiter never blocks.
    ewb::AuthFailureLimiter off(0);
    for (int i = 0; i < 100; ++i) off.record_failure("1.2.3.4", (double)i);
    CHECK(!off.blocked("1.2.3.4", 100.0), "threshold 0 disables the lockout");

    // Table full -> evict oldest rather than fail open.
    ewb::AuthFailureLimiter small(1, 60.0, 10.0, 40.0, /*max_tracked=*/4);
    for (int i = 0; i < 4; ++i) small.record_failure("192.168.0." + std::to_string(i), (double)i);
    CHECK(small.tracked() <= 4, "table capped");
    small.record_failure("192.168.9.9", 100.0);
    CHECK(small.tracked() <= 4, "still capped after a new IP");
    CHECK(small.blocked("192.168.9.9", 100.5), "the new IP is tracked and blocked, not dropped");
}

// --- text sanitisation (stages 1.5/1.6/1.7) ----------------------------------

static void test_sanitize_text() {
    CHECK(ewb::sanitize_text("hello", 64) == "hello", "plain text is untouched");
    CHECK(ewb::sanitize_text("a\nb\r\tc", 64) == "abc", "framing characters are stripped");
    CHECK(ewb::sanitize_text("hey its sam", 64) == "hey its sam", "the captured chat line survives");
    CHECK(ewb::sanitize_text("a:b", 64) == "a:b", "':' is legal in chat text — it is field-last");
    CHECK(ewb::sanitize_text(std::string(300, 'x'), 256).size() == 256, "length cap");
    // UTF-8 passes through: only ASCII control bytes are removed.
    CHECK(ewb::sanitize_text("caf\xC3\xA9", 64) == "caf\xC3\xA9", "UTF-8 survives");
}

// --- parseMessage: hand-rolled split matches std::getline (stage 7.13) ----------
//
// server_posix.cpp's parseMessage() used to build a std::stringstream and
// std::getline() out of it for every inbound line — expensive, and POSVEL (the
// highest-frequency verb on the wire) pays for it most. Stage 7.13 replaced it
// with a hand-rolled ':'-delimited substr split. This is not a header under
// test — server_posix.cpp is a full program (sockets, threads, main()) that
// this offline suite does not link — so both implementations are reproduced
// here verbatim and checked against each other instead of against the shipped
// symbol. Keep these two in lockstep with server_posix.cpp's parseMessage() and
// its old implementation if either ever changes.

// The original implementation (pre-7.13), reproduced for comparison only.
static std::vector<std::string> parseMessage_oldStringstream(const std::string& message) {
    std::vector<std::string> parts;
    std::stringstream ss(message);
    std::string part;
    while (std::getline(ss, part, ':')) {
        parts.push_back(part);
    }
    return parts;
}

// The stage-7.13 implementation, reproduced for comparison only — must be kept
// byte-for-byte identical to server_posix.cpp's parseMessage().
static std::vector<std::string> parseMessage_newSplit(const std::string& message) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start < message.size()) {
        const size_t colon = message.find(':', start);
        if (colon == std::string::npos) {
            parts.push_back(message.substr(start));
            break;
        }
        parts.push_back(message.substr(start, colon - start));
        start = colon + 1;
    }
    return parts;
}

static void check_parseMessage_agree(const std::string& in, const char* label) {
    const std::vector<std::string> a = parseMessage_oldStringstream(in);
    const std::vector<std::string> b = parseMessage_newSplit(in);
    CHECK(a == b, label);
}

static void test_parse_message_equivalence() {
    check_parseMessage_agree("", "empty string");
    check_parseMessage_agree(":", "a single colon");
    check_parseMessage_agree("a:", "trailing colon");
    check_parseMessage_agree(":a", "leading colon");
    check_parseMessage_agree("abc", "no colons");
    check_parseMessage_agree("a::b", "consecutive colons (empty field in the middle)");
    check_parseMessage_agree("::::", "many consecutive colons, nothing else");
    check_parseMessage_agree("a:b:c", "plain three-field line");
    check_parseMessage_agree("POSVEL:1.0:2.0:3.0:0.1:0.2:0.3", "a realistic POSVEL line");
    check_parseMessage_agree("SIGNP:1:2:3:0:0:0:time 12:30: meet here",
                              "sign text carries its own colons");
    check_parseMessage_agree(std::string(1, ':'), "one-character string that is just ':'");
    check_parseMessage_agree(std::string(200, ':'), "200 consecutive colons");

    // A fuzz pass over random short strings drawn from a small alphabet weighted
    // toward ':' — this is the case most likely to expose an off-by-one in the
    // hand-rolled split versus std::getline's "no trailing empty field" quirk.
    std::mt19937 rng(0xE57713);  // fixed seed: deterministic, reproducible failures
    std::uniform_int_distribution<int> lenDist(0, 12);
    static const char alphabet[] = ":::abc012\n\t";
    std::uniform_int_distribution<int> charDist(0, (int)sizeof(alphabet) - 2);
    for (int trial = 0; trial < 5000; ++trial) {
        std::string s;
        const int len = lenDist(rng);
        for (int i = 0; i < len; ++i) s.push_back(alphabet[charDist(rng)]);
        check_parseMessage_agree(s, "fuzz");
    }
}

// --- signs removed with their block (stage 7.2) ----------------------------------

static bool same_signs(const std::vector<ewb::Sign>& p, const std::vector<ewb::Sign>& q) {
    if (p.size() != q.size()) return false;
    for (size_t i = 0; i < p.size(); ++i)
        if (p[i].x != q[i].x || p[i].y != q[i].y || p[i].z != q[i].z || p[i].a != q[i].a ||
            p[i].b != q[i].b || p[i].c != q[i].c || p[i].text != q[i].text)
            return false;
    return true;
}

static void test_remove_signs_on_block() {
    // Two signs on one block that differ in `a`, with a sign on each of the six
    // neighbouring blocks around and between them.
    static const int kNeighbour[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                                         {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    std::vector<ewb::Sign> v{{100, 34, 200, 0, 27, 0, "face 0"}};
    for (int i = 0; i < 6; ++i) {
        v.push_back({100 + kNeighbour[i][0], 34 + kNeighbour[i][1], 200 + kNeighbour[i][2],
                     0, 0, 0, "n" + std::to_string(i)});
        if (i == 2) v.push_back({100, 34, 200, 5, 38, 3, "face 5"});
    }

    std::vector<ewb::Sign> removed;
    CHECK(ewb::remove_signs_on_block(v, 100, 34, 200, &removed) == 2,
          "both signs on a block are removed, whatever their face");
    CHECK(removed.size() == 2 && removed[0].text == "face 0" && removed[1].text == "face 5",
          "the removed signs are handed back, in order");
    bool neighbours = v.size() == 6;
    for (size_t i = 0; neighbours && i < v.size(); ++i)
        neighbours = v[i].text == "n" + std::to_string(i);
    CHECK(neighbours, "a sign on each of the six neighbouring blocks survives, in order");

    const std::vector<ewb::Sign> before = v;
    CHECK(ewb::remove_signs_on_block(v, 100, 34, 200) == 0 && same_signs(v, before),
          "a block with no signs left removes nothing and leaves the list alone");
    CHECK(ewb::remove_signs_on_block(v, 7, 7, 7) == 0 && same_signs(v, before),
          "a miss leaves the list alone");
    std::vector<ewb::Sign> none;
    CHECK(ewb::remove_signs_on_block(none, 100, 34, 200) == 0 && none.empty(),
          "an empty list returns 0");
}

static void test_prune_signs() {
    std::vector<ewb::Sign> v;
    for (int i = 0; i < 6; ++i) v.push_back({i, 34, 0, 0, 0, 0, "s" + std::to_string(i)});
    int calls = 0;
    const auto gone = [&](const ewb::Sign& s) { ++calls; return s.x == 1 || s.x == 2 || s.x == 5; };

    std::vector<ewb::Sign> dropped;
    CHECK(ewb::prune_signs(v, gone, &dropped) == 3, "prune drops only where the predicate is true");
    CHECK(calls == 6, "the predicate is asked once per sign");
    CHECK(v.size() == 3 && v[0].text == "s0" && v[1].text == "s3" && v[2].text == "s4",
          "survivors keep their order");
    CHECK(dropped.size() == 3 && dropped[0].text == "s1" && dropped[1].text == "s2" &&
          dropped[2].text == "s5", "the dropped signs are handed back, in order");

    const std::vector<ewb::Sign> before = v;
    CHECK(ewb::prune_signs(v, [](const ewb::Sign&) { return false; }) == 0 && same_signs(v, before),
          "nothing to drop leaves the list alone");
    std::vector<ewb::Sign> none;
    calls = 0;
    CHECK(ewb::prune_signs(none, gone) == 0 && none.empty() && calls == 0,
          "an empty list is left alone");

    // The server's predicate: drop a sign only when its block is *stored* as air. An
    // absent cell is untouched base terrain, which may be solid, so it keeps its sign.
    const std::map<std::tuple<int, int, int>, int> world = {
        {{10, 32, 10}, 0},    // mined: stored as air
        {{11, 32, 10}, 13},   // built: stored solid
    };
    const auto storedAir = [&](const ewb::Sign& s) {
        const auto it = world.find(std::make_tuple(s.x, s.y, s.z));
        return it != world.end() && it->second == 0;
    };
    std::vector<ewb::Sign> w{{10, 32, 10, 3, 2, 2, "orphan"},
                             {11, 32, 10, 3, 2, 2, "on a block"},
                             {12, 32, 10, 3, 2, 2, "on base terrain"}};
    CHECK(ewb::prune_signs(w, storedAir) == 1 && w.size() == 2 && w[0].text == "on a block" &&
          w[1].text == "on base terrain", "only the sign on a stored-air block is pruned");
}

// --- bounded saved-position table (stage 7.23) ----------------------------------

static void test_lru_table() {
    ewb::LruTable<int> t(3);
    CHECK(t.put("a", 1) == 0 && t.put("b", 2) == 0 && t.put("c", 3) == 0 && t.size() == 3,
          "fills to the cap without evicting");
    CHECK(t.put("d", 4) == 1 && t.size() == 3, "a new key past the cap evicts exactly one");
    CHECK(t.find("a") == nullptr, "...the least recently written");
    CHECK(t.find("b") && *t.find("b") == 2 && t.find("d") && *t.find("d") == 4, "the rest survive");

    // A returning name is refreshed, not duplicated, and is then the newest.
    CHECK(t.put("b", 20) == 0 && t.size() == 3, "rewriting a key neither grows nor evicts");
    CHECK(*t.find("b") == 20, "...and updates the value");
    t.put("e", 5);   // evicts the oldest, which is now "c" (b was refreshed past it)
    CHECK(t.find("c") == nullptr && t.find("b") != nullptr, "refresh moves a key to the back");

    // find() is a read: it must not keep a row alive.
    ewb::LruTable<int> r(2);
    r.put("x", 1); r.put("y", 2);
    (void)r.find("x");
    r.put("z", 3);
    CHECK(r.find("x") == nullptr && r.find("y") != nullptr, "find() does not refresh");

    // for_each is oldest -> newest, so a save/reload round-trips recency.
    std::string order;
    t.for_each([&](const std::string& k, int) { order += k; });
    CHECK(order == "dbe", "for_each visits every entry once, oldest first");
    ewb::LruTable<int> o(4);
    o.put("p", 0); o.put("q", 0); o.put("r", 0); o.put("p", 0);
    order.clear();
    o.for_each([&](const std::string& k, int) { order += k; });
    CHECK(order == "qrp", "oldest first, refreshed key last");

    // Reload in file order into a smaller table keeps the newest rows.
    ewb::LruTable<int> small(2);
    o.for_each([&](const std::string& k, int v) { small.put(k, v); });
    CHECK(small.size() == 2 && small.find("r") && small.find("p") && !small.find("q"),
          "loading an oversized file keeps the newest rows");

    // set_capacity trims oldest-first; a zero cap is clamped, never a broken table.
    ewb::LruTable<int> c(5);
    for (int i = 0; i < 5; ++i) c.put(std::to_string(i), i);
    CHECK(c.set_capacity(2) == 3 && c.size() == 2 && c.find("4") && c.find("3"), "shrinking evicts oldest");
    CHECK(c.set_capacity(0) == 1 && c.capacity() == 1 && c.size() == 1, "cap 0 clamps to 1");

    // The point of it: a flood of distinct names cannot grow the table.
    ewb::LruTable<int> flood(100);
    for (int i = 0; i < 100000; ++i) flood.put("n" + std::to_string(i), i);
    CHECK(flood.size() == 100, "100k distinct names, table stays at the cap");
    CHECK(flood.find("n99999") && !flood.find("n0"), "...holding the newest");
}

// --- accept() failure policy (stages 7.10 / 7.25) --------------------------------

static void test_accept_error_policy() {
    using A = ewb::AcceptErrorAction;
    CHECK(ewb::classify_accept_error(EINTR) == A::Retry, "EINTR is routine");
    CHECK(ewb::classify_accept_error(ECONNABORTED) == A::Retry, "ECONNABORTED is routine");
    for (int e : {EMFILE, ENFILE, ENOBUFS, ENOMEM})
        CHECK(ewb::classify_accept_error(e) == A::BackoffSleep, "resource exhaustion must sleep");
    CHECK(ewb::classify_accept_error(EBADF) == A::LogRetry, "anything else is logged, rate-limited");
    CHECK(ewb::classify_accept_error(EPROTO) == A::LogRetry, "EPROTO too");
}

int main() {
    test_sign_parse();
    test_sign_text_cannot_break_framing();
    test_sign_burst();
    test_client_signp_parse();
    test_sign_file_line_round_trip();
    test_sign_upsert();
    test_remove_signs_on_block();
    test_prune_signs();
    test_cell_cap_headroom();
    test_explode_single_tnt_golden();
    test_explode_chain_budget();
    test_explode_cluster_completes();
    test_explode_stays_in_world();
    test_explode_protected_cells_untouched();
    test_explode_protected_tnt_does_not_chain();
    test_explode_reach();
    test_explode_blast_footprint();
    test_zone_restore_wire();
    test_zone_cell_set();
    test_revert_queue();
    test_zone_audit_agg();
    test_spawn_parse();
    test_motd_parse();
    test_username_validation();
    test_duplicate_names();
    test_format_move();
    test_action_extra_validation();
    test_verb_allowed_before_join();
    test_parse_move_float();
    test_move_bounds();
    test_parse_move_triples();
    test_spawn_format_cannot_over_read();
    test_token_bucket();
    test_connect_limiter();
    test_const_time_eq();
    test_auth_failure_limiter();
    test_sanitize_text();
    test_parse_message_equivalence();
    test_lru_table();
    test_accept_error_policy();
    test_password_sources();

    if (g_fail) {
        std::fprintf(stderr, "\n%d check(s) FAILED\n", g_fail);
        return 1;
    }
    std::printf("protocol_test: all checks passed\n");
    return 0;
}
