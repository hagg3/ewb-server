// snapz_codec_test.cpp — offline round-trip for ROADMAP-SERVER stage 1.2.
//
//   clang++ -std=c++17 -O2 snapz_codec_test.cpp -lz -o snapz_codec_test
//   ./snapz_codec_test           # run the assertions
//   ./snapz_codec_test --emit    # also print a frame on stdout for the Rust cross-check
//
// Verifies, without a running server:
//   * encode_snapz -> parse header -> b64 decode -> raw inflate  round-trips
//   * inflated length == count * 20   (the stage 1.2 exit criterion)
//   * every 5xLE-i32 record survives byte-for-byte, including negatives (air = -1)
//   * base64 output carries no '=' padding
//   * every deflate level 0..9 inflates to the same records (stage 12.1a: the level
//     is the server's choice), and level 1 is the cheaper, larger one
//   * SnapzEncoder (one z_stream, deflateReset per frame) is byte-identical to a
//     fresh encode_snapz at the same level, frame after frame, including empty ones
//
// The produced frame is additionally cross-checked against a reference Rust
// SNAPZ decoder (`decode_frame`) in a companion test client's own test suite
// (see `accepts_a_cpp_encoded_frame`).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "snapz_codec.h"

using ewb::SnapRec;

static int g_fail = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_fail;                                                        \
        }                                                                   \
    } while (0)

// A representative burst covering the plan's Cell -> record shapes.
static std::vector<SnapRec> sample_records() {
    return {
        // plain solid block: flag 0, real type
        {65312, 33, 65775, 0, 74},
        {65313, 33, 65775, 0, 8},
        // cleared cell -> air: flag 1, type -1
        {65540, 101, 65540, 1, -1},
        // painted base (255) at low y: flag 3, field-5 = paint colour
        {65600, 40, 65600, 3, 11},
        // negative world coords (server uses signed i32 throughout)
        {-4, -1, -32768, 0, 1},
        // extremes, to prove the LE packing
        {2147483647, -2147483648, 0, 0, 127},
    };
}

static void run_round_trip(bool emit) {
    const std::vector<SnapRec> recs = sample_records();
    const std::string frame = ewb::encode_snapz(recs);

    // --- frame shape ---
    CHECK(frame.rfind("SNAPZ:", 0) == 0, "frame starts with SNAPZ:");
    CHECK(!frame.empty() && frame.back() == '\n', "frame ends with newline");

    const std::string body = frame.substr(0, frame.size() - 1);  // drop '\n'
    const size_t c1 = body.find(':');
    const size_t c2 = body.find(':', c1 + 1);
    CHECK(c1 != std::string::npos && c2 != std::string::npos, "frame has two ':' separators");

    const long count = std::strtol(body.substr(c1 + 1, c2 - c1 - 1).c_str(), nullptr, 10);
    const std::string b64 = body.substr(c2 + 1);
    CHECK(count == static_cast<long>(recs.size()), "header count matches record count");
    CHECK(b64.find('=') == std::string::npos, "base64 is unpadded");

    // --- decode path (mirrors snapz.rs) ---
    const std::vector<uint8_t> compressed = ewb::b64_decode(b64);
    const std::vector<uint8_t> raw =
        ewb::raw_inflate(compressed.data(), compressed.size(), size_t(count) * 20);

    CHECK(raw.size() == size_t(count) * 20, "inflated length == count * 20");

    bool all_match = raw.size() == recs.size() * 20;
    for (size_t i = 0; i < recs.size() && all_match; ++i) {
        const uint8_t* p = raw.data() + i * 20;
        const SnapRec got{ewb::get_le_i32(p),      ewb::get_le_i32(p + 4),
                          ewb::get_le_i32(p + 8),  ewb::get_le_i32(p + 12),
                          ewb::get_le_i32(p + 16)};
        all_match = got.x == recs[i].x && got.y == recs[i].y && got.z == recs[i].z &&
                    got.flag == recs[i].flag && got.type == recs[i].type;
    }
    CHECK(all_match, "every record round-trips byte-for-byte");

    // --- deflate really was headerless (raw, windowBits -15) ---
    // A zlib-wrapped stream would start 0x78; raw deflate does not.
    CHECK(!compressed.empty() && compressed[0] != 0x78, "stream is raw deflate, not zlib-wrapped");

    if (emit) {
        // stdout: just the frame, for `./snapz_codec_test --emit | ...`
        std::fwrite(frame.data(), 1, frame.size(), stdout);
    } else {
        std::printf("round-trip OK: %ld records, %zu compressed bytes, %zu b64 chars\n",
                    count, compressed.size(), b64.size());
    }
}

// Empty burst is a legal frame (count 0).
static void run_empty() {
    const std::string frame = ewb::encode_snapz({});
    const std::string body = frame.substr(0, frame.size() - 1);
    const size_t c1 = body.find(':');
    const size_t c2 = body.find(':', c1 + 1);
    const long count = std::strtol(body.substr(c1 + 1, c2 - c1 - 1).c_str(), nullptr, 10);
    CHECK(count == 0, "empty burst -> count 0");
    const std::vector<uint8_t> raw = ewb::raw_inflate(
        ewb::b64_decode(body.substr(c2 + 1)).data(),
        ewb::b64_decode(body.substr(c2 + 1)).size(), 0);
    CHECK(raw.empty(), "empty burst inflates to 0 bytes");
}

