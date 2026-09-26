// topmap.h — the pure, testable half of ROADMAP-SERVER stage 8.5: a bounded,
// top-down height map of the world, as one control verb.
//
//   topmap:<x0>:<z0>:<x1>:<z1>:<step>
//
// The box is sampled every `step` blocks on both axes, starting at its min corner
// (x0 + i*step, z0 + j*step, while <= the max corner) — point sampling, not an
// aggregate over the step×step square, so the cost is one column read per sample
// whatever `step` is. At most TOPMAP_MAX_SIDE samples per axis.
//
// For each sampled column the answer is the **surface a player sees**: the highest
// y whose effective cell is solid, where "effective" is the stored delta if there
// is one and the base terrain profile (base_profile.h) otherwise. A mined-out
// grass block therefore reads as the dirt under it, not as "nothing stored".
//
// Reply (the control socket's `ok:` convention; one header line, then one line per
// z row, north to south, each holding W space-separated tokens west to east):
//
//   ok: topmap <x0> <z0> <x1> <z1> <step> <W> <H>
//   <token> <token> ...          (H lines)
//
//   token  -            column untouched: no stored cell at all — the base surface
//                       (grass at y 32 on the default profile). The common case,
//                       so it is one byte.
//          y,type,color the surface cell. `type` is the block type the client
//                       draws — a painted base block is reported as its base type
//                       with `color` set, never as the 255 sentinel. `color` 0 =
//                       unpainted.
//          .            touched, and nothing solid left in the column at all.
//
// `x1`/`z1` in the header are the last *sampled* coordinates, which may be less
// than the requested corner when the box is not a multiple of `step`.
//
// Pure and unit-tested by topmap_test.cpp. The server owns the locking: it reads
// one chunk column's samples per g_worldMtx hold (the stage 7.6 discipline), so a
// full-size topmap never holds the world lock for more than a few microseconds at
// a time.

#pragma once

#include <bitset>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "base_profile.h"

namespace ewb {

constexpr int TOPMAP_MAX_SIDE = 256;      // samples per axis
constexpr int TOPMAP_MAX_STEP = 0xFFFFFF; // one sample per world width; keeps the arithmetic in int
constexpr int TOPMAP_HEIGHT   = 256;      // world height (SV_WORLD_HEIGHT)
constexpr int64_t TOPMAP_XZ_MAX = 0xFFFFFF;

struct TopmapReq {
    int x0 = 0, z0 = 0;   // min corner (normalised)
    int step = 1;
    int w = 0, h = 0;     // samples per axis
    int lastX() const { return x0 + (w - 1) * step; }
    int lastZ() const { return z0 + (h - 1) * step; }
};

/// Parse the verb's five fields (`x0 z0 x1 z1 step`). Either corner order is
/// accepted. False with `err` set on a non-number, a coordinate outside the
/// world, a step outside 1..TOPMAP_MAX_STEP, or more than TOPMAP_MAX_SIDE samples
/// on either axis (the error names the smallest step that would fit).
inline bool topmap_parse(const std::vector<std::string>& f, TopmapReq& out, std::string& err) {
    if (f.size() != 5) { err = "expected x0 z0 x1 z1 step"; return false; }
    int64_t v[5];
    for (int i = 0; i < 5; ++i) {
        const std::string& s = f[i];
        char* end = nullptr;
        errno = 0;
        const long long n = std::strtoll(s.c_str(), &end, 10);
        if (s.empty() || *end != '\0' || errno == ERANGE) { err = "non-numeric field '" + s + "'"; return false; }
        v[i] = n;
    }
    for (int i = 0; i < 4; ++i)
        if (v[i] < 0 || v[i] > TOPMAP_XZ_MAX) { err = "coordinate out of range (0..16777215)"; return false; }
    if (v[4] < 1 || v[4] > TOPMAP_MAX_STEP) {
        err = "step must be 1.." + std::to_string(TOPMAP_MAX_STEP);
        return false;
    }
    const int64_t x0 = v[0] < v[2] ? v[0] : v[2], x1 = v[0] < v[2] ? v[2] : v[0];
    const int64_t z0 = v[1] < v[3] ? v[1] : v[3], z1 = v[1] < v[3] ? v[3] : v[1];
    const int64_t step = v[4];
    const int64_t w = (x1 - x0) / step + 1, h = (z1 - z0) / step + 1;
    if (w > TOPMAP_MAX_SIDE || h > TOPMAP_MAX_SIDE) {
        const int64_t span = (x1 - x0 > z1 - z0) ? x1 - x0 : z1 - z0;
        // floor(span / step) + 1 <= MAX_SIDE  <=>  step > span / MAX_SIDE.
        const int64_t need = span / TOPMAP_MAX_SIDE + 1;
        err = "too many samples (" + std::to_string(w) + "x" + std::to_string(h) + ", max " +
              std::to_string(TOPMAP_MAX_SIDE) + " per side); use step >= " + std::to_string(need);
        return false;
    }
    out.x0 = (int)x0;
    out.z0 = (int)z0;
    out.step = (int)step;
    out.w = (int)w;
    out.h = (int)h;
    return true;
}

/// Accumulates the stored cells of each sampled column (fed in any order) and
/// renders the reply against `base`, the profile the client draws. `feed` is
/// called with the sample's grid index, not its world coordinate.
class TopmapGrid {
  public:
    TopmapGrid(const TopmapReq& r, const BaseProfile& base)
        : req_(r), base_(base), cols_(size_t(r.w) * size_t(r.h)) {}

