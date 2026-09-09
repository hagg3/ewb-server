// control_test.cpp — offline checks for ROADMAP-SERVER stage 3.2: the Tier 1
// operator control surface (control.h).
//
//   clang++ -std=c++17 -O2 -Wall control_test.cpp -o control_test
//   ./control_test
//
// Everything under test is pure: line splitting, the command table, the persisted
// ban list and op-level file, and `fill` volume arithmetic. The socket, the
// dispatch side effects (kick / broadcast / world edits) and the `stop` path are
// wire/process behaviour of server_posix.cpp and belong in the live socket test.

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "control.h"
#include "worldedit.h"   // WE_MAX_EDIT_CELLS — the cap the Tier 1 fill cap derives from

static int g_fail = 0;

#define CHECK(cond, msg)                                                             \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__);    \
            ++g_fail;                                                                \
        }                                                                            \
    } while (0)

static void test_split() {
    std::string v, r;
    CHECK(ewb::ctl_split("who\n", v, r) && v == "who" && r.empty(), "bare verb");
    CHECK(ewb::ctl_split("say:hello world\r\n", v, r) && v == "say" && r == "hello world",
          "verb:rest keeps spaces");
    CHECK(ewb::ctl_split("kick:Bob:being rude: again", v, r) && v == "kick" && r == "Bob:being rude: again",
          "rest keeps later colons");
    CHECK(!ewb::ctl_split("", v, r), "empty line rejected");
    CHECK(!ewb::ctl_split("\r\n", v, r), "CRLF-only line rejected");
    CHECK(ewb::ctl_split(":oops", v, r) == false, "leading colon => empty verb rejected");
}

static void test_fields() {
    auto f = ewb::ctl_fields("100:64:100:0:0:0:Welcome: home", 7);
    CHECK(f.size() == 7, "limit caps field count");
    CHECK(f[0] == "100" && f[6] == "Welcome: home", "last field keeps remaining colons");

    auto g = ewb::ctl_fields("a:b:c");
    CHECK(g.size() == 3 && g[1] == "b", "unlimited split");

    CHECK(ewb::ctl_fields("").empty(), "empty rest => no fields");

    auto one = ewb::ctl_fields("solo", 2);
    CHECK(one.size() == 1 && one[0] == "solo", "no colon => single field");
}

static void test_table() {
    CHECK(ewb::ctl_find("fill") != nullptr, "fill is a known verb");
    CHECK(ewb::ctl_find("nonsense") == nullptr, "unknown verb not found");
    const std::string help = ewb::ctl_help_text();
    for (const char* v : {"who", "say", "kick", "ban", "unban", "banlist", "save", "stop",
                          "op", "deop", "setblock", "fill", "signs", "region-stats"})
        CHECK(help.find(std::string("\n  ") + v) != std::string::npos ||
              help.find(std::string(v)) != std::string::npos,
              "help mentions every verb");
    // min/max arg bookkeeping the dispatcher relies on.
    CHECK(ewb::ctl_find("fill")->min_args == 7 && ewb::ctl_find("fill")->max_args == 8, "fill arity");
    CHECK(ewb::ctl_find("signs")->max_args == -1, "signs is free-form");
}

static void test_ip_heuristic() {
    CHECK(ewb::ctl_looks_like_ip("192.168.1.4"), "ipv4");
    CHECK(ewb::ctl_looks_like_ip("::1"), "ipv6 loopback");
    CHECK(!ewb::ctl_looks_like_ip("Player6835"), "username is not an ip");
    CHECK(!ewb::ctl_looks_like_ip("10.0.0.x"), "trailing letter => not an ip");
    CHECK(!ewb::ctl_looks_like_ip(""), "empty => not an ip");
}

static void test_banlist() {
    ewb::BanList b;
    CHECK(b.add("Griefer"), "add name");
    CHECK(b.add("203.0.113.9"), "add ip");
    CHECK(!b.add("Griefer"), "duplicate add is a no-op");
    CHECK(b.name_banned("Griefer") && !b.name_banned("griefer"), "name ban is exact-match");
    CHECK(b.ip_banned("203.0.113.9"), "ip ban");
    CHECK(b.banned("someone", "203.0.113.9"), "banned() matches on ip alone");
    CHECK(b.banned("Griefer", "1.2.3.4"), "banned() matches on name alone");

    // round-trip through the file format
    std::ostringstream out;
    b.serialize(out);
    ewb::BanList b2;
    std::istringstream in(out.str());
    b2.load(in);
    CHECK(b2.name_banned("Griefer") && b2.ip_banned("203.0.113.9"), "serialize/load round-trips");
    CHECK(b2.names.size() == 1 && b2.ips.size() == 1, "no phantom entries from the comment line");

    CHECK(b.remove("Griefer") && !b.name_banned("Griefer"), "remove name");
    CHECK(!b.remove("Griefer"), "remove of absent token is false");
    CHECK(b.remove("203.0.113.9") && b.empty(), "remove ip empties the list");
}

