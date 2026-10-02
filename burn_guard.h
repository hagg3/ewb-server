// burn_guard.h — the pure half of ROADMAP-SERVER stage 10.4: the `--tnt` and
// `--fire` switches, and what the server does when one of them refuses an edit.
//
// A switched-off TNT is refused at placement (10.4a): a `BUILD` of TNT, or of the
// expansion block that fills with TNT, is not applied or relayed, and the sender's
// client is told to take the block back out at once.
//
// A refused burn (10.4b) is harder, because the client lights the block *before*
// it tells the server, and a lit fuse cannot be cancelled. What the server can do:
//
//   * mine the lit cell on the sender's screen straight away. In the game a burning
//     cell that turns to air fires at once (Terrain::update, `tz == TYPE_NONE`), so
//     a lit wood block goes out before its fire spreads (at 1 s) and a lit TNT goes
//     off now instead of in 4 s;
//   * put back everything that burn changed on the sender's screen, from the
//     server's world — once shortly after (BURN_RESTORE_EARLY_SEC), and again once
//     every fire and fuse it could have started is over (burn_restore_late_sec), in
//     case the client did not behave as the game source says (stage 10.0 C6/C7).
//
// "Everything it changed" is preview_burn(): a dry run of the burn against an
// overlay that records writes and commits nothing — the fire's spread through the
// flammable blocks it touches (which the server does not otherwise model yet, 10.3)
// and the blasts and expansion fills it sets off (explode.h). A restore of a cell
// the client did not change redraws it as it already is, so over-reaching is cheap;
// missing one leaves a hole until the player rejoins.
//
// No clock and no lock of its own. Unit-tested in burn_test.cpp.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "block_rules.h"   // is_flammable, is_expansion, BLK_*
#include "explode.h"       // simBurn, ExplodeCell
#include "zone_guard.h"    // ZoneCellSet, RevertCell, ZONE_BURN_RESTORE_MAX

namespace ewb {

// --- the switches ------------------------------------------------------------

/// `on` / `off` (the `--tnt`, `--fire` flags and the control verbs). Nothing else.
inline bool parse_on_off(const std::string& s, bool& out) {
    if (s == "on") { out = true; return true; }
    if (s == "off") { out = false; return true; }
    return false;
}

/// 10.4a: with TNT off, a build of one of these is refused. TNT itself, and the
/// expansion block whose fill is TNT (87) — without it, TNT could still be made.
constexpr bool is_tnt_build(int type) { return type == BLK_TNT || type == BLK_BT_TNT; }

enum class BurnVerdict { Allow, RefuseFire, RefuseTnt };

/// Fire off refuses every burn. TNT off refuses a burn that would set off TNT,
/// make TNT (an expansion fill), or reach TNT through the fire it starts.
inline BurnVerdict burn_verdict(bool tntOn, bool fireOn, bool touchesTnt) {
    if (!fireOn) return BurnVerdict::RefuseFire;
    if (!tntOn && touchesTnt) return BurnVerdict::RefuseTnt;
    return BurnVerdict::Allow;
}

/// The private `[Server]` line the refused player sees (rate-limited by the caller).
inline const char* burn_refusal_notice(BurnVerdict v) {
    return v == BurnVerdict::RefuseTnt ? "TNT is disabled on this server."
                                       : "Burning is disabled on this server.";
}

// --- restore timing (the game's own timers, Terrain.mm) ----------------------

constexpr double BURN_SPREAD_SEC = 1.0;       ///< a burning block lights its neighbours after this
constexpr double BURN_LIFE_SEC = 6.0;         ///< ...and is gone after this (a player-lit TNT: 4 s)
constexpr double BURN_CHAIN_FUSE_SEC = 0.8;   ///< longest fuse of TNT or an expansion lit by a blast/fill
constexpr double BURN_RESTORE_MARGIN_SEC = 1.0;
/// The first restore. Long enough for the immediate mine to have been drawn (a
/// mine and a build in one burst can land in one frame, before the fire is
/// checked), short enough to read as "it bounced back".
constexpr double BURN_RESTORE_EARLY_SEC = 0.25;
constexpr double BURN_RESTORE_LATE_MAX_SEC = 120.0;

/// How long after a player lights an expansion block a MINE from them on that same
/// cell is taken as their client's own follow-up to the burn (the player-lit fuse
/// is 4 s), not as an edit. Not a switch: it applies whenever expansions burn.
constexpr double EXPAND_MINE_ECHO_SEC = 8.0;

/// When the last thing a refused burn started is over on the client: the fire
/// reaches its furthest block after `fireDepth` spreads, that block burns for
/// BURN_LIFE_SEC, and a chain `chainDepth` links deep adds a short fuse per link.
inline double burn_restore_late_sec(int fireDepth, int chainDepth) {
    const double t = fireDepth * BURN_SPREAD_SEC + BURN_LIFE_SEC + chainDepth * BURN_CHAIN_FUSE_SEC +
                     BURN_RESTORE_MARGIN_SEC;
    return std::min(t, BURN_RESTORE_LATE_MAX_SEC);
}

// --- the dry run ---------------------------------------------------------------

/// A world for explode.h's algorithms that reads through to `base` and keeps every
/// write to itself, recording which cells were written. Nothing reaches `base`.
template <class Base>
struct DryRunWorld {
    const Base& base;
    ZoneCellSet* writes;
    mutable std::unordered_map<uint64_t, ExplodeCell> over;
    mutable bool wroteTnt = false;

    int airType, tntType, fireworkType, bedrockType, steelType, goldenType, paintedBaseType;
    int yMin, yMax, radius;

