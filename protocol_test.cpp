// protocol_test.cpp — offline checks for ROADMAP-SERVER stages 1.5, 1.7 and 1.10:
// the sign sidecar / `SIGNP` wire formats (including a player's sign write and its
// slot upsert, and removing signs with their block — stage 7.2), and the hardening primitives (username validation, `ACTION`
// payload validation, the world cell cap headroom, token bucket, per-IP connect
// limiter, constant-time password compare, per-IP failed-auth limiter, text
// sanitisation).
//
//   clang++ -std=c++17 -O2 -Wall protocol_test.cpp -o protocol_test
//   ./protocol_test
//
// Everything under test is pure and takes its clock as a parameter, so nothing
// here sleeps or opens a socket. The join *sequence* (1.3), `PONG` (1.4) and the
// chat kill-switch removal (1.6) are wire-order properties of handleClient and are
// covered by the live socket test in the stage 1.8 ladder, not here.

#include <cstdio>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "hardening.h"
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

// --- usernames (stage 1.7) ---------------------------------------------------

// --- world spawn sidecar (stage 5.3) ---------------------------------------

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
    test_spawn_parse();
    test_username_validation();
    test_action_extra_validation();
    test_token_bucket();
    test_connect_limiter();
    test_const_time_eq();
    test_auth_failure_limiter();
    test_sanitize_text();

    if (g_fail) {
        std::fprintf(stderr, "\n%d check(s) FAILED\n", g_fail);
        return 1;
    }
    std::printf("protocol_test: all checks passed\n");
    return 0;
}
