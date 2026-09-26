// matchmaker_test.cpp — offline checks for ROADMAP-SERVER Phase 2: the pure
// matchmaker logic in matchmaker.h (name sanitising, REGISTER parsing, SERVER:
// row / LIST formatting, and the TTL registry).
//
//   c++ -std=c++17 -O2 -Wall matchmaker_test.cpp -o matchmaker_test
//   ./matchmaker_test
//
// Everything under test is pure and takes its clock as a parameter, so nothing
// here sleeps or opens a socket. The accept loop and the HOST spawn in
// edenmatch.cpp are exercised by hand (see ROADMAP-SERVER §2.x).

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "matchmaker.h"

using namespace edenmatch;

static int g_fail = 0;

#define CHECK(cond, msg)                                                          \
    do {                                                                          \
        if (!(cond)) {                                                            \
            std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_fail;                                                             \
        }                                                                         \
    } while (0)

static void test_sanitize_name() {
    CHECK(sanitize_name("Build Battle Arena") == "Build Battle Arena", "plain name kept");
    CHECK(sanitize_name("  spaced  out  ") == "spaced out", "whitespace collapsed + trimmed");

    // --- framing safety: the only guarantee this function owes (stage 2.3) ---
    CHECK(sanitize_name("evil:name") == "evilname", "colon dropped (framing safety)");
    CHECK(sanitize_name("line\nbreak") == "linebreak", "newline dropped (framing safety)");
    CHECK(sanitize_name("carriage\rreturn") == "carriagereturn", "CR dropped (framing safety)");
    CHECK(sanitize_name("tabs\there") == "tabs here", "tab becomes a space");
    CHECK(sanitize_name("bell\x07" "and\x1b" "esc\x7f" "del") == "bellandescdel",
          "control bytes dropped");
    CHECK(sanitize_name("Rogue:203.0.113.5:1:9:1") == "Rogue203.0.113.5191",
          "a forged SERVER: row collapses into one harmless field");

    // --- widened rule: printable punctuation and non-ASCII now survive ---
    CHECK(sanitize_name("Ari's Server") == "Ari's Server", "apostrophe survives");
    CHECK(sanitize_name("[EU] Build-Battle #2 (24/7)!") == "[EU] Build-Battle #2 (24/7)!",
          "punctuation survives");
    CHECK(sanitize_name("émoji 🎮 test") == "émoji 🎮 test",
          "non-ASCII survives");
    CHECK(!sanitize_name("!!!").empty(), "a punctuation-only name is no longer erased");

    // --- length cap ---
    CHECK(MAX_NAME_LEN == 48, "cap is 48 bytes, inside the client's 64-byte name field");
    CHECK(sanitize_name(std::string(100, 'x')).size() == MAX_NAME_LEN, "length capped");
    // Truncation must not leave half a UTF-8 code point on the wire: 46 ASCII bytes
    // plus a 4-byte emoji would be cut at 48, so the whole emoji goes.
    CHECK(sanitize_name(std::string(46, 'x') + "🎮") == std::string(46, 'x'),
          "truncation drops a partial UTF-8 sequence rather than splitting it");
    CHECK(sanitize_name("") == "", "empty in, empty out");
    CHECK(sanitize_name(":::") == "", "colons-only name still sanitises to empty");
}

