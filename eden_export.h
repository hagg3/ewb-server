// eden_export.h — the server-world → `.eden` conversion core: a *writer* for
// the format `eden_file.h` reads.
//
// ROADMAP-SERVER stage 5.5 (Phase 5). Pure and header-only: a `WorldStore`
// (`world_store.h`), a sign list (`sign_store.h`), a `Spawn` (`spawn_store.h`)
// and an `ExportOptions` in, file bytes out. No filesystem, no argument parsing,
// no terminal output — that is `eden_export.cpp`'s job. Offline suite:
// `eden_export_test.cpp`, wired into `build_server.sh`.
//
// This is the inverse of `eden_import.h`, and it is deliberately written as its
// mirror image:
//
//   * **One base profile.** Export fills every emitted chunk with the *same*
//     `BaseProfile` import diffs against (`eden_import.h`, and `--base-profile`
//     on both tools). A different profile on either side and the round trip is
//     not identity — it is a re-terraform.
//
//   * **The axis rename, backwards, in one place.** Server (x, y = height, z)
//     → file (x, y = plane, z = height), for blocks (`eden_cell_offset`) and for
//     signs (`eden_sign_to_file`). Those two functions mirror `eden_scan_chunk`
//     and `eden_sign_to_server`; nothing else in this header touches an axis.
//
//   * **Drop and count, never wrap.** A cell above the chosen height ceiling or
//     in a chunk the game's directory cannot address is dropped and counted, so
//     the CLI can name it. Silently folding it back into range would write a
//     world the game misreads.
//
// The correctness bar is a round trip, not a parse — see `eden_export_test.cpp`
// and `docs/export.md` § The round-trip property.
//
// Header-only, C++17. Links -lz only for the optional ZIP wrapper (`--zip`).

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <zlib.h>        // crc32, for the ZIP wrapper

#include "eden_file.h"
#include "eden_import.h"   // BaseProfile, eden_default_profile, EDEN_* ids
#include "sign_store.h"
#include "snapz_codec.h"   // raw_deflate, for the ZIP wrapper
#include "spawn_store.h"
#include "world_store.h"

namespace ewb {

// ── little-endian scalar writes (the mirror of eden_file.h's rd_*) ──────────

inline void wr_u32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}
inline void wr_i32(std::vector<uint8_t>& v, int32_t x) { wr_u32(v, uint32_t(x)); }
inline void wr_u16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(uint8_t(x));
    v.push_back(uint8_t(x >> 8));
}
inline void wr_u64(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(uint8_t(x >> (8 * i)));
}
inline void wr_f32(std::vector<uint8_t>& v, float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    wr_u32(v, u);
}

// ── options ─────────────────────────────────────────────────────────────────

/// Where the signs go. The game reads a `signs_<worldfile>.dat` sidecar in
/// preference to the world's own inline trailer, and so does `eden_import`
/// (`--signs`, with that exact default name), so the sidecar is the default:
/// it is what closes the round trip with no extra argument.
enum class SignMode {
    Sidecar,   ///< `signs_<file>.dat` next to the `.eden` — default
    Inline,    ///< the appended `SGN1` trailer inside the `.eden` itself
    None,      ///< write no signs at all
};

inline const char* eden_sign_mode_name(SignMode m) {
    return m == SignMode::Sidecar ? "sidecar" : m == SignMode::Inline ? "inline" : "none";
}
inline bool eden_parse_sign_mode(const std::string& s, SignMode& out) {
    if (s == "sidecar") { out = SignMode::Sidecar; return true; }
    if (s == "inline")  { out = SignMode::Inline;  return true; }
    if (s == "none")    { out = SignMode::None;    return true; }
    return false;
}

/// Where a world with no cells at all is anchored, and the fallback the header
/// carries when no `eden_spawn.txt` was found: the plane's centre (65536) at the
/// ground standing height (`docs/protocol.md` § Coordinate model).
inline constexpr float EDEN_PLANE_CENTRE   = 65536.0f;
inline constexpr float EDEN_STAND_HEIGHT   = 33.92f;

