// explode.h — the TNT / paint explosion chain (ROADMAP-SERVER stages 7.7, 7.19).
//
// Mirrors Terrain::explode: a spherical radius centred on a cell; color != 0
// paints the sphere, color == 0 destroys it. A TNT caught in the blast
// chains into its own explosion (a firework did too, until 10.2). Pulled out of server_posix.cpp into a
// pure header, world access supplied by the caller, so the worklist bound can
// be unit-tested offline (protocol_test.cpp) without g_world / g_worldMtx.
//
// 7.7: the original recursive simExplode was bounded only by a depth > 6 guard.
// Depth is fine for the stack, but the *fan-out* at each depth is not bounded:
// every TNT inside a blast is chained into — and, as in the client,
// a TNT already consumed by a sibling's blast is exploded again when its turn
// comes — so a dense field gives thousands of explosions, all under one lock
// from one packet. 7.7 made it an explicit worklist with a cap on the *total*
// explosions processed. That cap is a termination bound, not a latency bound.
//
// 7.19: the bound is now a **work budget in cell visits** — every cell a blast
// reads — because visits are what hold the world lock (a blast is ~99% reads:
// a dense chain measured ~4.2 M reads against ~10 K writes). Each blast scans
// a fixed explode_visits_per_blast(radius) cells, so the budget is enforced one whole
// blast at a time: a chain is truncated between blasts, never half-way through
// one. The caller sizes the budget (edenserver's --burn-max-cells) and charges
// the player for the blasts that actually ran (ExplodeResult::explosions).
//
// 8.2: protected zones. `protectedAt(x, y, z)` is asked before a cell is read. A
// protected cell is neither written nor **chained** — a TNT or expansion block
// inside a zone does not go off, so it cannot carry the blast further (skipping only the
// write would still detonate it and chain past the zone). The hook only answers;
// the caller collects the cells it said yes to, for the restore it sends clients
// (which simulate the blast themselves and did destroy them). A protected root
// runs no blast at all. EXPLODE_REACH bounds how far from its root a chain can
// touch, so a caller can drop every zone outside that box before the chain runs.
//
// 10.2: the game's own rules for what a burn sets off (the game's Terrain.mm;
// ROADMAP-SERVER 10.2). A firework, lit or caught in a blast, shoots and
// removes only its own cell — it never blasts and never chains. The golden cube
// survives a blast, like bedrock and steel. An expansion block (ids 81–111) is
// not destroyed by a blast but lit, and a lit one *fills* the air within
// EXPAND_EXTENT of it with its material, lighting the expansion blocks around it
// (blocktntexplode). simBurn() is the entry point for a burn; the expansions run
// after the TNT chain, on what is left of the same visit budget, and a chain of
// them stays inside burn_reach() of the root so the zone hook still covers it.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <unordered_set>
#include <vector>

#include "block_rules.h"   // is_expansion, expansion_material, ramp faces (stage 10.2)