static void test_parse_register() {
    Registration r;

    CHECK(parse_register("REGISTER:My World:27015:0:203.0.113.5", "10.0.0.9", r),
          "full line parses");
    CHECK(r.name == "My World" && r.port == 27015 && !r.hasPassword && r.ip == "203.0.113.5",
          "fields + explicit advertise IP");

    CHECK(parse_register("REGISTER:My World:27015:1", "10.0.0.9", r), "3-field line parses");
    CHECK(r.hasPassword && r.ip == "10.0.0.9", "hasPassword=1, IP falls back to peer");

    CHECK(parse_register("REGISTER:My World:27015:0:", "10.0.0.9", r), "empty advertise field");
    CHECK(r.ip == "10.0.0.9", "empty advertise IP falls back to peer");

    CHECK(!parse_register("REGISTER:My World:27015", "1.2.3.4", r), "missing hasPassword rejected");
    CHECK(!parse_register("REGISTER::27015:0", "1.2.3.4", r), "empty name rejected");
    CHECK(parse_register("REGISTER:Ari's Server:27015:0", "1.2.3.4", r) &&
              r.name == "Ari's Server",
          "punctuation in a registered name survives sanitising (stage 2.3)");
    CHECK(!parse_register("REGISTER:\x01" "\x02:27015:0", "1.2.3.4", r),
          "name that sanitises to empty rejected");
    CHECK(!parse_register("REGISTER:x:0:0", "1.2.3.4", r), "port 0 rejected");
    CHECK(!parse_register("REGISTER:x:99999:0", "1.2.3.4", r), "port > 65535 rejected");
    CHECK(!parse_register("REGISTER:x:27o15:0", "1.2.3.4", r), "non-numeric port rejected");
    CHECK(!parse_register("REGISTER:x:27015:0", "", r), "no IP anywhere rejected");
    CHECK(!parse_register("LIST", "1.2.3.4", r), "non-REGISTER line rejected");

    // --- stage 7.20: the advertise field must be an IPv4 address ---
    CHECK(!parse_register("REGISTER:x:27015:0:not-an-ip", "1.2.3.4", r), "hostname-ish advertise rejected");
    CHECK(!parse_register("REGISTER:x:27015:0:1.2.3.4\x1b[2J", "1.2.3.4", r),
          "control bytes in advertise rejected");
    CHECK(!parse_register("REGISTER:x:27015:0:1.2.3", "1.2.3.4", r), "short dotted quad rejected");
    CHECK(!parse_register("REGISTER:x:27015:0:256.1.1.1", "1.2.3.4", r), "octet > 255 rejected");
    CHECK(!parse_register("REGISTER:x:27015:0: 1.2.3.4", "1.2.3.4", r), "leading space rejected");
    CHECK(parse_register("REGISTER:x:27015:0:::1", "1.2.3.4", r) && r.ip == "1.2.3.4",
          "an IPv6 literal splits into an empty field and falls back to the peer");
    Registration keep; keep.name = "untouched";
    CHECK(!parse_register("REGISTER:x:27015:0:bogus", "1.2.3.4", keep) && keep.name == "untouched",
          "a rejected line leaves `out` untouched");
}

static void test_advertise_policy() {
    Registration r;
    // peer 198.51.100.7 claims a different public server's address
    CHECK(parse_register("REGISTER:Free V-Bucks:27015:0:203.0.113.5", "198.51.100.7", r), "parses");
    CHECK(advertise_policy(r, "198.51.100.7", "198.51.100.7", false), "untrusted foreign claim overridden");
    CHECK(r.ip == "198.51.100.7", "listed under the peer's own address instead");

    CHECK(parse_register("REGISTER:x:27015:0:198.51.100.7", "198.51.100.7", r), "parses");
    CHECK(!advertise_policy(r, "198.51.100.7", "198.51.100.7", false) && r.ip == "198.51.100.7",
          "a peer may always advertise its own address");

    CHECK(parse_register("REGISTER:x:27015:0:203.0.113.5", "127.0.0.1", r), "parses");
    CHECK(!advertise_policy(r, "127.0.0.1", "127.0.0.1", true) && r.ip == "203.0.113.5",
          "a trusted peer's claim is honoured");

    // --advertise-ip set globally: the fallback is honoured as a claim too
    CHECK(parse_register("REGISTER:x:27015:0:192.0.2.1", "10.0.0.9", r), "parses");
    CHECK(!advertise_policy(r, "10.0.0.9", "192.0.2.1", false) && r.ip == "192.0.2.1",
          "claiming the operator's --advertise-ip is allowed");

    TrustList t;
    CHECK(t.trusted("127.0.0.1") && t.trusted("127.8.9.10"), "loopback is always trusted");
    CHECK(!t.trusted("10.0.0.9"), "nothing else is trusted by default");
    CHECK(!t.trusted("not-an-ip"), "junk is never trusted");
    CHECK(t.parse("192.168.1.0/24, 203.0.113.5"), "CIDR + bare address list parses");
    CHECK(t.trusted("192.168.1.200") && !t.trusted("192.168.2.1"), "/24 matched by prefix");
    CHECK(t.trusted("203.0.113.5") && !t.trusted("203.0.113.6"), "bare address is a /32");
    CHECK(!t.parse("10.0.0.0/33") && t.trusted("203.0.113.5"), "bad prefix rejected, list unchanged");
    CHECK(!t.parse("example.com"), "hostname rejected");
    CHECK(t.parse("0.0.0.0/0") && t.trusted("8.8.8.8"), "/0 trusts everyone (explicit opt-in)");
}