    DryRunWorld(const Base& b, ZoneCellSet* w)
        : base(b), writes(w), airType(b.airType), tntType(b.tntType), fireworkType(b.fireworkType),
          bedrockType(b.bedrockType), steelType(b.steelType), goldenType(b.goldenType),
          paintedBaseType(b.paintedBaseType), yMin(b.yMin), yMax(b.yMax), radius(b.radius) {}

    bool get(int x, int y, int z, ExplodeCell& out) const {
        auto it = over.find(zone_cell_key(x, y, z));
        if (it != over.end()) { out = it->second; return true; }
        return base.get(x, y, z, out);
    }
    bool set(int x, int y, int z, int type, int color) const {
        over[zone_cell_key(x, y, z)] = {(unsigned char)type, (unsigned char)color};
        writes->add(x, y, z);
        if (type == tntType) wroteTnt = true;
        return true;
    }
    // A refused burn changes nothing on the server, so no zone has anything to
    // protect from it; the restore covers protected cells like any other.
    bool protectedAt(int, int, int) const { return false; }
    int baseType(int y) const { return base.baseType(y); }

    /// The block a client sees at (x, y, z), or -1 outside the world's height.
    int typeAt(int x, int y, int z) const {
        if (y < yMin || y >= yMax) return -1;
        ExplodeCell c;
        if (!get(x, y, z, c)) return baseType(y);
        return c.type == paintedBaseType ? baseType(y) : c.type;
    }
};

/// What one burn at (x, y, z) changes on the screen of the client that lit it.
struct BurnPreview {
    int rootType = -1;        ///< the block lit (-1: outside the world)
    bool flammable = false;   ///< the client lights it; anything else, a burn does nothing to
    bool explosive = false;   ///< TNT, a firework or an expansion: emptying the cell sets it off at once
    bool touchesTnt = false;  ///< a blast runs, the fire reaches TNT, or a fill makes TNT
    int fireDepth = 0;        ///< how many times the fire spreads before it runs out of fuel
    int chainDepth = 0;       ///< the deepest blast or expansion link it sets off
    size_t explosions = 0;    ///< blasts the dry run ran (charged like a real burn's)
    size_t visits = 0;        ///< cells the blasts and fills read
    bool truncated = false;   ///< the budget or the cell cap cut the preview short
    ZoneCellSet cells;        ///< every cell to restore, the lit one first
    explicit BurnPreview(size_t cap) : cells(cap) {}
};

/// Dry-run the burn: the fire spreading through every flammable block 6-connected
/// to the lit one (an expansion block catches but does not pass the fire on, as in
/// the game), then, in the order the fire reaches them, each block burning away —
/// TNT and expansion blocks through simBurn, so their blasts and fills are counted.
///
/// Bounded by `cap` cells (the restore cap) and by `maxVisits` for the blasts and
/// fills together (the --burn-max-cells budget). The caller holds whatever lock
/// `w` reads under.
template <class World>
inline BurnPreview preview_burn(const World& w, int x, int y, int z, size_t maxVisits,
                                size_t cap = ZONE_BURN_RESTORE_MAX) {
    BurnPreview p(cap);
    DryRunWorld<World> dw(w, &p.cells);
    p.rootType = dw.typeAt(x, y, z);
    if (p.rootType <= 0 || !is_flammable(p.rootType)) return p;
    p.flammable = true;
    p.explosive = p.rootType == BLK_TNT || p.rootType == BLK_FIREWORK || is_expansion(p.rootType);

    // The fire.
    struct Burning { int x, y, z, depth; };
    std::vector<Burning> fire{{x, y, z, 0}};
    std::unordered_set<uint64_t> seen{zone_cell_key(x, y, z)};
    p.cells.add(x, y, z);
    static const int D[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}, {0, 1, 0}, {0, -1, 0}};
    for (size_t i = 0; i < fire.size(); ++i) {
        const Burning b = fire[i];
        const int t = dw.typeAt(b.x, b.y, b.z);
        p.fireDepth = std::max(p.fireDepth, b.depth);
        if (t == BLK_TNT || t == BLK_BT_TNT) p.touchesTnt = true;
        if (is_expansion(t)) continue;   // catches, but does not spread (IS_BLOCKTNT)
        for (const auto& d : D) {
            const int nx = b.x + d[0], ny = b.y + d[1], nz = b.z + d[2];
            if (!is_flammable(dw.typeAt(nx, ny, nz))) continue;
            if (!seen.insert(zone_cell_key(nx, ny, nz)).second) continue;
            if (fire.size() >= cap) { p.truncated = true; continue; }
            fire.push_back({nx, ny, nz, b.depth + 1});
            p.cells.add(nx, ny, nz);
        }
    }

    // What each burning block does when it goes.
    for (const Burning& b : fire) {
        const int t = dw.typeAt(b.x, b.y, b.z);
        if (t == BLK_TNT || is_expansion(t)) {
            if (p.visits >= maxVisits) { p.truncated = true; continue; }
            const ExplodeResult r = simBurn(dw, b.x, b.y, b.z, maxVisits - p.visits);
            p.visits += r.visits;
            p.explosions += r.explosions;
            if (r.explosions) p.touchesTnt = true;
            if (r.truncated) p.truncated = true;
            p.chainDepth = std::max(p.chainDepth, r.maxDepth + (r.explosions || r.expansions ? 1 : 0));
        } else if (t > 0) {
            dw.set(b.x, b.y, b.z, BLK_AIR, 0);   // burns away; a firework shoots off its own cell
        }
    }
    if (dw.wroteTnt) p.touchesTnt = true;
    if (p.cells.overflow()) p.truncated = true;
    return p;
}

}  // namespace ewb
