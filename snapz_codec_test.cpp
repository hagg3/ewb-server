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
//
// The produced frame is additionally fed to VuencLink's
// world::snapz::decode_frame by a #[test] in apps/vuenclink/src-tauri/src/world/snapz.rs
// (see `accepts_a_cpp_encoded_frame`).

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

int main(int argc, char** argv) {
    const bool emit = argc > 1 && std::strcmp(argv[1], "--emit") == 0;
    run_round_trip(emit);
    if (!emit) run_empty();
    if (g_fail) {
        std::fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    if (!emit) std::printf("all checks passed\n");
    return 0;
}