static void test_registry_ownership() {
    using R = Registry::AddResult;
    Registry reg;
    Registration victim; victim.name = "Real"; victim.ip = "203.0.113.5"; victim.port = 27015;
    victim.peer = "203.0.113.5";
    CHECK(reg.add(victim, 10, 0) == R::Ok, "victim registers");
    reg.set_players(10, 4);

    // A different peer that somehow holds the same ip:port claim (e.g. a trusted
    // peer, or the operator's --advertise-ip) cannot displace a live row.
    Registration attacker = victim; attacker.name = "Free V-Bucks"; attacker.peer = "198.51.100.7";
    uint64_t displaced = 999;
    CHECK(reg.add(attacker, 20, 1, &displaced) == R::Taken, "foreign peer refused");
    CHECK(displaced == 0, "nothing displaced");
    auto snap = reg.snapshot();
    CHECK(snap.size() == 1 && snap[0].name == "Real" && snap[0].conn == 10, "victim row intact");

    // The same peer on a new connection (a restarted server) does displace it.
    Registration restarted = victim; restarted.name = "Real v2";
    CHECK(reg.add(restarted, 11, 2, &displaced) == R::Ok && displaced == 10,
          "same peer replaces its own stale connection");
    snap = reg.snapshot();
    CHECK(snap[0].players == 4, "player count carried across the restart");

    // An orphaned row is reclaimable by anyone whose claim passed advertise_policy.
    reg.orphan(11);
    CHECK(reg.add(attacker, 21, 3, &displaced) == R::Ok && displaced == 0,
          "orphaned row reclaimed (no live owner to displace)");

    // One connection owns one row: re-REGISTER on a new port moves it.
    Registry r2;
    Registration a; a.name = "A"; a.ip = "1.1.1.1"; a.port = 27015; a.peer = "1.1.1.1";
    CHECK(r2.add(a, 30, 0) == R::Ok, "add");
    for (int p = 27016; p < 27100; ++p) {
        Registration m = a; m.port = p;
        r2.add(m, 30, 0);
    }
    CHECK(r2.size() == 1 && r2.snapshot()[0].port == 27099, "port-hopping re-REGISTERs keep one row");

    // Per-peer cap on new rows.
    Registry r3;
    size_t ok = 0;
    for (int i = 0; i < 40; ++i) {
        Registration m = a; m.port = 30000 + i;
        if (r3.add(m, 100 + i, 0, nullptr, MAX_REGISTRATIONS_PER_PEER) == R::Ok) ++ok;
    }
    CHECK(ok == MAX_REGISTRATIONS_PER_PEER, "untrusted peer capped at MAX_REGISTRATIONS_PER_PEER rows");
    Registration m = a; m.port = 30000;
    CHECK(r3.add(m, 100, 1, nullptr, MAX_REGISTRATIONS_PER_PEER) == R::Ok,
          "re-REGISTER of an existing row is not blocked by the cap");
    Registration other = a; other.ip = other.peer = "2.2.2.2";
    CHECK(r3.add(other, 500, 0, nullptr, MAX_REGISTRATIONS_PER_PEER) == R::Ok, "another peer unaffected");
    Registration loop = a; loop.peer = "127.0.0.1";
    size_t okLoop = 0;
    for (int i = 0; i < 40; ++i) {
        loop.port = 31000 + i;
        if (r3.add(loop, 600 + i, 0, nullptr, 0) == R::Ok) ++okLoop;
    }
    CHECK(okLoop == 40, "maxPerPeer 0 (trusted peer) is uncapped");
}

static void test_format_rows() {
    Registration r;
    r.name = "Discordia";
    r.ip = "203.0.113.5";
    r.port = 27600;
    r.hasPassword = true;
    r.players = 1;
    r.flag6 = 0;

    // Default is the 7-field grammar read off the live matchmaker's wire (stage 2.3).
    CHECK(format_server_row(r) == "SERVER:Discordia:203.0.113.5:27600:1:1:0",
          "default row is the 7-field capture grammar");
    CHECK(format_server_row(r, RowForm::Capture7) == format_server_row(r),
          "Capture7 is the default");
    // The two narrower attested forms stay available for an A/B.
    CHECK(format_server_row(r, RowForm::Prod6) == "SERVER:Discordia:203.0.113.5:27600:1:1",
          "6-field form drops flag6");
    CHECK(format_server_row(r, RowForm::Sketch4) == "SERVER:Discordia:203.0.113.5:27600:1",
          "4-field sketch form");

    // flag6 is its own column: it tracks neither `locked` nor `players`.
    Registration pvp = r;
    pvp.name = "PvP Arena";
    pvp.hasPassword = false;
    pvp.players = 0;
    pvp.flag6 = 1;
    CHECK(format_server_row(pvp) == "SERVER:PvP Arena:203.0.113.5:27600:0:0:1",
          "flag6 emitted independently of locked/players");

    std::vector<Registration> none;
    CHECK(format_list(none) == "END\n", "empty list is a bare END");
    CHECK(format_list(none, RowForm::Sketch4) == "END\n", "empty list is a bare END in any form");

    std::vector<Registration> two = {r, r};
    CHECK(format_list(two) ==
              "SERVER:Discordia:203.0.113.5:27600:1:1:0\n"
              "SERVER:Discordia:203.0.113.5:27600:1:1:0\n"
              "END\n",
          "list is rows then END");
    CHECK(format_list(two, RowForm::Prod6) ==
              "SERVER:Discordia:203.0.113.5:27600:1:1\n"
              "SERVER:Discordia:203.0.113.5:27600:1:1\n"
              "END\n",
          "list honours the row form");
}