namespace ewb {

/// Recursion-depth guard, kept from the original implementation as a second,
/// orthogonal bound.
constexpr int EXPLODE_MAX_DEPTH = 6;

/// Default blast radius (edenserver --tnt-radius). The game's EXPLOSION_RADIUS is 5;
/// the server used to copy the older public server's 6 (ROADMAP-SERVER 10.1). The
/// radius is a per-world setting (ExplodeWorld::radius); everything below that
/// depends on it takes it as an argument.
constexpr int EXPLODE_RADIUS = 5;

/// Bounds for --tnt-radius. The ceiling keeps one blast's footprint (and the zone
/// reach box) sane; the floor is a blast that still destroys more than its centre.
constexpr int EXPLODE_RADIUS_MIN = 1;
constexpr int EXPLODE_RADIUS_MAX = 16;

/// How far (per axis, in cells) any cell a chain reads or writes can be from its
/// root. A link at depth d sits inside its parent's sphere, so within d * RADIUS of
/// the root; links deeper than EXPLODE_MAX_DEPTH never run, and the deepest blast
/// reaches RADIUS past its own centre.
constexpr int explode_reach(int radius = EXPLODE_RADIUS) { return radius * (EXPLODE_MAX_DEPTH + 1); }
constexpr int EXPLODE_REACH = explode_reach();

/// Cells one blast reads: its centre, plus every sample of the radius-R sphere
/// (the y = 0 layer is sampled twice, as Terrain::explode does). A blast within
/// R of the world's top or bottom reads fewer; the budget charges the full
/// figure regardless, so it stays an upper bound.
constexpr size_t explode_visits_per_blast(int radius = EXPLODE_RADIUS) {
    size_t n = 1;
    const int R = radius;
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
inline void explode_for_each_blast_cell(int cx, int cy, int cz, int yMin, int yMax, F&& fn,
                                        int radius = EXPLODE_RADIUS) {
    if (cy >= yMin && cy < yMax) fn(cx, cy, cz);
    const int R = radius;
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
/// visits is ~1 000 blasts at the old radius 6 (more at 5) — a solid 5x5x5 block of TNT, re-explosions
/// included, runs to completion — and measured ~10 ms of world lock on the
/// development machine (11–25 ns a visit). The 7.7 cap of 4 096 blasts is ~4x
/// that. Stage 7.19 in ROADMAP-SERVER has the numbers.
constexpr size_t EXPLODE_DEFAULT_MAX_VISITS = size_t(1) << 20;

// --- expansion blocks (stage 10.2b) -----------------------------------------

/// How far an expansion fills from its centre, per axis (the game's `er`).
constexpr int EXPAND_EXTENT = 2;

/// How much further than explode_reach() a chain of expansion blocks may carry a
/// burn. A lit expansion lights its expansion neighbours within EXPAND_EXTENT, with
/// no depth limit in the game; the server stops a chain at this box so the zones it
/// is told about (everything within burn_reach() of the root) always cover every
/// cell it can write. A link past it is dropped and counted as truncated.
constexpr int EXPAND_CHAIN_REACH = 64;

/// The box, per axis around a burn's root, that every cell its whole simulation
/// (blasts and expansions) reads or writes lies inside.
constexpr int burn_reach(int radius = EXPLODE_RADIUS) { return explode_reach(radius) + EXPAND_CHAIN_REACH; }

/// Cells one expansion fill reads, at most: the six neighbours that bound it, the
/// box (each layer of yy sampled down and up, as blocktntexplode does), the corner
/// columns of a side variant plus the four faces of each, and the closing pass
/// that lights the expansion blocks around it.
constexpr size_t expand_visits_per_fill() {
    const size_t side = 2 * EXPAND_EXTENT + 1;
    const size_t box = side * side * (EXPAND_EXTENT + 1) * 2;
    const size_t ring = 4 * (EXPAND_EXTENT + 1) * 2;
    return 6 + box + ring * (1 + 4) + box;
}
/// One fired expansion: a read of its centre, then two fills — the game re-lights
/// the centre during the first, and that node fires at once on an emptied cell,
/// which is how the centre ends up as the material rather than air.
constexpr size_t EXPAND_VISITS_PER_LINK = 1 + 2 * expand_visits_per_fill();

/// Every cell an expansion centred on (cx, cy, cz) can fill: the ±EXPAND_EXTENT box,
/// clipped to (yMin, yMax). A superset of any one fill. For a caller restoring the
/// fill of a protected expansion that every client will still set off.
template <class F>
inline void expand_for_each_box_cell(int cx, int cy, int cz, int yMin, int yMax, F&& fn) {
    const int E = EXPAND_EXTENT;
    for (int x = cx - E; x <= cx + E; ++x)
        for (int z = cz - E; z <= cz + E; ++z)
            for (int y = cy - E; y <= cy + E; ++y)
                if (y > yMin && y < yMax) fn(x, y, z);
}

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
    // The block a client draws in a cell nothing is stored for (base_profile.h).
    // An expansion only fills air, so it has to know which unstored cells are air.
    std::function<int(int y)> baseType = [](int) { return 0; };
    int airType = 0;
    int tntType = 9;
    int fireworkType = 65;
    int bedrockType = 1;
    int steelType = 74;
    int goldenType = 71;  ///< the golden cube: a blast skips it, as the game's explodeBlock does
    int paintedBaseType = 255;
    int yMin = 0;
    int radius = EXPLODE_RADIUS;  ///< blast radius in cells (--tnt-radius)
    // Exclusive. The world height (SV_WORLD_HEIGHT / ewb::WS_WORLD_HEIGHT). Was
    // 1024, which let a blast near the top write cells above the world that no
    // REGION reply reads (ROADMAP-SERVER 7.22); protocol_test ties it to the store.
    int yMax = 256;
};

struct ExplodeResult {
    size_t refused = 0;     ///< new cells the caller's world cap refused
    size_t truncated = 0;   ///< chain links dropped when the budget (or the reach box) ran out
    size_t explosions = 0;  ///< blasts actually run (the root included)
    size_t expansions = 0;  ///< expansion blocks actually fired (stage 10.2b)
    size_t visits = 0;      ///< cells actually read
    size_t protectedHits = 0;  ///< samples protectedAt refused (repeats included)
    int maxDepth = 0;       ///< deepest link that ran (root = 0); a restore's timing uses it
};

namespace detail {

/// One burn's simulation: the TNT chain, then every expansion block it lit (or the
/// root expansion), against one shared visit budget.
///
/// Order: every blast runs before any expansion fires. In the game a chained TNT
/// and a lit expansion both have a 0.5–0.8 s random fuse, so each client interleaves
/// them its own way; blasts first is the one order the server can state. The only
/// case it changes is a later blast tearing up an earlier fill.
template <class World>
class BurnRun {
public:
    BurnRun(const World& w, int rx, int ry, int rz, size_t maxVisits)
        : w_(w), rx_(rx), ry_(ry), rz_(rz), maxVisits_(maxVisits),
          reach_(burn_reach(w.radius) - EXPAND_EXTENT) {}

    ExplodeResult res;

    /// The TNT chain from (cx, cy, cz) — simExplode's 7.7/7.19 algorithm. TNT in a
    /// blast chains; an expansion block is lit (queued) instead of destroyed; a
    /// firework is destroyed like any block and does not chain (10.2a: in the game
    /// it is lit, shoots, and removes only its own cell); bedrock, steel and the
    /// golden cube survive.
    void blastChain(int cx, int cy, int cz) {
        struct Node { int x, y, z, depth; };
        const size_t maxBlasts = std::max<size_t>(1, maxVisits_ / explode_visits_per_blast(w_.radius));
        std::vector<Node> worklist{{cx, cy, cz, 0}};
        std::vector<Node> chain;

        while (!worklist.empty()) {
            Node cur = worklist.back();
            worklist.pop_back();
            ++res.explosions;
            res.maxDepth = std::max(res.maxDepth, cur.depth);

            ExplodeCell center;
            bool haveCenter = w_.get(cur.x, cur.y, cur.z, center);
            ++res.visits;
            int color = haveCenter ? center.color : 0;
            bool painting = (color != 0);
            if (!w_.set(cur.x, cur.y, cur.z, w_.airType, 0)) ++res.refused;

            chain.clear();
            const int R = w_.radius;
            for (int i = 1; i <= R; i++) {
                int yy = R - i;
                for (int j = cur.x - R; j <= cur.x + R; j++) {
                    for (int k = cur.z - R; k <= cur.z + R; k++) {
                        int ox = j - cur.x, oz = k - cur.z;
                        if (ox * ox + oz * oz + yy * yy > R * R) continue;
                        int ys[2] = {cur.y - yy, cur.y + yy};
                        for (int s = 0; s < 2; s++) {
                            int sy = ys[s];
                            if (sy < w_.yMin || sy >= w_.yMax) continue;
                            ++res.visits;
                            if (w_.protectedAt(j, sy, k)) { ++res.protectedHits; continue; }
                            ExplodeCell c;
                            bool have = w_.get(j, sy, k, c);
                            if (painting) {
                                if (have) {
                                    if (c.type == w_.airType) continue;
                                    if (c.type == w_.tntType && c.color == 0) continue;
                                    w_.set(j, sy, k, c.type, color);  // existing cell: never refused
                                } else {
                                    if (!w_.set(j, sy, k, w_.paintedBaseType, color)) ++res.refused;
                                }
                            } else {
                                if (have) {
                                    if (c.type == w_.airType) continue;
                                    if (c.type == w_.tntType)
                                        chain.push_back({j, sy, k, cur.depth + 1});
                                    else if (is_expansion(c.type))
                                        light(j, sy, k, c.type, cur.depth + 1, /*askZone=*/false);
                                    else if (c.type != w_.bedrockType && c.type != w_.steelType &&
                                             c.type != w_.goldenType)
                                        w_.set(j, sy, k, w_.airType, 0);
                                } else {
                                    if (!w_.set(j, sy, k, w_.airType, 0)) ++res.refused;
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
    }

    /// Queue the root of a burn that is itself an expansion block. It always runs.
    void lightRoot(int x, int y, int z, int type) {
        lit_.insert(key(x, y, z));
        queue_.push_back({x, y, z, type, 0});
        rootQueued_ = true;
    }

    /// Fire every lit expansion block, in the order lit. Each one may light more.
    void expansions() {
        for (size_t qi = 0; qi < queue_.size(); ++qi) {
            const Link n = queue_[qi];
            const bool isRoot = rootQueued_ && qi == 0;
            if (!isRoot && res.visits + EXPAND_VISITS_PER_LINK > maxVisits_) {
                res.truncated += queue_.size() - qi;
                return;
            }
            fire(n);
        }
    }

private:
    struct Link { int x, y, z, type, depth; };

    static uint64_t key(int x, int y, int z) {
        return (((uint64_t)(uint32_t)x & 0xFFFFFFull) << 40) |
               (((uint64_t)(uint32_t)z & 0xFFFFFFull) << 16) | ((uint64_t)(uint32_t)y & 0xFFFFull);
    }

    /// The block a client sees at (x, y, z): the stored one, the natural one where
    /// nothing is stored, or -1 outside the world's height (the game's unloaded
    /// chunk, which nothing treats as air).
    int typeAt(int x, int y, int z) {
        ++res.visits;
        if (y < w_.yMin || y >= w_.yMax) return -1;
        ExplodeCell c;
        if (!w_.get(x, y, z, c)) return w_.baseType(y);
        return c.type == w_.paintedBaseType ? w_.baseType(y) : c.type;
    }

    /// The game's burnBlock(…, TRUE) on an expansion block: queue it unless it is
    /// already lit. The cell currently firing is re-lit by its own fill; that is
    /// the refire fire() handles, not a new link.
    void light(int x, int y, int z, int type, int depth, bool askZone) {
        if (firing_ && x == fx_ && y == fy_ && z == fz_) { refire_ = true; return; }
        const uint64_t k = key(x, y, z);
        if (lit_.count(k)) return;
        lit_.insert(k);   // decided once, whatever the answer
        if (askZone && w_.protectedAt(x, y, z)) { ++res.protectedHits; return; }
        if (std::abs(x - rx_) > reach_ || std::abs(z - rz_) > reach_ || std::abs(y - ry_) > reach_) {
            ++res.truncated;
            return;
        }
        queue_.push_back({x, y, z, type, depth});
    }

    /// One cell of a fill: air takes `fillType`; an expansion block is lit.
    void fillCell(int x, int y, int z, int fillType, int color, int depth) {
        const int t = typeAt(x, y, z);
        if (t == w_.airType) {
            if (fillType == w_.airType) return;   // TYPE_BLOCK_TNT: air into air
            if (w_.protectedAt(x, y, z)) { ++res.protectedHits; return; }
            if (!w_.set(x, y, z, fillType, color)) ++res.refused;
        } else if (t > 0 && is_expansion(t)) {
            light(x, y, z, t, depth, /*askZone=*/true);
        }
    }

    /// The game's Terrain::blocktntexplode, for one centre.
    void fill(int cx, int cy, int cz, int btype, int color, int depth) {
        const int E = EXPAND_EXTENT;
        const int mat = expansion_material(btype);
        const int ramp = expansion_ramp(btype);
        auto solid = [&](int x, int y, int z) { return typeAt(x, y, z) > 0; };
        // A side already against something solid does not grow in that direction.
        const int left = solid(cx - 1, cy, cz) ? 0 : E;
        const int right = solid(cx + 1, cy, cz) ? 0 : E;
        const int fwd = solid(cx, cy, cz - 1) ? 0 : E;
        const int back = solid(cx, cy, cz + 1) ? 0 : E;
        const int top = (cy + 1 == w_.yMax || solid(cx, cy + 1, cz)) ? 0 : E;
        const int bot = (cy - 1 <= w_.yMin || solid(cx, cy - 1, cz)) ? 0 : E;
        auto corner = [&](int j, int k) { return std::abs(j - cx) + std::abs(k - cz) == 2 * E; };
        auto eachCell = [&](int lo, int hi, int f, int b, bool both, auto&& fn) {
            for (int i = 0; i <= E; i++) {
                const int yy = E - i;
                for (int j = cx - lo; j <= cx + hi; j++)
                    for (int k = cz - f; k <= cz + b; k++) {
                        if (!both) {
                            if (cy - yy > w_.yMin && yy <= bot) fn(j, cy - yy, k);
                            if (cy + yy < w_.yMax && yy <= top) fn(j, cy + yy, k);
                        } else {
                            if (cy - yy > w_.yMin) fn(j, cy - yy, k);
                            if (cy + yy < w_.yMax) fn(j, cy + yy, k);
                        }
                    }
            }
        };
        // The box. A side variant leaves its four corner columns for the ramp ring.
        eachCell(left, right, fwd, back, false, [&](int x, int y, int z) {
            if (ramp && corner(x, z)) return;
            fillCell(x, y, z, mat, color, depth + 1);
        });
        // The ramp ring: each corner cell turns to a side, a ramp or a full block by
        // what its four faces are against (the box filled just now included).
        if (ramp)
            eachCell(left, right, fwd, back, false, [&](int x, int y, int z) {
                if (!corner(x, z)) return;
                const int t = typeAt(x, y, z);
                if (t == w_.airType) {
                    if (w_.protectedAt(x, y, z)) { ++res.protectedHits; return; }
                    bool faces[4];
                    for (int d = 0; d < 4; ++d) faces[d] = face_solid(typeAt(x + FACE_DX[d], y, z + FACE_DZ[d]), d);
                    if (!w_.set(x, y, z, ramp_for_faces(ramp, faces), color)) ++res.refused;
                } else if (t > 0 && is_expansion(t)) {
                    light(x, y, z, t, depth + 1, /*askZone=*/true);
                }
            });
        // Last, every expansion block in the whole box is lit, whatever bounded the fill.
        eachCell(E, E, E, E, true, [&](int x, int y, int z) {
            const int t = typeAt(x, y, z);
            if (t > 0 && is_expansion(t)) light(x, y, z, t, depth + 1, /*askZone=*/true);
        });
    }

    /// A lit expansion's fuse runs out. Its fill runs with the colour the block has
    /// now, re-lighting the block itself; the burn then empties the cell, so that
    /// re-lit node fires at once and fills the centre too — with no colour, since
    /// emptying the cell cleared it.
    void fire(const Link& n) {
        ++res.expansions;
        res.maxDepth = std::max(res.maxDepth, n.depth);
        ExplodeCell c;
        const bool have = w_.get(n.x, n.y, n.z, c);
        ++res.visits;
        firing_ = true;
        fx_ = n.x; fy_ = n.y; fz_ = n.z;
        refire_ = false;
        fill(n.x, n.y, n.z, n.type, have ? c.color : 0, n.depth);
        if (!w_.set(n.x, n.y, n.z, w_.airType, 0)) ++res.refused;
        if (refire_) fill(n.x, n.y, n.z, n.type, 0, n.depth);
        firing_ = false;
    }

    const World& w_;
    int rx_, ry_, rz_;
    size_t maxVisits_;
    int reach_;   ///< how far from the root an expansion's centre may be
    std::vector<Link> queue_;
    std::unordered_set<uint64_t> lit_;
    bool rootQueued_ = false;
    bool firing_ = false, refire_ = false;
    int fx_ = 0, fy_ = 0, fz_ = 0;
};

}  // namespace detail

/// Runs the TNT chain rooted at (cx, cy, cz) until it finishes or the next blast
/// would exceed `maxVisits`, then fires every expansion block it lit with what is
/// left of the budget. The root blast always runs, whatever the budget.
template <class World>
inline ExplodeResult simExplode(const World& w, int cx, int cy, int cz,
                                size_t maxVisits = EXPLODE_DEFAULT_MAX_VISITS) {
    // A protected root never goes off. Callers deny the ACTION before it gets
    // here; this keeps the header's own contract whole. Chained links are
    // checked before they are queued, so only the root can arrive protected.
    if (w.protectedAt(cx, cy, cz)) {
        ExplodeResult res;
        ++res.protectedHits;
        return res;
    }
    detail::BurnRun<World> run(w, cx, cy, cz, maxVisits);
    run.blastChain(cx, cy, cz);
    run.expansions();
    return run.res;
}

/// What a burn (`ACTION:x:y:z:2`) does to the world, by what is at (x, y, z):
///
///   TNT                 a blast, chaining (simExplode)
///   an expansion block  its fill, chaining through the expansion blocks it lights (10.2b)
///   a firework          that one cell becomes air; nothing else (10.2a)
///   any other block     becomes air (the game leaves a non-flammable block alone — 10.3/D8)
///   nothing stored      nothing
///
/// The client only ever sends the cell the player lit and works out the rest itself;
/// this is the server working out the same rest.
template <class World>
inline ExplodeResult simBurn(const World& w, int x, int y, int z,
                             size_t maxVisits = EXPLODE_DEFAULT_MAX_VISITS) {
    ExplodeResult res;
    if (w.protectedAt(x, y, z)) { ++res.protectedHits; return res; }
    ExplodeCell c;
    res.visits = 1;
    if (!w.get(x, y, z, c) || c.type == w.airType) return res;
    if (c.type == w.tntType) return simExplode(w, x, y, z, maxVisits);
    if (is_expansion(c.type)) {
        detail::BurnRun<World> run(w, x, y, z, maxVisits);
        run.res.visits = 1;
        run.lightRoot(x, y, z, c.type);
        run.expansions();
        return run.res;
    }
    if (!w.set(x, y, z, w.airType, 0)) ++res.refused;
    return res;
}

}  // namespace ewb
