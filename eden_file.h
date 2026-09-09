// eden_file.h — clean-room parser for the `.eden` world file format.
//
// ROADMAP-SERVER stage 5.0 (Phase 5 — `.eden` world import). Pure and
// header-only: a byte buffer in, structs out. No conversion, no I/O policy, no
// axis rename — that is the converter's job (`eden_import.cpp`, stage 5.1).
// Exercised offline by `eden_file_test.cpp`, wired into `build_server.sh`.
//
// Format provenance is public and may be documented here: Robert Munafo's
// original reverse-engineering (`MROB.txt`) and the C# `EdenWorldManipulator`
// reference implementation. Reimplemented from those references only.
//
// Layout, in file order:
//
//   [0, 192)             header — see EdenHeader
//   chunk block data     dense (type, paint) voxel grids, one per saved chunk
//   [reserved gap]       0..400 slots x 60 B EntityData ("creature block"), or none
//   [directory_offset]   16-byte {i32 cx, i32 cy, u64 off} rows to EOF
//   [trailing rows]      an appended `SGN1` sign section, every row tagged
//                        cx = 0xffffffff so the game's directory reader skips it
//
// The file may be wrapped in a ZIP container (PK magic, DEFLATE) — detect by
// magic, not extension, and inflate before parsing.
//
// Header-only, C++17, links against -lz (ZIP inflate).

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <zlib.h>

namespace ewb {

// ── structs ─────────────────────────────────────────────────────────────────

struct EdenHeader {
    int32_t  seed = 0;
    float    pos[3]  = {0, 0, 0};   // player position   (x_plane, height, y_plane)
    float    home[3] = {0, 0, 0};   // "home" / set point (x_plane, height, y_plane)
    float    yaw = 0;
    uint64_t dir_offset = 0;        // file offset of the chunk directory
    std::string name;               // ASCII, NUL-padded, <= 50 bytes
    int32_t  version = 0;           // a hint, not authority — see eden_detect_chunk_size
    uint8_t  skycolors[16] = {0};   // 16-band sky palette (informational; no wire path)
};

// One directory row, coordinate/offset-gated and with a *derived* byte span
// (`[off, end)`), which is `chunk_size` for a well-formed world but shorter where
// the file's own directory says a chunk is cut off early by its successor.
struct EdenChunk {
    int32_t  cx = 0, cy = 0;
    uint64_t off = 0;
    uint64_t end = 0;
    uint64_t span() const { return end - off; }
};

// One sign record. `a`/`b`/`c` are unknown and passed through verbatim, exactly
// as `sign_store.h` insists for `eden_signs.txt`. Coordinates are absolute, in
// the file's own axis order (x_plane, y_plane, z_height) — no rename here.
struct EdenSign {
    int32_t x = 0, y = 0, z = 0, a = 0, b = 0, c = 0;
    std::string text;
};

struct EdenWorld {
    EdenHeader             hdr;
    size_t                 chunk_size = 0;   // 32768 (64z) or 131072 (256z)
    int                    bands = 0;        // chunk_size / 8192 → 4 or 16
    int                    z_ceiling = 0;    // bands*16 - 1 → 63 or 255
    std::vector<EdenChunk>  chunks;
    std::vector<uint8_t>   dir_trailer;      // appended sign section, verbatim
    std::vector<EdenSign>  signs;            // decoded from dir_trailer (inline form)
    std::vector<uint8_t>   bytes;            // the decompressed file, owned

