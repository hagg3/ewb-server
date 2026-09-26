// explode.h — the TNT / paint explosion chain (ROADMAP-SERVER stages 7.7, 7.19).
//
// Mirrors Terrain::explode: a spherical radius centred on a cell; color != 0
// paints the sphere, color == 0 destroys it. A TNT or firework caught in the
// blast chains into its own explosion. Pulled out of server_posix.cpp into a
// pure header, world access supplied by the caller, so the worklist bound can
// be unit-tested offline (protocol_test.cpp) without g_world / g_worldMtx.
//
// 7.7: the original recursive simExplode was bounded only by a depth > 6 guard.
// Depth is fine for the stack, but the *fan-out* at each depth is not bounded:
// every TNT/firework inside a blast is chained into — and, as in the client,
// a TNT already consumed by a sibling's blast is exploded again when its turn
// comes — so a dense field gives thousands of explosions, all under one lock
// from one packet. 7.7 made it an explicit worklist with a cap on the *total*
// explosions processed. That cap is a termination bound, not a latency bound.
//
// 7.19: the bound is now a **work budget in cell visits** — every cell a blast
// reads — because visits are what hold the world lock (a blast is ~99% reads:
// a dense chain measured ~4.2 M reads against ~10 K writes). Each blast scans
// a fixed EXPLODE_VISITS_PER_BLAST cells, so the budget is enforced one whole
// blast at a time: a chain is truncated between blasts, never half-way through
// one. The caller sizes the budget (edenserver's --burn-max-cells) and charges
// the player for the blasts that actually ran (ExplodeResult::explosions).
//
// 8.2: protected zones. `protectedAt(x, y, z)` is asked before a cell is read. A
// protected cell is neither written nor **chained** — a TNT or firework inside a
// zone does not go off, so it cannot carry the blast further (skipping only the
// write would still detonate it and chain past the zone). The hook only answers;
// the caller collects the cells it said yes to, for the restore it sends clients
// (which simulate the blast themselves and did destroy them). A protected root
// runs no blast at all. EXPLODE_REACH bounds how far from its root a chain can
// touch, so a caller can drop every zone outside that box before the chain runs.

#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <vector>