static void test_ops() {
    ewb::OpsFile o;
    CHECK(o.level_of("nobody", 0) == 0, "default when absent");
    CHECK(o.level_of("nobody", 1) == 1, "configurable default");
    CHECK(o.set("Alice", 2), "set a valid level");
    CHECK(!o.set("Bob", 3), "reject an out-of-range level");
    CHECK(!o.set("", 1), "reject an empty name");
    CHECK(o.level_of("Alice", 0) == 2, "stored level wins over default");
    CHECK(o.set("Alice", 1) && o.level_of("Alice", 0) == 1, "set replaces");

    std::ostringstream out;
    o.serialize(out);
    ewb::OpsFile o2;
    std::istringstream in(out.str());
    o2.load(in);
    CHECK(o2.level_of("Alice", 0) == 1, "ops file round-trips");

    CHECK(o.erase("Alice") && o.level_of("Alice", 0) == 0, "erase drops the entry");
    CHECK(!o.erase("Alice"), "erase of absent name is false");

    // a name containing ':' still round-trips (rfind splits on the LAST colon)
    ewb::OpsFile o3;
    std::istringstream weird("a:b:c:2\n");
    o3.load(weird);
    CHECK(o3.level_of("a:b:c", 0) == 2, "colon in name survives (rsplit)");
}

static void test_fill_volume() {
    CHECK(ewb::ctl_fill_volume(0, 0, 0, 0, 0, 0) == 1, "single cell");
    CHECK(ewb::ctl_fill_volume(0, 0, 0, 9, 9, 9) == 1000, "10x10x10");
    CHECK(ewb::ctl_fill_volume(9, 0, 9, 0, 9, 0) == 1000, "unordered corners");
    // a hostile pair must not overflow into a small positive number
    CHECK(ewb::ctl_fill_volume(0, 0, 0, 16777215, 255, 16777215) >
              ewb::ctl_fill_cap(ewb::WE_MAX_EDIT_CELLS, 4000000),
          "whole-world box exceeds the cap (no overflow)");
}

// Stage 3.4: the Tier 1 fill cap is *derived* from the Tier 2 per-command cap,
// so the two tiers can no longer drift apart. Pinned here because the shipped
// pair of numbers (131072 / 262144) is quoted in docs/commands.md.
static void test_fill_cap_derivation() {
    CHECK(ewb::ctl_fill_cap(ewb::WE_MAX_EDIT_CELLS, 4000000) == 262144,
          "default --we-max-cells derives the historical 262144 fill cap");
    CHECK(ewb::ctl_fill_cap(1000, 4000000) == 2000, "cap follows --we-max-cells");
    CHECK(ewb::ctl_fill_cap(4000000, 4000000) == 4000000,
          "clamped to the world's edited-cell ceiling");
    CHECK(ewb::ctl_fill_cap(0, 4000000) == 2, "a zero cap still allows setblock");
    CHECK(ewb::ctl_fill_cap(-5, 4000000) == 2, "a negative cap cannot open the gate");
}

// Stage 3.4: the control socket is the *more* privileged surface and had only
// the oversized-line guard. A caller that ignores the throttle must eventually
// be hung up on rather than throttled forever.
static void test_flood_guard() {
    ewb::CtlFlood f(4.0, 2.0);   // 4 at once, refilling 2/s

    double t = 100.0;
    for (int i = 0; i < 4; ++i)
        CHECK(f.check(t) == ewb::CtlFlood::Allow, "burst is spendable");
    CHECK(f.check(t) == ewb::CtlFlood::Throttle, "the 5th in the same instant is throttled");
    CHECK(f.strikes == 1, "a refusal is a strike");

    // Refusals accumulate to a disconnect.
    for (int i = 2; i < ewb::CTL_MAX_STRIKES; ++i)
        CHECK(f.check(t) == ewb::CtlFlood::Throttle, "still only throttled");
    CHECK(f.check(t) == ewb::CtlFlood::Disconnect, "enough refusals hangs up");

    // Backing off refills, and a successful command clears the strike count.
    ewb::CtlFlood g(4.0, 2.0);
    t = 200.0;
    for (int i = 0; i < 4; ++i) g.check(t);
    CHECK(g.check(t) == ewb::CtlFlood::Throttle, "dry");
    t += 2.0;                                   // 2 s at 2/s = 4 tokens
    CHECK(g.check(t) == ewb::CtlFlood::Allow, "refills over time");
    CHECK(g.strikes == 0, "a served command clears the strikes");

    // --control-rate 0 disables the guard entirely.
    ewb::CtlFlood off(4.0, 0.0);
    for (int i = 0; i < 1000; ++i)
        if (off.check(300.0) != ewb::CtlFlood::Allow) { CHECK(false, "rate 0 disables pacing"); break; }
    CHECK(off.strikes == 0, "rate 0 never strikes");
}

int main() {
    test_split();
    test_fields();
    test_table();
    test_ip_heuristic();
    test_banlist();
    test_ops();
    test_fill_volume();
    test_fill_cap_derivation();
    test_flood_guard();
    if (g_fail) { std::fprintf(stderr, "%d check(s) failed\n", g_fail); return 1; }
    std::puts("control_test: all checks passed");
    return 0;
}