static void test_parse_ping() {
    int n = -1;

    CHECK(!parse_ping("PING", n), "bare PING: no update");
    CHECK(parse_ping("PING:0", n) && n == 0, "PING:0");
    CHECK(parse_ping("PING:7", n) && n == 7, "PING:7");
    CHECK(parse_ping("PING:99999", n) && n == MAX_REPORTED_PLAYERS, "PING:99999 clamps");
    CHECK(!parse_ping("PING:-1", n), "PING:-1 ignored, not rejected as malformed heartbeat");
    CHECK(!parse_ping("PING:abc", n), "PING:abc ignored");
    CHECK(!parse_ping("PING:", n), "PING: (empty arg) ignored");
    CHECK(!parse_ping("PONG:1", n), "non-PING line rejected");
}

static void test_world_slug() {
    CHECK(world_slug("Ari's Server") == "ari_s_server", "punctuation collapses to underscores");
    CHECK(world_slug("My World") == "my_world", "space collapses to underscore");
    CHECK(world_slug("  padded  ") == "padded", "leading/trailing separators trimmed");
    CHECK(world_slug("!!!") == "world", "punctuation-only name falls back to 'world'");
    CHECK(world_slug("") == "world", "empty name falls back to 'world'");
    CHECK(world_slug("../../etc/passwd") == "etc_passwd", "path traversal slugs to something inert");
    CHECK(world_slug("émoji 🎮").find('/') == std::string::npos &&
          world_slug("émoji 🎮").find(':') == std::string::npos,
          "non-ASCII bytes never survive into the slug");
    CHECK(world_slug(std::string(100, 'x')).size() == MAX_SLUG_LEN, "length capped");
    CHECK(world_slug("Arena") == world_slug("arena"), "case-folded");
}

static void test_find_live_by_name() {
    std::vector<Registration> regs;
    Registration a; a.name = "Build Battle"; a.ip = "1.1.1.1"; a.port = 27015; a.conn = 5;
    Registration b; b.name = "Orphaned World"; b.ip = "2.2.2.2"; b.port = 27016; b.conn = 0;
    regs = {a, b};

    const Registration* found = find_live_by_name(regs, "build battle");
    CHECK(found && found->ip == "1.1.1.1", "case-insensitive match on a live entry");
    CHECK(find_live_by_name(regs, "Orphaned World") == nullptr,
          "an orphaned entry (conn == 0) is not a match");
    CHECK(find_live_by_name(regs, "nope") == nullptr, "no match for an unknown name");
}

static void test_registry() {
    Registry reg;
    Registration a;
    a.name = "A"; a.ip = "1.1.1.1"; a.port = 27015;
    Registration b;
    b.name = "B"; b.ip = "2.2.2.2"; b.port = 27016;

    CHECK(reg.add(a, /*conn*/ 10, /*now*/ 0) == Registry::AddResult::Ok, "add A");
    CHECK(reg.add(b, /*conn*/ 11, /*now*/ 0) == Registry::AddResult::Ok, "add B");
    CHECK(reg.size() == 2, "two entries");

    // Re-register same (name, ip, port) from a new connection: replaces, and
    // reports the displaced connection id.
    uint64_t displaced = 999;
    CHECK(reg.add(a, /*conn*/ 12, /*now*/ 5, &displaced) == Registry::AddResult::Ok, "re-add A on a new conn");
    CHECK(reg.size() == 2, "still two entries (replaced, not appended)");
    CHECK(displaced == 10, "old conn id reported as displaced");

    // TTL sweep: A last seen at 5, B at 0. At now=50, B is stale (>45s), A is not.
    auto dropped = reg.sweep(50);
    CHECK(dropped.size() == 1 && dropped[0] == 11, "B swept, its conn id returned");
    CHECK(reg.size() == 1, "only A remains");

    // touch() keeps A alive past what would otherwise be its TTL.
    reg.touch(12, 60);
    CHECK(reg.sweep(100).empty(), "touched A survives a later sweep");
    CHECK(reg.sweep(200).size() == 1, "A finally swept once its touch ages out");

    // remove() by connection id.
    CHECK(reg.add(b, 20, 0) == Registry::AddResult::Ok, "re-add B");
    reg.remove(20);
    CHECK(reg.size() == 0, "remove by conn id");
}