namespace ewb {

/// Recursion-depth guard, kept from the original implementation as a second,
/// orthogonal bound.
constexpr int EXPLODE_MAX_DEPTH = 6;

constexpr int EXPLODE_RADIUS = 6;

/// How far (per axis, in cells) any cell a chain reads or writes can be from its
/// root. A link at depth d sits inside its parent's sphere, so within d * RADIUS of
/// the root; links deeper than EXPLODE_MAX_DEPTH never run, and the deepest blast
/// reaches RADIUS past its own centre.
constexpr int EXPLODE_REACH = EXPLODE_RADIUS * (EXPLODE_MAX_DEPTH + 1);

/// Cells one blast reads: its centre, plus every sample of the radius-6 sphere
/// (the y = 0 layer is sampled twice, as Terrain::explode does). A blast within
/// 6 of the world's top or bottom reads fewer; the budget charges the full
/// figure regardless, so it stays an upper bound.
constexpr size_t explode_visits_per_blast() {
    size_t n = 1;
    const int R = EXPLODE_RADIUS;
    for (int yy = 0; yy < R; ++yy)
        for (int ox = -R; ox <= R; ++ox)
            for (int oz = -R; oz <= R; ++oz)
                if (ox * ox + oz * oz + yy * yy <= R * R) n += 2;
    return n;
}
constexpr size_t EXPLODE_VISITS_PER_BLAST = explode_visits_per_blast();

/// Every cell one blast centred on (cx, cy, cz) samples — the centre, then the
/// sphere in simExplode's own order, clipped to [yMin, yMax). Cells on the y = cy
/// layer come round twice, as they do in the blast. For a caller that needs the
/// footprint without running it: the server restores the sphere of a protected
/// TNT that every client will still set off, because clients know nothing of zones.
template <class F>
inline void explode_for_each_blast_cell(int cx, int cy, int cz, int yMin, int yMax, F&& fn) {
    if (cy >= yMin && cy < yMax) fn(cx, cy, cz);
    const int R = EXPLODE_RADIUS;
    for (int i = 1; i <= R; i++) {
        const int yy = R - i;
        for (int j = cx - R; j <= cx + R; j++)
            for (int k = cz - R; k <= cz + R; k++) {
                const int ox = j - cx, oz = k - cz;
                if (ox * ox + oz * oz + yy * yy > R * R) continue;
                const int ys[2] = {cy - yy, cy + yy};
                for (int sy : ys)
                    if (sy >= yMin && sy < yMax) fn(j, sy, k);
            }
    }
}

/// Default cell-visit budget for one chain (edenserver --burn-max-cells). 2^20
/// visits is ~1 000 blasts — a solid 5x5x5 block of TNT, re-explosions
/// included, runs to completion — and measured ~10 ms of world lock on the
/// development machine (11–25 ns a visit). The 7.7 cap of 4 096 blasts is ~4x
/// that. Stage 7.19 in ROADMAP-SERVER has the numbers.
constexpr size_t EXPLODE_DEFAULT_MAX_VISITS = size_t(1) << 20;

struct ExplodeCell {
    unsigned char type;
    unsigned char color;
};

/// World access + block-id vocabulary the algorithm needs. `get` reports
/// whether a cell has a stored (non-default) value. `set` applies an edit and
/// returns false when the caller's cap refused a brand-new cell — mirrors
/// worldSet's contract so refusals are countable without the header knowing
/// what the cap is.
///
/// This std::function form is the convenient one for tests. simExplode is a
/// template over any type with the same members, so the server passes a struct
/// with plain member functions instead and pays no indirect call per visit.
struct ExplodeWorld {
    std::function<bool(int x, int y, int z, ExplodeCell& out)> get;
    std::function<bool(int x, int y, int z, int type, int color)> set;
    // True for a cell in a protected zone (8.2): not written, not chained.
    std::function<bool(int x, int y, int z)> protectedAt = [](int, int, int) { return false; };
    int airType = 0;
    int tntType = 9;
    int fireworkType = 65;
    int bedrockType = 1;
    int steelType = 74;
    int paintedBaseType = 255;
    int yMin = 0;
    // Exclusive. The world height (SV_WORLD_HEIGHT / ewb::WS_WORLD_HEIGHT). Was
    // 1024, which let a blast near the top write cells above the world that no
    // REGION reply reads (ROADMAP-SERVER 7.22); protocol_test ties it to the store.
    int yMax = 256;
};

struct ExplodeResult {
    size_t refused = 0;     ///< new cells the caller's world cap refused
    size_t truncated = 0;   ///< chain links dropped when the budget ran out
    size_t explosions = 0;  ///< blasts actually run (the root included)
    size_t visits = 0;      ///< cells actually read
    size_t protectedHits = 0;  ///< samples protectedAt refused (repeats included)
};

/// Runs the chain until it finishes or the next blast would exceed `maxVisits`.
/// The root blast always runs, whatever the budget.
template <class World>
inline ExplodeResult simExplode(const World& w, int cx, int cy, int cz,
                                size_t maxVisits = EXPLODE_DEFAULT_MAX_VISITS) {
    struct Node { int x, y, z, depth; };
    const size_t maxBlasts = std::max<size_t>(1, maxVisits / EXPLODE_VISITS_PER_BLAST);
    std::vector<Node> worklist{{cx, cy, cz, 0}};
    std::vector<Node> chain;
    ExplodeResult res;

    // A protected root never goes off. Callers deny the ACTION before it gets
    // here; this keeps the header's own contract whole. Chained links are
    // checked before they are queued, so only the root can arrive protected.
    if (w.protectedAt(cx, cy, cz)) { ++res.protectedHits; return res; }

    while (!worklist.empty()) {
        Node cur = worklist.back();
        worklist.pop_back();
        ++res.explosions;

        ExplodeCell center;
        bool haveCenter = w.get(cur.x, cur.y, cur.z, center);
        ++res.visits;
        int color = haveCenter ? center.color : 0;
        bool painting = (color != 0);
        if (!w.set(cur.x, cur.y, cur.z, w.airType, 0)) ++res.refused;

        chain.clear();
        const int R = EXPLODE_RADIUS;
        for (int i = 1; i <= R; i++) {
            int yy = R - i;
            for (int j = cur.x - R; j <= cur.x + R; j++) {
                for (int k = cur.z - R; k <= cur.z + R; k++) {
                    int ox = j - cur.x, oz = k - cur.z;
                    if (ox * ox + oz * oz + yy * yy > R * R) continue;
                    int ys[2] = {cur.y - yy, cur.y + yy};
                    for (int s = 0; s < 2; s++) {
                        int sy = ys[s];
                        if (sy < w.yMin || sy >= w.yMax) continue;
                        ++res.visits;
                        if (w.protectedAt(j, sy, k)) { ++res.protectedHits; continue; }
                        ExplodeCell c;
                        bool have = w.get(j, sy, k, c);
                        if (painting) {
                            if (have) {
                                if (c.type == w.airType) continue;
                                if (c.type == w.tntType && c.color == 0) continue;
                                w.set(j, sy, k, c.type, color);  // existing cell: never refused
                            } else {
                                if (!w.set(j, sy, k, w.paintedBaseType, color)) ++res.refused;
                            }
                        } else {
                            if (have) {
                                if (c.type == w.airType) continue;
                                if (c.type == w.tntType || c.type == w.fireworkType)
                                    chain.push_back({j, sy, k, cur.depth + 1});
                                else if (c.type != w.bedrockType && c.type != w.steelType)
                                    w.set(j, sy, k, w.airType, 0);
                            } else {
                                if (!w.set(j, sy, k, w.airType, 0)) ++res.refused;
                            }
                        }
                    }
                }
            }
        }
        // A link past the depth guard would never run, so it is not queued and
        // is not "truncated" either — that is the chain's natural end, as in the
        // client. A link that would take the chain past the budget is dropped.
        for (const Node& t : chain) {
            if (t.depth > EXPLODE_MAX_DEPTH) continue;
            if (res.explosions + worklist.size() < maxBlasts) worklist.push_back(t);
            else ++res.truncated;
        }
    }
    return res;
}

}  // namespace ewb