// Stage 7.30: raw_inflate must throw on input that can never finish and on output past
// its cap, rather than doubling its buffer forever.
template <class F>
static bool throws(F f) {
    try { f(); } catch (const std::exception&) { return true; }
    return false;
}

static void run_bounded_inflate() {
    CHECK(throws([] { ewb::raw_inflate(nullptr, 0); }), "empty input throws instead of spinning");

    const std::vector<uint8_t> zeros(1 << 20, 0);
    const std::vector<uint8_t> packed = ewb::raw_deflate(zeros);
    CHECK(ewb::raw_inflate(packed.data(), packed.size()).size() == zeros.size(),
          "a well-formed stream still inflates under the default cap");

    CHECK(throws([&] { ewb::raw_inflate(packed.data(), packed.size() / 2); }),
          "a truncated stream throws");
    CHECK(throws([&] { ewb::raw_inflate(packed.data(), packed.size(), 0, zeros.size() / 2); }),
          "output past max_out throws (decompression bomb)");
    CHECK(ewb::raw_inflate(packed.data(), packed.size(), 0, zeros.size()).size() == zeros.size(),
          "output exactly at max_out is allowed");
    CHECK(throws([] { const uint8_t junk[4] = {0xff, 0xff, 0xff, 0xff};
                      ewb::raw_inflate(junk, sizeof junk); }),
          "garbage throws");
}

// Decode a SNAPZ line back to its records.
static std::vector<SnapRec> decode_line(const std::string& line) {
    const std::string body = line.substr(0, line.size() - 1);
    const size_t c1 = body.find(':');
    const size_t c2 = body.find(':', c1 + 1);
    const std::vector<uint8_t> z = ewb::b64_decode(body.substr(c2 + 1));
    std::vector<uint8_t> raw;
    try { raw = ewb::raw_inflate(z.data(), z.size()); } catch (const std::exception&) { raw.clear(); }
    std::vector<SnapRec> out;
    for (size_t i = 0; i + 20 <= raw.size(); i += 20) {
        const uint8_t* p = raw.data() + i;
        out.push_back({ewb::get_le_i32(p), ewb::get_le_i32(p + 4), ewb::get_le_i32(p + 8),
                       ewb::get_le_i32(p + 12), ewb::get_le_i32(p + 16)});
    }
    return out;
}

static bool same_records(const std::vector<SnapRec>& a, const SnapRec* b, size_t n) {
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i)
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z || a[i].flag != b[i].flag ||
            a[i].type != b[i].type)
            return false;
    return true;
}

// A frame-sized burst shaped like real terrain: chunk-major runs of columns.
static std::vector<SnapRec> terrain_records(size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> t(1, 12), skip(0, 9);
    std::vector<SnapRec> v;
    int x = 65536, z = 65536, y = 0;
    while (v.size() < n) {
        if (skip(rng) == 0) v.push_back({x, y, z, 1, -1});
        else v.push_back({x, y, z, 0, t(rng)});
        if (skip(rng) == 0 && v.size() < n) v.push_back({x, y, z, 3, skip(rng)});
        if (++y == 40) { y = 0; if (++z % 16 == 0) { z -= 16; ++x; } }
    }
    v.resize(n);
    return v;
}

static void run_levels_and_reuse() {
    const std::vector<SnapRec> recs = terrain_records(3000, 7);
    size_t size1 = 0, size6 = 0;
    for (int level = 0; level <= 9; ++level) {
        const std::string line = ewb::encode_snapz(recs, level);
        CHECK(same_records(decode_line(line), recs.data(), recs.size()),
              "every level inflates to the same records");
        if (level == 1) size1 = line.size();
        if (level == 6) size6 = line.size();
    }
    CHECK(ewb::encode_snapz(recs) == ewb::encode_snapz(recs, 6), "the default level is still 6");
    CHECK(size1 >= size6, "level 1 is not smaller than level 6 (it trades bytes for CPU)");
    std::printf("  3000-record frame: level 1 %zu B, level 6 %zu B (+%.1f%%)\n", size1, size6,
                100.0 * ((double)size1 - (double)size6) / (double)size6);

    // One encoder, many frames of varying size: byte-identical to fresh encodes.
    for (int level : {1, 6, 9}) {
        ewb::SnapzEncoder enc(level);
        CHECK(enc.level() == level, "encoder keeps its level");
        bool same = true;
        for (unsigned f = 0; f < 40; ++f) {
            const std::vector<SnapRec> v = terrain_records(f % 7 == 0 ? 0 : 1 + (f * 977) % 3000, f);
            const std::string reused = enc.encode(v.data(), v.size());
            same = same && reused == ewb::encode_snapz(v.data(), v.size(), level);
        }
        CHECK(same, "a reused z_stream is byte-identical to a fresh one, frame after frame");
        CHECK(enc.encode(nullptr, 0) == ewb::encode_snapz(nullptr, 0, level), "and so is SNAPZ:0:");
    }
    CHECK(ewb::deflate_level_clamp(-3) == 0 && ewb::deflate_level_clamp(12) == 9 &&
              ewb::deflate_level_clamp(4) == 4, "levels clamp into 0..9");
}

int main(int argc, char** argv) {
    const bool emit = argc > 1 && std::strcmp(argv[1], "--emit") == 0;
    run_round_trip(emit);
    if (!emit) run_empty();
    if (!emit) run_bounded_inflate();
    if (!emit) run_levels_and_reuse();
    if (g_fail) {
        std::fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    if (!emit) std::printf("all checks passed\n");
    return 0;
}