/// Ceiling on the file this tool is willing to produce, matching the cap
/// `eden_file.h` will inflate a ZIP-wrapped world back up to (`EDEN_MAX_UNZIP`).
/// A world past it is refused rather than written — a `.eden` that the parser,
/// and therefore the editor, would reject is worse than no file.
inline constexpr size_t EDEN_EXPORT_MAX_BYTES = EDEN_MAX_UNZIP;

struct ExportOptions {
    std::string name;                       ///< world name, <= 50 bytes in the header
    BaseProfile profile = eden_default_profile();

    Spawn       spawn{EDEN_PLANE_CENTRE, EDEN_STAND_HEIGHT, EDEN_PLANE_CENTRE};
    bool        have_spawn = false;         ///< false: `spawn` is the default above

    // Header fields the server does not store. Defaults, unless
    // `eden_apply_origin` has folded in the source world's own values from an
    // `eden_origin.txt` sidecar (stage 5.6).
    int32_t seed = 0;
    float   yaw  = 0.0f;
    uint8_t skycolors[16] = {0};
    bool    have_home = false;              ///< false: `home` mirrors `spawn`
    float   home[3] = {0, 0, 0};
    int32_t version = 0;                    ///< 0 = derive from the z format
    int     z_hint = 0;                     ///< 64 / 256: the source's format, a tie-breaker

    SignMode signs = SignMode::Sidecar;
    int      force_z = 0;                   ///< 0 = auto, or 64 / 256
    bool     zip = false;                   ///< wrap the file in a ZIP container
    size_t   max_bytes = EDEN_EXPORT_MAX_BYTES;
};

/// Fold an `eden_origin.txt` into `opt`. Precedence, most specific first:
///   - `--seed` / `--yaw` on the command line (`seed_set` / `yaw_set`) beat the file;
///   - an `eden_spawn.txt` that has been *changed* since import beats `pos` — the
///     operator moved the spawn on purpose. One that still says what import wrote
///     (equal at its own two decimals) is the same spawn, so the origin's exact
///     `pos` is used and the header's float comes back bit-for-bit.
/// Returns true when the origin's `pos` supplied the spawn.
inline bool eden_apply_origin(const EdenOrigin& o, ExportOptions& opt,
                              bool seed_set, bool yaw_set) {
    if (o.seed && !seed_set) opt.seed = *o.seed;
    if (o.yaw && !yaw_set)   opt.yaw  = *o.yaw;
    if (o.sky) std::copy(o.sky->begin(), o.sky->end(), opt.skycolors);
    if (o.home) {
        opt.have_home = true;
        std::copy(o.home->begin(), o.home->end(), opt.home);
    }
    if (o.version) opt.version = *o.version;
    if (o.z)       opt.z_hint  = *o.z;

    bool pos_used = false;
    if (o.pos) {
        const Spawn exact{(*o.pos)[0], (*o.pos)[1], (*o.pos)[2]};
        if (!opt.have_spawn || format_spawn_line(opt.spawn) == format_spawn_line(exact)) {
            opt.spawn = exact;
            opt.have_spawn = true;
            pos_used = true;
        }
    }
    return pos_used;
}

// ── result ──────────────────────────────────────────────────────────────────

struct ExportResult {
    std::vector<uint8_t> bytes;          ///< the `.eden` file (ZIP-wrapped if asked)
    std::vector<uint8_t> sign_sidecar;   ///< the `signs_<file>.dat` body, if SignMode::Sidecar

    size_t chunks = 0;          ///< chunk blocks written
    size_t cells  = 0;          ///< store cells that landed in one
    size_t signs  = 0;          ///< signs written (sidecar or inline)

    size_t cells_dropped_height = 0;   ///< above the chosen z ceiling
    size_t cells_dropped_coord  = 0;   ///< chunk coordinate outside the directory gate
    size_t signs_dropped        = 0;   ///< not expressible in file coordinates
    size_t painted_base_on_air  = 0;   ///< type 255 where the profile has no block