    const TopmapReq& req() const { return req_; }

    /// One stored cell of sample (i, j): `type` is logical (0 = mined air,
    /// 255 = painted base).
    void feed(int i, int j, int y, unsigned char type, unsigned char color) {
        if (i < 0 || j < 0 || i >= req_.w || j >= req_.h || y < 0 || y >= TOPMAP_HEIGHT) return;
        Col& c = cols_[size_t(j) * size_t(req_.w) + size_t(i)];
        c.touched = true;
        c.stored.set(size_t(y));
        if (type == 0) return;                       // mined: stored, not solid
        if (type == 255) {                           // painted base: the base block, painted
            type = base_.at(y).type;
            if (type == 0) return;                   // over base air it draws nothing
        }
        if (y > c.topY) { c.topY = y; c.type = type; c.color = color; }
    }

    /// The token for one sample.
    std::string token(int i, int j) const {
        const BaseProfile& base = base_;
        const Col& c = cols_[size_t(j) * size_t(req_.w) + size_t(i)];
        if (!c.touched) return "-";
        // Highest base-solid cell that no stored cell overrides.
        int baseY = -1;
        for (int y = (int)base.layers.size() - 1; y >= 0; --y) {
            if (y >= TOPMAP_HEIGHT) continue;
            if (base.at(y).type != 0 && !c.stored.test(size_t(y))) { baseY = y; break; }
        }
        int y = -1;
        unsigned type = 0, color = 0;
        if (c.topY >= 0) { y = c.topY; type = c.type; color = c.color; }
        if (baseY > y) {
            y = baseY; type = base.at(baseY).type; color = base.at(baseY).paint;
        }
        if (y < 0) return ".";
        return std::to_string(y) + "," + std::to_string(type) + "," + std::to_string(color);
    }

    /// The whole reply, header included, '\n'-terminated.
    std::string render() const {
        std::string out = "ok: topmap " + std::to_string(req_.x0) + " " + std::to_string(req_.z0) + " " +
                          std::to_string(req_.lastX()) + " " + std::to_string(req_.lastZ()) + " " +
                          std::to_string(req_.step) + " " + std::to_string(req_.w) + " " +
                          std::to_string(req_.h) + "\n";
        out.reserve(out.size() + size_t(req_.w) * size_t(req_.h) * 2);
        for (int j = 0; j < req_.h; ++j) {
            for (int i = 0; i < req_.w; ++i) {
                if (i) out.push_back(' ');
                out += token(i, j);
            }
            out.push_back('\n');
        }
        return out;
    }

  private:
    struct Col {
        std::bitset<TOPMAP_HEIGHT> stored;   // y has a stored delta (any type)
        int topY = -1;                        // highest stored non-air y
        unsigned char type = 0, color = 0;
        bool touched = false;
    };
    TopmapReq req_;
    BaseProfile base_;
    std::vector<Col> cols_;
};

/// Walk the request chunk column by chunk column: `fn(cx, cz, samples)` gets
/// every sample (i, j, x, z) whose column lies in chunk column (cx, cz), so a
/// caller can take its lock once per chunk column. Chunk columns with no sample
/// are never visited.
struct TopmapSample { int i, j, x, z; };

template <class F>
inline void topmap_for_each_chunk_column(const TopmapReq& r, F&& fn) {
    std::vector<TopmapSample> batch;
    // Sample x/z coordinates grouped by chunk index, in ascending order.
    auto groups = [](int origin, int step, int n) {
        std::vector<std::pair<int, std::vector<int>>> g;   // chunk -> sample indices
        for (int k = 0; k < n; ++k) {
            const int c = (origin + k * step) >> 4;
            if (g.empty() || g.back().first != c) g.push_back({c, {}});
            g.back().second.push_back(k);
        }
        return g;
    };
    const auto gx = groups(r.x0, r.step, r.w);
    const auto gz = groups(r.z0, r.step, r.h);
    for (const auto& zx : gz) {
        for (const auto& xx : gx) {
            batch.clear();
            for (int j : zx.second)
                for (int i : xx.second)
                    batch.push_back({i, j, r.x0 + i * r.step, r.z0 + j * r.step});
            fn(xx.first, zx.first, batch);
        }
    }
}

}  // namespace ewb
