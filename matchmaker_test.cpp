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
    CHECK(sanitize_name("evil:name") == "evilname", "colon dropped (framing safety)");
    CHECK(sanitize_name("line\nbreak") == "linebreak", "newline dropped (framing safety)");
    CHECK(sanitize_name("tabs\there") == "tabs here", "tab becomes a space");
    CHECK(sanitize_name("!!!") == "", "punctuation-only name is empty");
    CHECK(sanitize_name(std::string(100, 'x')).size() == MAX_NAME_LEN, "length capped");
    CHECK(sanitize_name("émoji 🎮 test") == "moji test", "non-ASCII bytes dropped");
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
    CHECK(!parse_register("REGISTER:!!!:27015:0", "1.2.3.4", r), "name that sanitises to empty rejected");
    CHECK(!parse_register("REGISTER:x:0:0", "1.2.3.4", r), "port 0 rejected");
    CHECK(!parse_register("REGISTER:x:99999:0", "1.2.3.4", r), "port > 65535 rejected");
    CHECK(!parse_register("REGISTER:x:27o15:0", "1.2.3.4", r), "non-numeric port rejected");
    CHECK(!parse_register("REGISTER:x:27015:0", "", r), "no IP anywhere rejected");
    CHECK(!parse_register("LIST", "1.2.3.4", r), "non-REGISTER line rejected");
}

static void test_format_rows() {
    Registration r;
    r.name = "Discordia";
    r.ip = "45.79.193.87";
    r.port = 27600;
    r.hasPassword = true;
    r.players = 1;
    r.flag6 = 0;

    // 7-field grammar is the capture-confirmed browse row.
    CHECK(format_server_row(r) == "SERVER:Discordia:45.79.193.87:27600:1:1:0",
          "7-field row matches the capture grammar");
    // 4-field short form is the developer-sketch variant.
    CHECK(format_server_row(r, true) == "SERVER:Discordia:45.79.193.87:27600:1",
          "4-field short row");

    std::vector<Registration> none;
    CHECK(format_list(none) == "END\n", "empty list is a bare END");

    std::vector<Registration> two = {r, r};
    CHECK(format_list(two) ==
              "SERVER:Discordia:45.79.193.87:27600:1:1:0\n"
              "SERVER:Discordia:45.79.193.87:27600:1:1:0\n"
              "END\n",
          "list is rows then END");
}

static void test_registry() {
    Registry reg;
    Registration a;
    a.name = "A"; a.ip = "1.1.1.1"; a.port = 27015;
    Registration b;
    b.name = "B"; b.ip = "2.2.2.2"; b.port = 27016;

    CHECK(reg.add(a, /*conn*/ 10, /*now*/ 0), "add A");
    CHECK(reg.add(b, /*conn*/ 11, /*now*/ 0), "add B");
    CHECK(reg.size() == 2, "two entries");

    // Re-register same (name, ip, port) from a new connection: replaces, and
    // reports the displaced connection id.
    uint64_t displaced = 999;
    CHECK(reg.add(a, /*conn*/ 12, /*now*/ 5, &displaced), "re-add A on a new conn");
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
    CHECK(reg.add(b, 20, 0), "re-add B");
    reg.remove(20);
    CHECK(reg.size() == 0, "remove by conn id");
}

int main() {
    test_sanitize_name();
    test_parse_register();
    test_format_rows();
    test_registry();

    if (g_fail) {
        std::fprintf(stderr, "matchmaker_test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("matchmaker_test: all checks passed\n");
    return 0;
}