    size_t chunk_size = 0;      ///< 32768 (64z) or 131072 (256z)
    int    bands      = 0;
    int    z_ceiling  = 0;      ///< 63 or 255
    bool   seeded     = false;  ///< the world was empty; one base chunk was written

    size_t uncompressed_bytes = 0;   ///< before the optional ZIP wrapper

    /// `64z` / `256z`, the way the summary and `docs/export.md` name it.
    std::string z_format() const { return std::to_string(z_ceiling + 1) + "z"; }
};

// ── the two sentinels, in one place ─────────────────────────────────────────

/// Map one **logical** store cell (`WorldStore::get`'s type/colour) to the
/// (type, paint) pair the file records at that height.
///
///   type 0    mined air. The server's `CELL_MINED` (254) never surfaces through
///             an accessor — it reads back as logical 0 — so this one branch
///             covers both. Paint goes with it: a voxel with no block has no
///             colour, and keeping one would make the round trip non-idempotent
///             (import would drop the paint, the next export would not write it).
///   type 255  `CELL_PAINTED_BASE`: the base terrain's own block at this height,
///             recoloured. Where the profile has no block there is nothing to
///             recolour, so it degrades to air and is counted.
///   otherwise verbatim.
inline void eden_cell_to_file(const BaseProfile& profile, int z,
                              uint8_t type, uint8_t color,
                              uint8_t& out_type, uint8_t& out_paint,
                              bool* painted_base_on_air = nullptr) {
    if (type == CELL_AIR) { out_type = 0; out_paint = 0; return; }
    if (type == CELL_PAINTED_BASE) {
        const BaseVoxel b = profile.at(z);
        if (b.type == 0) {
            if (painted_base_on_air) *painted_base_on_air = true;
            out_type = 0;
            out_paint = 0;
            return;
        }
        out_type = b.type;
        out_paint = color;
        return;
    }
    out_type = type;
    out_paint = color;
}

// ── the axis rename, backwards ──────────────────────────────────────────────

/// Byte offset of a server cell inside its chunk block, and the chunk it belongs
/// to. The one place blocks change axes: server (x, y = height, z) becomes file
/// (x, y = plane, z = height), so `cx = x >> 4`, `cy = z >> 4`, and the file's
/// own `z` is the server's `y`. The exact mirror of `eden_scan_chunk`, which
/// reads `fn(cx*16 + lx, z, cy*16 + ly)`.
inline void eden_cell_chunk(int x, int y, int z, int& cx, int& cy, int& fz,
                            int& lx, int& ly) {
    cx = x >> 4;
    cy = z >> 4;
    lx = x & 15;
    ly = z & 15;
    fz = y;
}

/// A server `Sign` in file coordinates, or false if it cannot be one. The mirror
/// of `eden_sign_to_server` — the same swap, run the other way — plus the height
/// gate the chosen z format imposes.
inline bool eden_sign_to_file(const Sign& s, int z_ceiling, EdenSign& out) {
    if (s.x < 0 || s.z < 0 || s.y < 0) return false;
    if (s.y > z_ceiling) return false;          // no room for it in this z format
    if (!eden_is_chunk_coord(s.x >> 4) || !eden_is_chunk_coord(s.z >> 4)) return false;
    out.x = s.x;          // file x  = server x
    out.y = s.z;          // file y  = server z   (plane)
    out.z = s.y;          // file z  = server y   (height)
    out.a = s.a; out.b = s.b; out.c = s.c;
    out.text = sanitize_text(s.text, SIGN_TEXT_MAX);
    return true;
}

// ── height format ───────────────────────────────────────────────────────────

