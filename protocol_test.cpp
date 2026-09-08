// protocol_test.cpp — offline checks for ROADMAP-SERVER stages 1.5 and 1.7:
// the sign sidecar / `SIGNP` wire format, and the hardening primitives
// (username validation, `ACTION` payload validation, token bucket, per-IP
// connect limiter, text sanitisation).
//
//   clang++ -std=c++17 -O2 -Wall protocol_test.cpp -o protocol_test
//   ./protocol_test
//
// Everything under test is pure and takes its clock as a parameter, so nothing
// here sleeps or opens a socket. The join *sequence* (1.3), `PONG` (1.4) and the
// chat kill-switch removal (1.6) are wire-order properties of handleClient and are
// covered by the live socket test in the stage 1.8 ladder, not here.

#include <cstdio>
#include <string>
#include <vector>

#include "hardening.h"
#include "sign_store.h"

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

// --- usernames (stage 1.7) ---------------------------------------------------

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

int main() {
    test_sign_parse();
    test_sign_text_cannot_break_framing();
    test_sign_burst();
    test_username_validation();
    test_action_extra_validation();
    test_token_bucket();
    test_connect_limiter();
    test_sanitize_text();

    if (g_fail) {
        std::fprintf(stderr, "\n%d check(s) FAILED\n", g_fail);
        return 1;
    }
    std::printf("protocol_test: all checks passed\n");
    return 0;
}