    const uint8_t* data() const { return bytes.data(); }
    size_t         size() const { return bytes.size(); }
};

// ── limits ──────────────────────────────────────────────────────────────────

// A ZIP-wrapped world is a decompression-bomb shape (a real specimen is a 167x
// archive). Refuse to inflate past this ceiling; the caller may raise it.
inline constexpr size_t EDEN_MAX_UNZIP     = 512ull * 1024 * 1024;
inline constexpr size_t EDEN_HEADER_BYTES  = 192;
inline constexpr size_t EDEN_DIR_ENTRY     = 16;
inline constexpr int32_t EDEN_CHUNK_COORD_LIMIT = 1 << 15;   // twoToOne gate: 0..32767
inline constexpr size_t EDEN_MAX_TRAILER_BYTES = 64 * 1024;  // multiple of 16
inline constexpr size_t EDEN_MAX_DIR_ENTRIES  = 4'000'000;

// ── little-endian scalar reads ──────────────────────────────────────────────

inline int32_t  rd_i32(const uint8_t* p) {
    return int32_t(uint32_t(p[0]) | uint32_t(p[1]) << 8 |
                   uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24);
}
inline uint32_t rd_u32(const uint8_t* p) { return uint32_t(rd_i32(p)); }
inline uint16_t rd_u16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
inline uint64_t rd_u64(const uint8_t* p) {
    return uint64_t(rd_u32(p)) | (uint64_t(rd_u32(p + 4)) << 32);
}
inline float    rd_f32(const uint8_t* p) {
    float f;
    uint32_t u = rd_u32(p);
    std::memcpy(&f, &u, 4);
    return f;
}

// ── ZIP wrapper ─────────────────────────────────────────────────────────────

inline bool eden_is_zip(const uint8_t* b, size_t n) {
    return n >= 4 && b[0] == 'P' && b[1] == 'K' &&
           (b[2] == 0x03 || b[2] == 0x05 || b[2] == 0x07);
}

// Bounded raw-DEFLATE. Refuses to grow the output past `max_out`.
inline std::vector<uint8_t> raw_inflate_bounded(const uint8_t* data, size_t len,
                                                size_t size_hint, size_t max_out) {
    z_stream s{};
    if (inflateInit2(&s, -15) != Z_OK)
        throw std::runtime_error("eden_file: inflateInit2 failed");

    size_t cap = std::min(max_out, std::max<size_t>(size_hint ? size_hint : len * 4 + 64, 4096));
    std::vector<uint8_t> out(cap);
    s.next_in  = const_cast<Bytef*>(data);
    s.avail_in = uInt(len);

    for (;;) {
        s.next_out  = out.data() + s.total_out;
        s.avail_out = uInt(out.size() - s.total_out);
        const int rc = inflate(&s, Z_FINISH);
        if (rc == Z_STREAM_END) break;
        if (rc == Z_OK || rc == Z_BUF_ERROR) {
            if (out.size() >= max_out) {
                inflateEnd(&s);
                throw std::runtime_error("eden_file: decompressed size exceeds the cap "
                                         "(decompression bomb?)");
            }
            out.resize(std::min(max_out, out.size() * 2));
            continue;
        }
        inflateEnd(&s);
        throw std::runtime_error("eden_file: inflate failed (corrupt ZIP member)");
    }
    out.resize(s.total_out);
    inflateEnd(&s);
    return out;
}

// Select the `.eden` member (not under `__MACOSX/`) from a ZIP archive and
// inflate it, bounded by `max_out`. "Inflate at the first PK" is wrong: a real
// specimen carries an `__MACOSX/._x.eden` AppleDouble decoy.
inline std::vector<uint8_t> eden_unzip(const uint8_t* b, size_t n,
                                       size_t max_out = EDEN_MAX_UNZIP) {
    // End of central directory: scan back for PK\x05\x06.
    if (n < 22) throw std::runtime_error("eden_file: truncated ZIP");
    size_t eocd = SIZE_MAX;
    for (size_t i = n - 22 + 1; i-- > 0;) {
        if (b[i] == 'P' && b[i + 1] == 'K' && b[i + 2] == 0x05 && b[i + 3] == 0x06) {
            eocd = i;
            break;
        }
        if (n - i > 22 + 65535) break;   // comment field caps at 64 KiB
    }
    if (eocd == SIZE_MAX) throw std::runtime_error("eden_file: no ZIP end-of-central-directory");

    uint32_t cd_count = rd_u16(b + eocd + 10);
    uint32_t cd_off   = rd_u32(b + eocd + 16);

    size_t   best_local = SIZE_MAX;
    uint32_t best_csize = 0, best_usize = 0;
    uint16_t best_method = 0;

    size_t p = cd_off;
    for (uint32_t i = 0; i < cd_count; ++i) {
        if (p + 46 > n || rd_u32(b + p) != 0x02014b50) break;
        uint16_t method  = rd_u16(b + p + 10);
        uint32_t csize   = rd_u32(b + p + 20);
        uint32_t usize   = rd_u32(b + p + 24);
        uint16_t namelen = rd_u16(b + p + 28);
        uint16_t extlen  = rd_u16(b + p + 30);
        uint16_t cmtlen  = rd_u16(b + p + 32);
        uint32_t local   = rd_u32(b + p + 42);
        if (p + 46 + namelen > n) break;
        std::string name(reinterpret_cast<const char*>(b + p + 46), namelen);
        p += 46 + namelen + extlen + cmtlen;

        bool is_macosx = name.rfind("__MACOSX/", 0) == 0;
        bool is_eden   = name.size() >= 5 && name.compare(name.size() - 5, 5, ".eden") == 0;
        if (is_eden && !is_macosx) {
            best_local  = local;
            best_csize  = csize;
            best_usize  = usize;
            best_method = method;
            break;
        }
    }
    if (best_local == SIZE_MAX)
        throw std::runtime_error("eden_file: ZIP has no .eden member");
    if (best_usize > max_out)
        throw std::runtime_error("eden_file: ZIP member's declared size exceeds the cap "
                                 "(decompression bomb?)");

    // Local file header: data begins after the variable name/extra fields.
    if (best_local + 30 > n || rd_u32(b + best_local) != 0x04034b50)
        throw std::runtime_error("eden_file: bad ZIP local header");
    uint16_t lnamelen = rd_u16(b + best_local + 26);
    uint16_t lextlen  = rd_u16(b + best_local + 28);
    size_t   dstart   = best_local + 30 + lnamelen + lextlen;
    if (dstart + best_csize > n)
        throw std::runtime_error("eden_file: ZIP member data runs past EOF");

    if (best_method == 0) {   // stored
        if (best_csize > max_out)
            throw std::runtime_error("eden_file: stored ZIP member exceeds the cap");
        return std::vector<uint8_t>(b + dstart, b + dstart + best_csize);
    }
    if (best_method != 8)
        throw std::runtime_error("eden_file: unsupported ZIP compression method");
    return raw_inflate_bounded(b + dstart, best_csize, best_usize, max_out);
}

// ── header ──────────────────────────────────────────────────────────────────

inline EdenHeader eden_parse_header(const uint8_t* b, size_t n) {
    if (n < EDEN_HEADER_BYTES)
        throw std::runtime_error("eden_file: file smaller than a 192-byte header");
    EdenHeader h;
    h.seed = rd_i32(b + 0);
    for (int i = 0; i < 3; ++i) h.pos[i]  = rd_f32(b + 4 + 4 * i);
    for (int i = 0; i < 3; ++i) h.home[i] = rd_f32(b + 16 + 4 * i);
    h.yaw        = rd_f32(b + 28);
    h.dir_offset = rd_u64(b + 32);
    {
        const char* nm = reinterpret_cast<const char*>(b + 40);
        size_t len = 0;
        while (len < 50 && nm[len] != '\0') ++len;
        h.name.assign(nm, len);
    }
    h.version = rd_i32(b + 92);
    std::memcpy(h.skycolors, b + 132, 16);
    return h;
}

// ── directory ───────────────────────────────────────────────────────────────

struct EdenDirEntry { int32_t cx, cy; uint64_t off; };

inline bool eden_is_chunk_coord(int32_t c) {
    return c >= 0 && c < EDEN_CHUNK_COORD_LIMIT;
}

// Every 16-byte row from `dir_offset` to EOF, decoded, no filtering.
inline std::vector<EdenDirEntry> eden_decode_directory(const uint8_t* b, size_t n,
                                                       uint64_t dir_offset) {
    if (dir_offset < EDEN_HEADER_BYTES || dir_offset >= n)
        throw std::runtime_error("eden_file: chunk directory offset out of range");
    std::vector<EdenDirEntry> out;
    for (size_t i = dir_offset; i + EDEN_DIR_ENTRY <= n && out.size() < EDEN_MAX_DIR_ENTRIES;
         i += EDEN_DIR_ENTRY) {
        out.push_back({rd_i32(b + i), rd_i32(b + i + 4), rd_u64(b + i + 8)});
    }
    return out;
}

// Version-independent chunk-size detector. The game reserves a 400-slot x 60-byte
// creature block directly before the directory whenever it has ever written one,
// so for the true chunk_size the gap
//   dir_offset - (max_chunk_offset + chunk_size)
// is either exactly 0 or a whole number of 60-byte slots, <= 400 of them. Try
// 256z first; take the size for which exactly one candidate is valid. This
// resolves a single-chunk world and the updated-game `version = 2` on 256z, both
// of which the min-gap fallback cannot. Returns 0 when ambiguous.
inline size_t eden_detect_chunk_size_creature_gap(const std::vector<EdenDirEntry>& gated,
                                                  uint64_t dir_offset) {
    if (gated.empty()) return 0;
    uint64_t max_off = 0;
    for (auto& e : gated) max_off = std::max(max_off, e.off);
    auto valid = [&](uint64_t cs) -> bool {
        if (max_off + cs > dir_offset) return false;
        uint64_t gap = dir_offset - (max_off + cs);
        return gap == 0 || (gap % 60 == 0 && gap / 60 <= 400);
    };
    bool a = valid(131072), b = valid(32768);
    if (a && !b) return 131072;
    if (b && !a) return 32768;
    return 0;
}

// Full detection: version >= 5 is authoritative 256z; otherwise the creature-gap
// detector, then the min-offset-gap fallback (no two 256z chunks are < 131072 B
// apart). `n` is the file size — offsets past EOF are ignored in the fallback.
inline size_t eden_detect_chunk_size(const std::vector<EdenDirEntry>& gated,
                                     uint64_t dir_offset, int32_t version, size_t n) {
    if (version >= 5) return 131072;
    if (size_t cs = eden_detect_chunk_size_creature_gap(gated, dir_offset)) return cs;

    std::vector<uint64_t> offs;
    for (auto& e : gated)
        if (e.off >= EDEN_HEADER_BYTES && e.off < n) offs.push_back(e.off);
    std::sort(offs.begin(), offs.end());
    offs.erase(std::unique(offs.begin(), offs.end()), offs.end());
    uint64_t min_gap = 32768;
    for (size_t i = 1; i < offs.size(); ++i)
        min_gap = std::min(min_gap, offs[i] - offs[i - 1]);
    return min_gap >= 131072 ? 131072 : 32768;
}

// ── signs ───────────────────────────────────────────────────────────────────

inline constexpr size_t EDEN_SIGN_RECORD = 120;
inline constexpr size_t EDEN_SIGN_HEADER = 12;   // "SGN1" | u32 version | u32 count
inline constexpr size_t EDEN_MAX_SIGNS   = 100'000;

// Parse a `signs_<worldfile>.eden.dat` sidecar, or an equivalent in-memory
// buffer beginning at the "SGN1" magic. Anything not matching the shape yields
// an empty vector — a foreign or corrupt sidecar must never be fatal.
inline std::vector<EdenSign> eden_parse_signs(const uint8_t* b, size_t n) {
    std::vector<EdenSign> out;
    if (n < EDEN_SIGN_HEADER || std::memcmp(b, "SGN1", 4) != 0) return out;
    size_t count = std::min<size_t>(rd_u32(b + 8), EDEN_MAX_SIGNS);
    size_t off = EDEN_SIGN_HEADER;
    for (size_t i = 0; i < count; ++i) {
        if (off + EDEN_SIGN_RECORD > n) break;   // drop a torn trailing record
        EdenSign s;
        s.x = rd_i32(b + off + 0);
        s.y = rd_i32(b + off + 4);
        s.z = rd_i32(b + off + 8);
        s.a = rd_i32(b + off + 12);
        s.b = rd_i32(b + off + 16);
        s.c = rd_i32(b + off + 20);
        const char* t = reinterpret_cast<const char*>(b + off + 24);
        size_t tlen = 0;
        while (tlen < EDEN_SIGN_RECORD - 24 && t[tlen] != '\0') ++tlen;
        s.text.assign(t, tlen);
        out.push_back(std::move(s));
        off += EDEN_SIGN_RECORD;
    }
    return out;
}
inline std::vector<EdenSign> eden_parse_signs(const std::vector<uint8_t>& v) {
    return eden_parse_signs(v.data(), v.size());
}

// Parse the inline (post-directory) trailer. Each 16-byte row is `ff ff ff ff` +
// 12 payload bytes; stripping the tag and concatenating the payloads rebuilds the
// bytes the game wrote. The first 12 reconstructed bytes are an outer wrapper
// ("SGN1" | u32 length | u32 0); skipping them exposes the same `SGN1` container
// the sidecar uses. Any untagged row means this is not (only) a sign trailer —
// bail to empty rather than guess.
inline std::vector<EdenSign> eden_parse_inline_signs(const uint8_t* b, size_t n) {
    if (n == 0 || n % 16 != 0) return {};
    std::vector<uint8_t> payload;
    payload.reserve(n / 16 * 12);
    for (size_t i = 0; i < n; i += 16) {
        if (!(b[i] == 0xff && b[i + 1] == 0xff && b[i + 2] == 0xff && b[i + 3] == 0xff))
            return {};
        payload.insert(payload.end(), b + i + 4, b + i + 16);
    }
    if (payload.size() < EDEN_SIGN_HEADER || std::memcmp(payload.data(), "SGN1", 4) != 0)
        return {};
    return eden_parse_signs(payload.data() + EDEN_SIGN_HEADER,
                            payload.size() - EDEN_SIGN_HEADER);
}
inline std::vector<EdenSign> eden_parse_inline_signs(const std::vector<uint8_t>& v) {
    return eden_parse_inline_signs(v.data(), v.size());
}

// ── voxel addressing ────────────────────────────────────────────────────────
//
// Within a chunk at base `addr`, a voxel at local (lx, ly) and world height z:
//   band = z / 16,  lz = z % 16
//   type  = addr + band*8192 + lx*256 + ly*16 + lz
//   paint = type  + 4096
// Each band is 8192 B: a 4096-B type region then a 4096-B paint region, each a
// 16x16x16 grid. Every read is bounded by the chunk's *derived* span — a short
// span (the 107,072-B overlap case is real) reads as air, never the neighbour's
// bytes.

inline size_t eden_voxel_type_offset(int lx, int ly, int z) {
    int band = z / 16, lz = z % 16;
    return size_t(band) * 8192 + size_t(lx) * 256 + size_t(ly) * 16 + lz;
}

inline uint8_t eden_block(const uint8_t* b, const EdenChunk& c, int lx, int ly, int z) {
    if (lx < 0 || lx > 15 || ly < 0 || ly > 15 || z < 0) return 0;
    size_t o = eden_voxel_type_offset(lx, ly, z);
    return o < c.span() ? b[c.off + o] : 0;
}

inline uint8_t eden_paint(const uint8_t* b, const EdenChunk& c, int lx, int ly, int z) {
    if (lx < 0 || lx > 15 || ly < 0 || ly > 15 || z < 0) return 0;
    size_t o = eden_voxel_type_offset(lx, ly, z) + 4096;
    return o < c.span() ? b[c.off + o] : 0;
}

// ── top-level parse ─────────────────────────────────────────────────────────

// Parse raw file bytes (ZIP-wrapped or not) into an EdenWorld. Throws
// std::runtime_error on a structurally unusable file. Sidecar signs are the
// caller's job (they live in a separate file); `world.signs` is the inline form.
inline EdenWorld eden_load(const uint8_t* raw, size_t raw_len,
                           size_t max_unzip = EDEN_MAX_UNZIP) {
    EdenWorld w;
    if (eden_is_zip(raw, raw_len)) w.bytes = eden_unzip(raw, raw_len, max_unzip);
    else                          w.bytes.assign(raw, raw + raw_len);

    const uint8_t* b = w.bytes.data();
    const size_t   n = w.bytes.size();

    w.hdr = eden_parse_header(b, n);

    // Pass A: every raw directory row.
    std::vector<EdenDirEntry> rows = eden_decode_directory(b, n, w.hdr.dir_offset);

    // Pass A½: peel a trailing sign section off the real chunk rows. The trailer
    // is everything after the last coordinate-gated row; interior rows that fail
    // the gate are corruption and are dropped, never folded into the trailer.
    size_t last_valid = 0;
    for (size_t i = rows.size(); i-- > 0;) {
        if (eden_is_chunk_coord(rows[i].cx) && eden_is_chunk_coord(rows[i].cy)) {
            last_valid = i + 1;
            break;
        }
    }
    {
        size_t tstart = std::min(n, size_t(w.hdr.dir_offset) + last_valid * 16);
        size_t tend   = std::min(n, size_t(w.hdr.dir_offset) + rows.size() * 16);
        size_t tlen   = std::min(tend > tstart ? tend - tstart : 0, EDEN_MAX_TRAILER_BYTES);
        w.dir_trailer.assign(b + tstart, b + tstart + tlen);
    }
    rows.resize(last_valid);
    rows.erase(std::remove_if(rows.begin(), rows.end(), [](const EdenDirEntry& e) {
                   return !(eden_is_chunk_coord(e.cx) && eden_is_chunk_coord(e.cy));
               }),
               rows.end());

    // Chunk size, then bands.
    w.chunk_size = eden_detect_chunk_size(rows, w.hdr.dir_offset, w.hdr.version, n);
    w.bands      = int(w.chunk_size / 8192);
    w.z_ceiling  = w.bands * 16 - 1;

    // Pass B: keep only rows whose data provably fits.
    std::vector<EdenDirEntry> kept;
    for (auto& e : rows)
        if (e.off >= EDEN_HEADER_BYTES && e.off + w.chunk_size <= n)
            kept.push_back(e);
    if (kept.empty())
        throw std::runtime_error("eden_file: no addressable chunk in the directory");

    // Derived spans: a chunk runs until the next chunk offset, the directory, or
    // EOF — whichever is nearest.
    std::vector<uint64_t> starts;
    for (auto& e : kept) starts.push_back(e.off);
    std::sort(starts.begin(), starts.end());
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
    for (auto& e : kept) {
        auto it = std::upper_bound(starts.begin(), starts.end(), e.off);
        uint64_t next = it == starts.end() ? n : *it;
        uint64_t barrier = w.hdr.dir_offset > e.off ? std::min(next, w.hdr.dir_offset) : next;
        uint64_t span = std::min<uint64_t>(w.chunk_size, barrier - e.off);
        w.chunks.push_back({e.cx, e.cy, e.off, e.off + span});
    }

    w.signs = eden_parse_inline_signs(w.dir_trailer);
    return w;
}

inline EdenWorld eden_load(const std::vector<uint8_t>& raw, size_t max_unzip = EDEN_MAX_UNZIP) {
    return eden_load(raw.data(), raw.size(), max_unzip);
}

}  // namespace ewb