/// 64z unless something sits above 63, then 256z. Signs count: a world whose
/// only tall thing is a sign still needs the taller format to keep it.
/// `force_z` (64 or 256) overrides, and anything above the resulting ceiling is
/// dropped by the writer and counted.
///
/// `z_hint` (from `eden_origin.txt`) only breaks the tie for a world with
/// nothing tall in it: a source that *was* 256z stays 256z after everything
/// above 63 has been mined away. It never lowers the ceiling.
inline int eden_choose_z_ceiling(const WorldStore& store, const std::vector<Sign>& signs,
                                 int force_z, int z_hint = 0) {
    if (force_z == 64)  return 63;
    if (force_z == 256) return 255;
    bool tall = z_hint == 256;
    store.for_each([&](int, int y, int, unsigned char, unsigned char) {
        if (y > 63) tall = true;
    });
    for (const Sign& s : signs)
        if (s.y > 63) tall = true;
    return tall ? 255 : 63;
}

inline bool eden_parse_z_format(const std::string& s, int& out) {
    if (s == "auto") { out = 0;   return true; }
    if (s == "64" || s == "64z")   { out = 64;  return true; }
    if (s == "256" || s == "256z") { out = 256; return true; }
    return false;
}

// ── signs: the SGN1 container, the sidecar and the inline trailer ───────────

/// The `SGN1` container both forms carry: magic, version, count, then 120-byte
/// records. Exactly what `eden_parse_signs` reads.
inline std::vector<uint8_t> eden_build_sgn1(const std::vector<EdenSign>& recs) {
    std::vector<uint8_t> b;
    b.reserve(EDEN_SIGN_HEADER + recs.size() * EDEN_SIGN_RECORD);
    b.insert(b.end(), {'S', 'G', 'N', '1'});
    wr_u32(b, 1);
    wr_u32(b, uint32_t(recs.size()));
    for (const EdenSign& r : recs) {
        wr_i32(b, r.x); wr_i32(b, r.y); wr_i32(b, r.z);
        wr_i32(b, r.a); wr_i32(b, r.b); wr_i32(b, r.c);
        std::string t = r.text;
        if (t.size() > EDEN_SIGN_RECORD - 24 - 1) t.resize(EDEN_SIGN_RECORD - 24 - 1);
        t.resize(EDEN_SIGN_RECORD - 24, '\0');   // NUL-padded, always NUL-terminated
        b.insert(b.end(), t.begin(), t.end());
    }
    return b;
}

/// The appended directory trailer: an outer `SGN1 | u32 length | u32 0` wrapper,
/// then the container, zero-padded to a multiple of 12 and split into 12-byte
/// payloads, each in a 16-byte row tagged `cx = 0xffffffff` so the game's
/// directory reader — and `eden_file.h`'s — skips it as a chunk row.
inline std::vector<uint8_t> eden_build_sign_trailer(const std::vector<EdenSign>& recs) {
    if (recs.empty()) return {};
    const std::vector<uint8_t> inner = eden_build_sgn1(recs);
    std::vector<uint8_t> payload;
    payload.insert(payload.end(), {'S', 'G', 'N', '1'});
    wr_u32(payload, uint32_t(inner.size()));
    wr_u32(payload, 0);
    payload.insert(payload.end(), inner.begin(), inner.end());
    while (payload.size() % 12 != 0) payload.push_back(0);

    std::vector<uint8_t> trailer;
    trailer.reserve(payload.size() / 12 * 16);
    for (size_t i = 0; i < payload.size(); i += 12) {
        trailer.insert(trailer.end(), {0xff, 0xff, 0xff, 0xff});
        trailer.insert(trailer.end(), payload.begin() + i, payload.begin() + i + 12);
    }
    return trailer;
}

/// Convert a server sign list to file records, dropping and counting whatever
/// cannot be expressed.
inline std::vector<EdenSign> eden_signs_to_file(const std::vector<Sign>& in, int z_ceiling,
                                                size_t& dropped) {
    std::vector<EdenSign> out;
    dropped = 0;
    out.reserve(in.size());
    for (const Sign& s : in) {
        if (out.size() >= EDEN_MAX_SIGNS) { ++dropped; continue; }
        EdenSign f;
        if (eden_sign_to_file(s, z_ceiling, f)) out.push_back(std::move(f));
        else ++dropped;
    }
    return out;
}

