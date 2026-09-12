// explode.h — the TNT / paint explosion chain (ROADMAP-SERVER stage 7.7).
//
// Mirrors Terrain::explode: a spherical radius centred on a cell; color != 0
// paints the sphere, color == 0 destroys it. A TNT or firework caught in the
// blast chains into its own explosion. Pulled out of server_posix.cpp into a
// pure header, world access supplied by the caller, so the worklist bound can
// be unit-tested offline (protocol_test.cpp) without g_world / g_worldMtx.
//
// Defect this fixes: the original recursive simExplode was bounded only by a
// depth > 6 guard. Depth is fine for the stack — six levels of recursion is
// nothing — but the *fan-out* at each depth is not bounded: every TNT/firework
// inside a blast is chained into, and a dense field gives branch-factor^6
// explosions, each a ~13^3 sphere scan, all under one lock from one packet. The
// fix: an explicit worklist with a hard cap on the *total* explosions processed
// for one ACTION, not just their nesting depth.

#pragma once

#include <cstddef>
#include <functional>
#include <vector>

namespace ewb {

/// Total explosions one chain may process, across all depths. The reference
/// value; callers may override for testing.
constexpr size_t EXPLODE_MAX_CHAIN = 4096;

/// Recursion-depth guard, independent of EXPLODE_MAX_CHAIN — kept from the
/// original implementation as a second, orthogonal bound.
constexpr int EXPLODE_MAX_DEPTH = 6;

constexpr int EXPLODE_RADIUS = 6;

struct ExplodeCell {
    unsigned char type;
    unsigned char color;
};

/// World access + block-id vocabulary the algorithm needs. `get` reports
/// whether a cell has a stored (non-default) value. `set` applies an edit and
/// returns false when the caller's cap refused a brand-new cell — mirrors
/// worldSet's contract so refusals are countable without the header knowing
/// what the cap is.
struct ExplodeWorld {
    std::function<bool(int x, int y, int z, ExplodeCell& out)> get;
    std::function<bool(int x, int y, int z, int type, int color)> set;
    int airType = 0;
    int tntType = 9;
    int fireworkType = 65;
    int bedrockType = 1;
    int steelType = 74;
    int paintedBaseType = 255;
    int yMin = 0;
    int yMax = 1024;  // exclusive
};

/// Runs the chain to completion. Returns the number of cells the cap refused
/// plus chain links dropped once `maxChain` explosions have been processed —
/// both are "this callback's world didn't fully land", so callers that already
/// surface a cap-refusal notice to the player can reuse it for truncation too.
inline size_t simExplode(const ExplodeWorld& w, int cx, int cy, int cz,
                          size_t maxChain = EXPLODE_MAX_CHAIN) {
    struct Node { int x, y, z, depth; };
    std::vector<Node> worklist{{cx, cy, cz, 0}};
    size_t refused = 0;
    size_t processed = 0;

    while (!worklist.empty()) {
        Node cur = worklist.back();
        worklist.pop_back();
        if (cur.depth > EXPLODE_MAX_DEPTH) continue;
        ++processed;

        ExplodeCell center;
        bool haveCenter = w.get(cur.x, cur.y, cur.z, center);
        int color = haveCenter ? center.color : 0;
        bool painting = (color != 0);
        if (!w.set(cur.x, cur.y, cur.z, w.airType, 0)) ++refused;

        std::vector<Node> chain;
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
                        ExplodeCell c;
                        bool have = w.get(j, sy, k, c);
                        if (painting) {
                            if (have) {
                                if (c.type == w.airType) continue;
                                if (c.type == w.tntType && c.color == 0) continue;
                                w.set(j, sy, k, c.type, color);  // existing cell: never refused
                            } else {
                                if (!w.set(j, sy, k, w.paintedBaseType, color)) ++refused;
                            }
                        } else {
                            if (have) {
                                if (c.type == w.airType) continue;
                                if (c.type == w.tntType || c.type == w.fireworkType)
                                    chain.push_back({j, sy, k, cur.depth + 1});
                                else if (c.type != w.bedrockType && c.type != w.steelType)
                                    w.set(j, sy, k, w.airType, 0);
                            } else {
                                if (!w.set(j, sy, k, w.airType, 0)) ++refused;
                            }
                        }
                    }
                }
            }
        }
        for (const Node& t : chain) {
            if (processed + worklist.size() < maxChain) worklist.push_back(t);
            else ++refused;  // chain truncated at the cap
        }
    }
    return refused;
}

}  // namespace ewb
