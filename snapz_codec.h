// snapz_codec.h — raw DEFLATE + standard-alphabet base64 (no padding) + the
// `SNAPZ:<count>:<b64>` frame encoder for the Eden region-streaming protocol.
//
// ROADMAP-SERVER stage 1.2 (done before 1.1). The wire contract, from
// CAPTURE-FINDINGS.md Pass 2 and the native-client pcap:
//
//   SNAPZ:<count>:<base64>\n
//     base64 : standard alphabet (A-Za-z0-9+/), NO '=' padding
//     bytes  : raw DEFLATE — zlib windowBits = -15, no zlib/gzip header
//     inflated length == count * 20
//     each 20-byte record : five little-endian signed int32  (x, y, z, flag, type)
//
// This must be byte-compatible with the two Rust reference implementations it is
// verified against:
//   - VuencLink  apps/vuenclink/src-tauri/src/world/snapz.rs :: decode_frame
//   - mock_server apps/vuenclink/src-tauri/examples/mock_server.rs :: encode_snapz
// both of which use flate2's DeflateEncoder at Compression::new(6) == zlib level 6,
// headerless. Hence deflateInit2(&s, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY).
//
// Header-only, C++17, links against -lz.

#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <zlib.h>

namespace ewb {

// --- raw DEFLATE (headerless stream, level 6) --------------------------------

inline std::vector<uint8_t> raw_deflate(const uint8_t* data, size_t len) {
    z_stream s{};
    // windowBits = -15 -> raw deflate, no header, no trailing Adler-32.
    if (deflateInit2(&s, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        throw std::runtime_error("deflateInit2 failed");

    std::vector<uint8_t> out(deflateBound(&s, static_cast<uLong>(len)));
    s.next_in   = const_cast<Bytef*>(data);
    s.avail_in  = static_cast<uInt>(len);
    s.next_out  = out.data();
    s.avail_out = static_cast<uInt>(out.size());

    const int rc = deflate(&s, Z_FINISH);
    if (rc != Z_STREAM_END) {
        deflateEnd(&s);
        throw std::runtime_error("deflate did not reach Z_STREAM_END");
    }
    out.resize(out.size() - s.avail_out);
    deflateEnd(&s);
    return out;
}

inline std::vector<uint8_t> raw_deflate(const std::vector<uint8_t>& in) {
    return raw_deflate(in.data(), in.size());
}

// --- raw INFLATE (for the round-trip test / debugging) ----------------------

inline std::vector<uint8_t> raw_inflate(const uint8_t* data, size_t len, size_t size_hint = 0) {
    z_stream s{};
    if (inflateInit2(&s, -15) != Z_OK)
        throw std::runtime_error("inflateInit2 failed");

    std::vector<uint8_t> out(size_hint ? size_hint : (len * 4 + 64));
    s.next_in  = const_cast<Bytef*>(data);
    s.avail_in = static_cast<uInt>(len);

    for (;;) {
        s.next_out  = out.data() + s.total_out;
        s.avail_out = static_cast<uInt>(out.size() - s.total_out);
        const int rc = inflate(&s, Z_FINISH);
        if (rc == Z_STREAM_END) break;
        if (rc == Z_OK || rc == Z_BUF_ERROR) {  // need more output room
            out.resize(out.size() * 2);
            continue;
        }
        inflateEnd(&s);
        throw std::runtime_error("inflate failed");
    }
    out.resize(s.total_out);
    inflateEnd(&s);
    return out;
}

// --- standard base64, no padding on encode ----------------------------------

inline std::string b64_encode(const uint8_t* data, size_t len) {
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);

    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        const uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
        out.push_back(T[(n >> 18) & 63]);
        out.push_back(T[(n >> 12) & 63]);
        out.push_back(T[(n >> 6) & 63]);
        out.push_back(T[n & 63]);
    }
    const size_t rem = len - i;
    if (rem == 1) {
        const uint32_t n = uint32_t(data[i]) << 16;
        out.push_back(T[(n >> 18) & 63]);
        out.push_back(T[(n >> 12) & 63]);
        // no '=' — the real server omits padding
    } else if (rem == 2) {
        const uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
        out.push_back(T[(n >> 18) & 63]);
        out.push_back(T[(n >> 12) & 63]);
        out.push_back(T[(n >> 6) & 63]);
    }
    return out;
}

inline std::string b64_encode(const std::vector<uint8_t>& in) {
    return b64_encode(in.data(), in.size());
}

// Tolerant decoder: accepts optional '=' padding and embedded whitespace.
inline std::vector<uint8_t> b64_decode(const std::string& in) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<uint8_t> out;
    out.reserve(in.size() / 4 * 3 + 3);
    uint32_t buf = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        const int v = val(c);
        if (v < 0) throw std::runtime_error("bad base64 character");
        buf = (buf << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(uint8_t((buf >> bits) & 0xFF));
        }
    }
    return out;
}

// --- SNAPZ frame ------------------------------------------------------------

struct SnapRec {
    int32_t x, y, z, flag, type;
};

inline void put_le_i32(std::vector<uint8_t>& v, int32_t n) {
    const uint32_t u = static_cast<uint32_t>(n);
    v.push_back(uint8_t(u & 0xFF));
    v.push_back(uint8_t((u >> 8) & 0xFF));
    v.push_back(uint8_t((u >> 16) & 0xFF));
    v.push_back(uint8_t((u >> 24) & 0xFF));
}

inline int32_t get_le_i32(const uint8_t* p) {
    return int32_t(uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
                   (uint32_t(p[3]) << 24));
}

// Returns the full wire line, terminating '\n' included.
//
// The (pointer, count) form lets a large burst be framed in place — stage 1.1
// splits at SNAPZ_FRAME_RECORDS without copying each slice into its own vector.
inline std::string encode_snapz(const SnapRec* recs, size_t n) {
    std::vector<uint8_t> raw;
    raw.reserve(n * 20);
    for (size_t i = 0; i < n; ++i) {
        const SnapRec& r = recs[i];
        put_le_i32(raw, r.x);
        put_le_i32(raw, r.y);
        put_le_i32(raw, r.z);
        put_le_i32(raw, r.flag);
        put_le_i32(raw, r.type);
    }
    const std::vector<uint8_t> z = raw_deflate(raw);
    return "SNAPZ:" + std::to_string(n) + ":" + b64_encode(z) + "\n";
}

inline std::string encode_snapz(const std::vector<SnapRec>& recs) {
    return encode_snapz(recs.data(), recs.size());
}

}  // namespace ewb