/// The sidecar path the game (and `eden_import --signs`' default) looks for:
/// `signs_<worldfile>.dat`, beside the world file.
inline std::string eden_sidecar_path(const std::string& eden_path) {
    const size_t slash = eden_path.find_last_of('/');
    const std::string dir  = (slash == std::string::npos) ? "" : eden_path.substr(0, slash + 1);
    const std::string base = (slash == std::string::npos) ? eden_path : eden_path.substr(slash + 1);
    return dir + "signs_" + base + ".dat";
}

// ── ZIP wrapper ─────────────────────────────────────────────────────────────

/// A single-member ZIP holding `name` — the container `eden_is_zip` /
/// `eden_unzip` detect and read back. Deflated; no directory entries, no
/// `__MACOSX/` decoy.
inline std::vector<uint8_t> eden_zip_single(const std::string& name,
                                            const std::vector<uint8_t>& data) {
    const uint32_t crc = uint32_t(crc32(0, data.data(), uInt(data.size())));
    const std::vector<uint8_t> comp = raw_deflate(data.data(), data.size());

    std::vector<uint8_t> out;
    const uint32_t local_off = 0;
    wr_u32(out, 0x04034b50);
    wr_u16(out, 20); wr_u16(out, 0); wr_u16(out, 8);   // version, flags, deflate
    wr_u16(out, 0);  wr_u16(out, 0);                    // mod time / date
    wr_u32(out, crc);
    wr_u32(out, uint32_t(comp.size()));
    wr_u32(out, uint32_t(data.size()));
    wr_u16(out, uint16_t(name.size())); wr_u16(out, 0);
    out.insert(out.end(), name.begin(), name.end());
    out.insert(out.end(), comp.begin(), comp.end());

    const uint32_t cd_off = uint32_t(out.size());
    wr_u32(out, 0x02014b50);
    wr_u16(out, 20); wr_u16(out, 20); wr_u16(out, 0);
    wr_u16(out, 8);  wr_u16(out, 0);  wr_u16(out, 0);
    wr_u32(out, crc);
    wr_u32(out, uint32_t(comp.size()));
    wr_u32(out, uint32_t(data.size()));
    wr_u16(out, uint16_t(name.size()));
    wr_u16(out, 0); wr_u16(out, 0); wr_u16(out, 0);
    wr_u16(out, 0); wr_u32(out, 0);
    wr_u32(out, local_off);
    out.insert(out.end(), name.begin(), name.end());
    const uint32_t cd_size = uint32_t(out.size()) - cd_off;

    wr_u32(out, 0x06054b50);
    wr_u16(out, 0); wr_u16(out, 0);
    wr_u16(out, 1); wr_u16(out, 1);
    wr_u32(out, cd_size);
    wr_u32(out, cd_off);
    wr_u16(out, 0);
    return out;
}

// ── the writer ──────────────────────────────────────────────────────────────

/// One chunk block's worth of pure base terrain, ready to `memcpy` per chunk.
/// Laid out exactly as `eden_voxel_type_offset` addresses it: per band, a
/// 4096-byte type grid then a 4096-byte paint grid.
inline std::vector<uint8_t> eden_base_chunk(const BaseProfile& profile, int bands) {
    std::vector<uint8_t> c(size_t(bands) * 8192, 0);
    for (int band = 0; band < bands; ++band)
        for (int lx = 0; lx < 16; ++lx)
            for (int ly = 0; ly < 16; ++ly)
                for (int lz = 0; lz < 16; ++lz) {
                    const BaseVoxel v = profile.at(band * 16 + lz);
                    const size_t o = size_t(band) * 8192 + size_t(lx) * 256 +
                                     size_t(ly) * 16 + size_t(lz);
                    c[o] = v.type;
                    c[o + 4096] = v.paint;
                }
    return c;
}