static void test_players_and_ordering() {
    Registry reg;
    Registration a; a.name = "A"; a.ip = "1.1.1.1"; a.port = 27015;
    Registration b; b.name = "B"; b.ip = "2.2.2.2"; b.port = 27016;
    reg.add(a, 10, 0);
    reg.add(b, 11, 0);

    reg.set_players(10, 3);
    reg.set_players(11, 7);
    auto snap = reg.snapshot();
    CHECK(snap.size() == 2, "two entries after set_players");

    // list is players-desc: B (7) before A (3).
    CHECK(format_list(snap) ==
              "SERVER:B:2.2.2.2:27016:0:7:0\n"
              "SERVER:A:1.1.1.1:27015:0:3:0\n"
              "END\n",
          "list sorted players-desc");

    // A re-register (e.g. a rename picked up via a fresh REGISTER on the same
    // conn) must not flicker the count back to 0.
    Registration a2 = a;
    reg.add(a2, 10, 1);
    auto snap2 = reg.snapshot();
    auto itA = std::find_if(snap2.begin(), snap2.end(),
                             [](const Registration& r) { return r.conn == 10; });
    CHECK(itA != snap2.end() && itA->players == 3, "player count survives re-register");

    // set_players on an unknown conn is a harmless no-op.
    reg.set_players(999, 42);
    CHECK(reg.snapshot().size() == 2, "set_players on unknown conn is a no-op");

    // Tie on players sorts by name.
    Registration c; c.name = "C"; c.ip = "3.3.3.3"; c.port = 27017;
    reg.add(c, 12, 0);
    reg.set_players(12, 3);  // ties A at 3
    auto snap3 = reg.snapshot();
    CHECK(format_list(snap3) ==
              "SERVER:B:2.2.2.2:27016:0:7:0\n"
              "SERVER:A:1.1.1.1:27015:0:3:0\n"
              "SERVER:C:3.3.3.3:27017:0:3:0\n"
              "END\n",
          "tie on players breaks by name");
}

static void test_dedupe_by_ip_port() {
    Registry reg;
    Registration a; a.name = "Alpha"; a.ip = "1.1.1.1"; a.port = 27015;
    CHECK(reg.add(a, 10, 0) == Registry::AddResult::Ok, "add Alpha");

    // A rename from the SAME connection's REGISTER, same ip:port, different
    // name: must replace (dedupe on ip:port), not create a second row (2.2).
    Registration renamed = a; renamed.name = "Alpha Renamed";
    uint64_t displaced = 999;
    CHECK(reg.add(renamed, 10, 1, &displaced) == Registry::AddResult::Ok, "re-register with a new name");
    CHECK(reg.size() == 1, "still one entry — no double-listing on rename");
    auto snap = reg.snapshot();
    CHECK(snap.size() == 1 && snap[0].name == "Alpha Renamed", "row picked up the new name");
}

