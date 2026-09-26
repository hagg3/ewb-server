// auth_test.cpp — offline checks for ROADMAP-SERVER stage 8.6: auth.h's SHA-256,
// HMAC-SHA-256 and PBKDF2-HMAC-SHA-256 against published vectors (FIPS 180-4
// examples, RFC 4231, RFC 7914 §11), the PIN rules, the eden_auth.txt grammar
// and the effective-level / zone-bypass rules.
//
//   c++ -std=c++17 -O2 -Wall auth_test.cpp -o auth_test
//   ./auth_test

#include <algorithm>
#include <chrono>
#include <functional>
#include <random>
#include <cstdio>
#include <sstream>
#include <string>

#include "auth.h"

using namespace ewb;

static int g_fail = 0;

#define CHECK(cond, msg)                                                          \
    do {                                                                          \
        if (!(cond)) {                                                            \
            std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_fail;                                                             \
        }                                                                         \
    } while (0)

// A deterministic "random" source: counts up from `seed`.
struct CountingRandom {
    uint8_t next;
    explicit CountingRandom(uint8_t seed) : next(seed) {}
    bool operator()(uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; ++i) p[i] = next++;
        return true;
    }
};

static void test_sha256() {
    CHECK(auth_hex(sha256("")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256('')");
    CHECK(auth_hex(sha256("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256('abc')");
    CHECK(auth_hex(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
          "sha256 two-block message");
    CHECK(auth_hex(sha256(std::string(1000000, 'a'))) ==
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
          "sha256 one million 'a'");

    // Streaming in odd-sized pieces must match one-shot.
    const std::string msg(1000, 'q');
    Sha256 s;
    for (size_t i = 0; i < msg.size(); i += 7) s.update(msg.data() + i, std::min<size_t>(7, msg.size() - i));
    uint8_t d[32];
    s.final(d);
    CHECK(std::string(reinterpret_cast<char*>(d), 32) == sha256(msg), "streamed update matches one-shot");
}

static void test_hmac() {
    // RFC 4231 test cases 1 and 6 (the latter's key is longer than a block).
    CHECK(auth_hex(hmac_sha256(std::string(20, '\x0b'), "Hi There")) ==
              "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
          "RFC 4231 case 1");
    CHECK(auth_hex(hmac_sha256(std::string(131, '\xaa'),
                               "Test Using Larger Than Block-Size Key - Hash Key First")) ==
              "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54",
          "RFC 4231 case 6 (long key)");
}

static void test_pbkdf2() {
    // RFC 7914 §11.
    CHECK(auth_hex(pbkdf2_sha256("passwd", "salt", 1, 64)) ==
              "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
              "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783",
          "RFC 7914 PBKDF2 c=1");
    CHECK(auth_hex(pbkdf2_sha256("Password", "NaCl", 80000, 64)) ==
              "4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56"
              "a1d425a1225833549adb841b51c9b3176a272bdebba1d078478f62b397f33c8d",
          "RFC 7914 PBKDF2 c=80000");
    // The shape the server uses: an 8-digit PIN, a 16-byte salt, 32-byte output
    // (value from Python's hashlib.pbkdf2_hmac).
    std::string salt;
    for (int i = 0; i < 16; ++i) salt.push_back(static_cast<char>(i));
    CHECK(auth_hex(pbkdf2_sha256("12345678", salt, 1000, 32)) ==
              "42b99a197a7d9cafa9fa5b60091da410edb6f3c1cc4e159a5f901b5348abb716",
          "PBKDF2 with the server's PIN/salt shape");
}

static void test_hex() {
    std::string out = "keep";
    CHECK(auth_unhex("00ff7Aa0", out) && out == std::string("\x00\xff\x7a\xa0", 4), "hex decodes both cases");
    out = "keep";
    CHECK(!auth_unhex("abc", out) && out == "keep", "odd-length hex refused, output untouched");
    CHECK(!auth_unhex("zz", out), "non-hex refused");
    CHECK(auth_hex(std::string("\x01\xab", 2)) == "01ab", "hex encodes lower-case");
}

static void test_pins() {
    CountingRandom rnd(0);
    std::string pin;
    CHECK(auth_make_pin(std::ref(rnd), pin), "pin generated");
    CHECK(pin == "01234567", "pin digits come from bytes mod 10");
    CHECK(auth_pin_wellformed(pin), "generated pin is well-formed");

    // Bytes 250..255 are rejected (they would bias 0..5).
    CountingRandom high(250);
    CHECK(auth_make_pin(std::ref(high), pin), "pin generated past rejected bytes");
    CHECK(pin == "01234567", "250..255 skipped: the stream restarts at 0 after wrapping");

    auto fails = [](uint8_t*, size_t) { return false; };
    pin = "keep";
    CHECK(!auth_make_pin(fails, pin) && pin == "keep", "a failed random source is reported, pin untouched");

    CHECK(!auth_pin_wellformed("1234567"), "7 digits refused");
    CHECK(!auth_pin_wellformed("123456789"), "9 digits refused");
    CHECK(!auth_pin_wellformed("1234 678"), "a space refused");
    CHECK(!auth_pin_wellformed("12345a78"), "a letter refused");
    CHECK(!auth_pin_wellformed(""), "empty refused");

    // Uniformity: over many draws each digit's share is close to 10%.
    std::mt19937 gen(7);
    auto mt = [&](uint8_t* p, size_t n) { for (size_t i = 0; i < n; ++i) p[i] = (uint8_t)gen(); return true; };
    int counts[10] = {0};
    for (int i = 0; i < 20000; ++i) {
        auth_make_pin(mt, pin);
        for (char c : pin) ++counts[c - '0'];
    }
    bool even = true;
    for (int c : counts) if (c < 15000 || c > 17000) even = false;   // expect 16000 each
    CHECK(even, "digits are uniformly distributed");
}

static void test_records() {
    CountingRandom rnd(9);
    AuthRecord r;
    CHECK(auth_make_record("24681357", std::ref(rnd), r, 2000), "record made");
    CHECK(r.salt.size() == AUTH_SALT_BYTES && r.hash.size() == AUTH_HASH_BYTES, "record sizes");
    CHECK(auth_verify(r, "24681357"), "right PIN verifies");
    CHECK(!auth_verify(r, "24681358"), "wrong PIN refused");
    CHECK(!auth_verify(r, "2468135"), "short PIN refused before hashing");
    CHECK(!auth_verify(r, "24681357 "), "trailing space refused");

    // Same PIN, different salt: different hash.
    AuthRecord r2;
    CountingRandom rnd2(100);
    auth_make_record("24681357", std::ref(rnd2), r2, 2000);
    CHECK(r2.hash != r.hash, "salt changes the hash");

    // Round trip through the line grammar.
    const std::string line = auth_serialize_line("Bob Smith", r);
    std::string name;
    AuthRecord back;
    CHECK(auth_parse_line(line, name, back), "serialised line parses");
    CHECK(name == "Bob Smith" && back.iterations == 2000 && back.salt == r.salt && back.hash == r.hash,
          "round trip keeps every field (a name may contain a space)");
    CHECK(auth_verify(back, "24681357"), "a reloaded record still verifies");
}

static void test_parse_invalid() {
    std::string name, why;
    AuthRecord r;
    const std::string salt(32, 'a'), hash(64, 'b');
    const std::string ok = "alice:pbkdf2-sha256:100000:" + salt + ":" + hash;
    CHECK(auth_parse_line(ok, name, r), "baseline parses");
    CHECK(!auth_parse_line("alice:pbkdf2-sha256:100000:" + salt, name, r, &why), "missing field refused");
    CHECK(!auth_parse_line("server:pbkdf2-sha256:100000:" + salt + ":" + hash, name, r), "reserved name refused");
    CHECK(!auth_parse_line("a]b:pbkdf2-sha256:100000:" + salt + ":" + hash, name, r), "bad name char refused");
    CHECK(!auth_parse_line("alice:sha1:100000:" + salt + ":" + hash, name, r, &why) && why == "unknown hash scheme",
          "unknown scheme refused");
    CHECK(!auth_parse_line("alice:pbkdf2-sha256:10:" + salt + ":" + hash, name, r), "too few iterations refused");
    CHECK(!auth_parse_line("alice:pbkdf2-sha256:99999999999:" + salt + ":" + hash, name, r), "too many iterations refused");
    CHECK(!auth_parse_line("alice:pbkdf2-sha256:-5:" + salt + ":" + hash, name, r), "negative iterations refused");
    CHECK(!auth_parse_line("alice:pbkdf2-sha256:1e5:" + salt + ":" + hash, name, r), "non-integer iterations refused");
    CHECK(!auth_parse_line("alice:pbkdf2-sha256:100000:abc:" + hash, name, r), "odd salt refused");
    CHECK(!auth_parse_line("alice:pbkdf2-sha256:100000:" + salt + ":" + std::string(62, 'b'), name, r),
          "short hash refused");
}

static void test_file() {
    CountingRandom rnd(1);
    AuthRecord a, b;
    auth_make_record("11111111", std::ref(rnd), a, 1000);
    auth_make_record("22222222", std::ref(rnd), b, 1000);
    AuthFile f;
    f.set("zed", a);
    f.set("amy", b);
    const std::string text = f.serialize();
    CHECK(text.find("11111111") == std::string::npos && text.find("22222222") == std::string::npos,
          "the file never contains a PIN");
    CHECK(text.find("amy:") < text.find("zed:"), "names are written sorted (deterministic saves)");

    std::istringstream in(text);
    AuthFile g;
    CHECK(AuthFile::load(in, g), "serialised file loads");
    CHECK(g.size() == 2 && g.has("amy") && g.has("zed"), "both names back");
    CHECK(auth_verify(*g.find("zed"), "11111111"), "zed's PIN still verifies after reload");

    // All-or-nothing.
    AuthFile keep;
    keep.set("keeper", a);
    std::string err;
    int line = 0;
    std::istringstream bad(text + "broken line\n");
    CHECK(!AuthFile::load(bad, keep, &err, &line), "a malformed line fails the load");
    CHECK(keep.size() == 1 && keep.has("keeper"), "a failed load leaves the target untouched");
    CHECK(line == 6, "the failing line number is reported (3 comment lines + 2 records + 1)");

    std::istringstream dup(auth_serialize_line("x", a) + "\n" + auth_serialize_line("x", b) + "\n");
    CHECK(!AuthFile::load(dup, keep, &err, &line) && err == "duplicate name" && line == 2, "duplicate refused");

    std::istringstream blank("\n# comment\n   \n");
    AuthFile empty;
    empty.set("gone", a);
    CHECK(AuthFile::load(blank, empty) && empty.empty(), "comments and blanks only: an empty set");

    CHECK(f.erase("amy") && !f.has("amy") && !f.erase("amy"), "erase once, not twice");
}

static void test_levels() {
    // No PIN: unchanged — the claimed name's level, logged in or not.
    CHECK(auth_effective_level(2, 0, false, false) == 2, "no PIN: file level applies (as before 8.6)");
    // A PIN, not logged in: the lower of default and file level.
    CHECK(auth_effective_level(2, 0, true, false) == 0, "PIN, not logged in: the default");
    CHECK(auth_effective_level(2, 1, true, false) == 1, "PIN, not logged in: default 1");
    CHECK(auth_effective_level(0, 1, true, false) == 0, "PIN, not logged in: never above the owner's own level");
    CHECK(auth_effective_level(2, 0, true, true) == 2, "PIN, logged in: the file level");

    CHECK(auth_zone_bypass_level(2, false) == -1, "not logged in: bypasses nothing");
    CHECK(auth_zone_bypass_level(2, true) == 2, "logged in: bypass at the effective level");
}

static void test_cost() {
    // Not a check — the number the iteration count was chosen against.
    CountingRandom rnd(3);
    AuthRecord r;
    auth_make_record("13572468", std::ref(rnd), r);
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = auth_verify(r, "13572468");
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ok, "default-cost record verifies");
    std::printf("  one /login verify at %u iterations: %.1f ms\n", (unsigned)AUTH_PBKDF2_ITERS, ms);
}

int main() {
    test_sha256();
    test_hmac();
    test_pbkdf2();
    test_hex();
    test_pins();
    test_records();
    test_parse_invalid();
    test_file();
    test_levels();
    test_cost();
    if (g_fail) {
        std::fprintf(stderr, "%d check(s) FAILED\n", g_fail);
        return 1;
    }
    std::printf("All auth_test checks passed.\n");
    return 0;
}