/// Write `store` (+ `signs`, + `opt.spawn`) as a `.eden`. Returns false with
/// `err` set and `out` untouched-in-spirit (the caller writes nothing) when the
/// world cannot be expressed — today that is only "the file would be larger than
/// `opt.max_bytes`". Everything else is a drop, counted in `out`.
///
/// Two passes over the store: one to choose the height format and the chunk set,
/// one to overlay cells into the allocated buffer. Nothing per-cell is buffered,
/// so a multi-million-cell world costs its own file size and no more.
inline bool eden_export_world(const WorldStore& store, const std::vector<Sign>& signs,
                              const ExportOptions& opt, ExportResult& out, std::string& err) {
    out = ExportResult{};
    err.clear();

    out.z_ceiling  = eden_choose_z_ceiling(store, signs, opt.force_z, opt.z_hint);
    out.bands      = (out.z_ceiling + 1) / 16;
    out.chunk_size = size_t(out.bands) * 8192;

    // ── pass 1: the chunk set, and the drops ────────────────────────────────
    std::map<std::pair<int32_t, int32_t>, size_t> index;   // (cx, cy) -> chunk slot
    store.for_each([&](int x, int y, int z, unsigned char, unsigned char) {
        int cx, cy, fz, lx, ly;
        eden_cell_chunk(x, y, z, cx, cy, fz, lx, ly);
        if (!eden_is_chunk_coord(cx) || !eden_is_chunk_coord(cy)) {
            ++out.cells_dropped_coord;
            return;
        }
        if (fz < 0 || fz > out.z_ceiling) {
            ++out.cells_dropped_height;
            return;
        }
        index.emplace(std::make_pair(int32_t(cx), int32_t(cy)), size_t(0));
    });

    // An empty world still has to be a *file*: `eden_load` refuses a directory
    // with no addressable chunk, so a world with nothing in it gets one chunk of
    // pure base terrain at the spawn. Re-importing it yields zero cells, which is
    // what it started as.
    if (index.empty()) {
        int32_t cx = int32_t(EDEN_PLANE_CENTRE) >> 4, cy = cx;
        if (opt.have_spawn) {
            const int32_t sx = int32_t(opt.spawn.x) >> 4, sz = int32_t(opt.spawn.z) >> 4;
            if (eden_is_chunk_coord(sx) && eden_is_chunk_coord(sz)) { cx = sx; cy = sz; }
        }
        index.emplace(std::make_pair(cx, cy), size_t(0));
        out.seeded = true;
    }

    {
        size_t slot = 0;
        for (auto& kv : index) kv.second = slot++;   // std::map: ascending (cx, cy)
    }
    out.chunks = index.size();

    // ── signs ───────────────────────────────────────────────────────────────
    std::vector<EdenSign> file_signs;
    if (opt.signs != SignMode::None)
        file_signs = eden_signs_to_file(signs, out.z_ceiling, out.signs_dropped);
    else
        out.signs_dropped = 0;
    out.signs = file_signs.size();

    std::vector<uint8_t> trailer;
    if (opt.signs == SignMode::Inline) trailer = eden_build_sign_trailer(file_signs);
    if (opt.signs == SignMode::Sidecar && !file_signs.empty())
        out.sign_sidecar = eden_build_sgn1(file_signs);

    // ── size gate, before a byte is allocated ───────────────────────────────
    const uint64_t body  = uint64_t(out.chunks) * uint64_t(out.chunk_size);
    const uint64_t total = EDEN_HEADER_BYTES + body +
                           uint64_t(out.chunks) * EDEN_DIR_ENTRY + trailer.size();
    if (total > uint64_t(opt.max_bytes)) {
        err = "the world needs " + std::to_string(total) + " bytes (" +
              std::to_string(out.chunks) + " chunks x " + std::to_string(out.chunk_size) +
              "), past the " + std::to_string(opt.max_bytes) +
              "-byte ceiling; raise it with --max-bytes if you mean it";
        return false;
    }

    // ── the buffer: header, chunk blocks, directory, trailer ────────────────
    const uint64_t dir_offset = EDEN_HEADER_BYTES + body;
    std::vector<uint8_t> f;
    f.reserve(size_t(total));

    wr_i32(f, opt.seed);
    const Spawn sp = opt.spawn;
    // The header's `pos` is already in server order (x, height, z) — the one
    // field that needs no rename. `home` has no server equivalent: it replays
    // the origin sidecar's value, else mirrors `pos` so an editor opening the
    // file has a sane set point.
    wr_f32(f, sp.x); wr_f32(f, sp.y); wr_f32(f, sp.z);
    if (opt.have_home) { wr_f32(f, opt.home[0]); wr_f32(f, opt.home[1]); wr_f32(f, opt.home[2]); }
    else               { wr_f32(f, sp.x); wr_f32(f, sp.y); wr_f32(f, sp.z); }
    wr_f32(f, opt.yaw);
    wr_u64(f, dir_offset);
    {
        std::string nm = opt.name;
        if (nm.size() > 49) nm.resize(49);     // always NUL-terminated inside 50
        nm.resize(50, '\0');
        f.insert(f.end(), nm.begin(), nm.end());
    }
    f.push_back(0); f.push_back(0);            // pad @90..92
    // `eden_detect_chunk_size` treats version >= 5 as authoritative 256z; 4 is
    // what every 64z specimen carries. Saying it in the header means the
    // detector never has to fall back to measuring gaps on our own files. A
    // recorded source version is replayed only while it still agrees with the
    // format actually written — an edit that grew or shrank the world past 63
    // must not leave a version that lies about it.
    {
        const bool tall = out.z_ceiling == 255;
        const bool keep = opt.version != 0 && (opt.version >= 5) == tall;
        wr_i32(f, keep ? opt.version : (tall ? 5 : 4));
    }
    f.resize(132, 0);                          // hash[36] @96..132 — unused
    f.insert(f.end(), opt.skycolors, opt.skycolors + 16);
    f.resize(EDEN_HEADER_BYTES, 0);

    // Fill: every emitted chunk starts as pure base terrain.
    {
        const std::vector<uint8_t> base = eden_base_chunk(opt.profile, out.bands);
        f.resize(size_t(dir_offset));
        for (size_t i = 0; i < out.chunks; ++i)
            std::memcpy(f.data() + EDEN_HEADER_BYTES + i * out.chunk_size,
                        base.data(), out.chunk_size);
    }

    // ── pass 2: overlay the store's cells ───────────────────────────────────
    store.for_each([&](int x, int y, int z, unsigned char type, unsigned char color) {
        int cx, cy, fz, lx, ly;
        eden_cell_chunk(x, y, z, cx, cy, fz, lx, ly);
        if (!eden_is_chunk_coord(cx) || !eden_is_chunk_coord(cy)) return;
        if (fz < 0 || fz > out.z_ceiling) return;
        const auto it = index.find({int32_t(cx), int32_t(cy)});
        if (it == index.end()) return;          // unreachable: pass 1 inserted it

        uint8_t ft = 0, fp = 0;
        bool on_air = false;
        eden_cell_to_file(opt.profile, fz, type, color, ft, fp, &on_air);
        if (on_air) ++out.painted_base_on_air;

        const size_t base = size_t(EDEN_HEADER_BYTES) + it->second * out.chunk_size;
        const size_t o = eden_voxel_type_offset(lx, ly, fz);
        f[base + o] = ft;
        f[base + o + 4096] = fp;
        ++out.cells;
    });

    // ── directory, then the sign trailer ────────────────────────────────────
    for (const auto& kv : index) {
        wr_i32(f, kv.first.first);
        wr_i32(f, kv.first.second);
        wr_u64(f, uint64_t(EDEN_HEADER_BYTES) + uint64_t(kv.second) * out.chunk_size);
    }
    f.insert(f.end(), trailer.begin(), trailer.end());

    out.uncompressed_bytes = f.size();
    if (opt.zip) {
        const std::string member = opt.name.empty() ? std::string("world.eden")
                                                    : eden_slug(opt.name) + ".eden";
        out.bytes = eden_zip_single(member, f);
    } else {
        out.bytes = std::move(f);
    }
    return true;
}

}  // namespace ewb