static void test_orphan_and_probe_sweep() {
    Registry reg;
    Registration a; a.name = "A"; a.ip = "1.1.1.1"; a.port = 27015;
    Registration b; b.name = "B"; b.ip = "2.2.2.2"; b.port = 27016;
    reg.add(a, 10, 0);
    reg.add(b, 11, 0);

    // Registration socket closes: orphan(), not remove() — the row survives.
    reg.orphan(10);
    CHECK(reg.size() == 2, "orphaned entry is not delisted");
    auto snap = reg.snapshot();
    auto itA = std::find_if(snap.begin(), snap.end(),
                             [](const Registration& r) { return r.ip == "1.1.1.1"; });
    CHECK(itA != snap.end() && itA->conn == 0, "orphaned entry's conn id is cleared");

    // No probe given: reproduces the old unconditional-drop behaviour.
    CHECK(reg.sweep(100).size() == 2, "no probe -> both entries dropped once stale");

    // Rebuild and sweep with a probe: A's address answers, B's doesn't.
    Registry reg2;
    reg2.add(a, 10, 0);
    reg2.add(b, 11, 0);
    reg2.orphan(10);
    auto probe = [](const std::string& ip, int) { return ip == "1.1.1.1"; };
    auto dropped = reg2.sweep(100, probe);
    CHECK(dropped.size() == 1, "only the unreachable entry is dropped");
    CHECK(reg2.size() == 1, "the reachable (orphaned) entry survives");
    auto snap2 = reg2.snapshot();
    CHECK(snap2[0].ip == "1.1.1.1", "the surviving entry is A");
    CHECK(reg2.sweep(101, probe).empty(), "a probed-alive entry's TTL was refreshed");
}

static void test_persistence_roundtrip() {
    Registration r;
    r.name = "My World"; r.ip = "203.0.113.5"; r.port = 27015;
    r.hasPassword = true; r.players = 3; r.flag6 = 0;

    std::string line = serialize_entry(r);
    Registration back;
    CHECK(parse_persisted_line(line, back), "persisted line parses");
    CHECK(back.name == r.name && back.ip == r.ip && back.port == r.port &&
              back.hasPassword == r.hasPassword && back.players == r.players,
          "round-trips every field");

    CHECK(!parse_persisted_line("garbage:not:enough:fields", back), "wrong field count rejected");
    CHECK(!parse_persisted_line("Name:1.2.3.4:notaport:0:0:0", back), "bad port rejected");
    CHECK(!parse_persisted_line("Name:evil\x07:27015:0:0:0", back), "non-IP address rejected (7.20)");

    std::vector<Registration> two = {r, r};
    CHECK(serialize_registry(two) == line + "\n" + line + "\n", "serialize_registry is one line each");
}

static void test_load_stale() {
    Registry reg;
    Registration a; a.name = "A"; a.ip = "1.1.1.1"; a.port = 27015; a.players = 5;
    std::vector<Registration> persisted = {a};

    reg.load_stale(persisted, /*now*/ 1000);
    CHECK(reg.size() == 1, "loaded entry present");
    auto snap = reg.snapshot();
    CHECK(snap[0].conn == 0, "loaded entry starts orphaned");
    CHECK(snap[0].players == 5, "loaded entry keeps its persisted player count");

    // It must be eligible for an immediate probe, not sit around for a full TTL.
    bool probed = false;
    auto probe = [&](const std::string&, int) { probed = true; return true; };
    reg.sweep(1000, probe);
    CHECK(probed, "a freshly loaded entry is probed on the very next sweep");
}

static void test_conn_caps() {
    ConnCaps caps;
    for (int i = 0; i < MAX_CONNS_PER_IP; ++i)
        CHECK(caps.tryAcquire("1.2.3.4"), "per-IP slot available under the cap");
    CHECK(!caps.tryAcquire("1.2.3.4"), "per-IP cap refuses the next one");
    CHECK(caps.tryAcquire("5.6.7.8"), "a different IP is unaffected by another's per-IP cap");

    caps.release("1.2.3.4");
    CHECK(caps.tryAcquire("1.2.3.4"), "releasing one slot frees it back up");

    ConnCaps global;
    for (int i = 0; i < MAX_CONNS_GLOBAL; ++i) {
        std::string ip = "10.0.0." + std::to_string(i % 250);
        if (!global.tryAcquire(ip)) { CHECK(false, "should not exhaust before the global cap"); break; }
    }
    CHECK(global.total() == MAX_CONNS_GLOBAL, "global counter tracks total acquisitions");
    CHECK(!global.tryAcquire("10.0.1.1"), "global cap refuses beyond MAX_CONNS_GLOBAL even on a fresh IP");
}

int main() {
    test_sanitize_name();
    test_parse_register();
    test_advertise_policy();
    test_registry_ownership();
    test_parse_ping();
    test_format_rows();
    test_registry();
    test_players_and_ordering();
    test_dedupe_by_ip_port();
    test_orphan_and_probe_sweep();
    test_persistence_roundtrip();
    test_load_stale();
    test_conn_caps();
    test_world_slug();
    test_find_live_by_name();

    if (g_fail) {
        std::fprintf(stderr, "matchmaker_test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("matchmaker_test: all checks passed\n");
    return 0;
}
