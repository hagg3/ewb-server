// block_rules.h — the game's block vocabulary that burning depends on
// (ROADMAP-SERVER stage 10): which blocks are flammable, the expansion blocks
// (ids 81–111) and what each one fills with, and which faces of a ramp or a
// side block count as solid.
//
// Ported from the game's own tables (Globals.mm `blockinfo` IS_FLAMMABLE /
// IS_BLOCKTNT, Terrain.mm `blockTntMap`, `isRampFaceSolid` / `isSideFaceSolid`).
// ⚠️ The source tree those come from is older than the retail client, so stage
// 10.0 has to confirm them against a retail capture (C4 for the expansion table).
//
// Pure data and small lookups, no world access. explode.h runs the burn rules on
// top of it; burn_guard.h uses it to work out what a refused burn changed on the
// sender's screen. Unit-tested in burn_test.cpp.

#pragma once

namespace ewb {

/// Block ids the burn rules name. The rest of the server only knows air, TNT,
/// bedrock, steel and the painted-base sentinel (server_posix.cpp's SV_* enum).
enum : int {
    BLK_AIR = 0,
    BLK_BEDROCK = 1,
    BLK_STONE = 2,
    BLK_DIRT = 3,
    BLK_SAND = 4,
    BLK_LEAVES = 5,
    BLK_TREE = 6,
    BLK_WOOD = 7,
    BLK_GRASS = 8,
    BLK_TNT = 9,
    BLK_DARK_STONE = 10,
    BLK_BRICK = 13,
    BLK_COBBLESTONE = 14,
    BLK_ICE = 15,
    BLK_CRYSTAL = 16,
    BLK_TRAMPOLINE = 17,
    BLK_LADDER = 18,
    BLK_CLOUD = 19,
    BLK_WATER = 20,
    BLK_WEAVE = 21,
    BLK_VINE = 22,
    BLK_LAVA = 23,
    BLK_STONE_RAMP1 = 24,
    BLK_WOOD_RAMP1 = 28,
    BLK_SHINGLE_RAMP1 = 32,
    BLK_ICE_RAMP1 = 36,
    BLK_ICE_RAMP4 = 39,
    BLK_STONE_SIDE1 = 40,
    BLK_ICE_SIDE4 = 55,
    BLK_SHINGLE = 56,
    BLK_GRADIENT = 57,
    BLK_GLASS = 58,
    BLK_FIREWORK = 65,
    BLK_DOOR1 = 66,
    BLK_DOOR_TOP = 70,
    BLK_GOLDEN_CUBE = 71,
    BLK_LIGHTBOX = 72,
    BLK_FLOWER = 73,
    BLK_STEEL = 74,
    BLK_EXPAND_FIRST = 81,   ///< TYPE_BLOCK_TNT, the plain expansion block (fills with nothing)
    BLK_BT_TNT = 87,         ///< the expansion block that fills with TNT
    BLK_BT_STONESIDE = 102,  ///< 102..105: the side variants, which add a ramp ring
    BLK_BT_WOODSIDE = 103,
    BLK_BT_ICESIDE = 104,
    BLK_BT_SHINGLESIDE = 105,
    BLK_EXPAND_LAST = 111,
};

/// An expansion block (the game's IS_BLOCKTNT): lit, it fills the air around it
/// instead of exploding.
constexpr bool is_expansion(int type) { return type >= BLK_EXPAND_FIRST && type <= BLK_EXPAND_LAST; }

/// The game's IS_FLAMMABLE: what a burn lights and fire spreads through.
constexpr bool is_flammable(int type) {
    return type == BLK_LEAVES || type == BLK_TREE || type == BLK_WOOD || type == BLK_TNT ||
           type == BLK_LADDER || type == BLK_WEAVE ||
           (type >= BLK_WOOD_RAMP1 && type <= BLK_WOOD_RAMP1 + 3) ||   // wood ramps 28..31
           (type >= 44 && type <= 47) ||                                // wood sides
           type == BLK_FIREWORK || (type >= BLK_DOOR1 && type <= BLK_DOOR_TOP) ||
           type == BLK_FLOWER || is_expansion(type);
}

/// What an expansion block fills with (the game's blockTntMap). 0 for TYPE_BLOCK_TNT
/// (81), which has no entry: it fills with air, so in effect it only vanishes.
constexpr int expansion_material(int type) {
    switch (type) {
        case 82: return BLK_GRASS;
        case 83: return BLK_DARK_STONE;
        case 84: return BLK_STONE;
        case 85: return BLK_DIRT;
        case 86: return BLK_SAND;
        case 87: return BLK_TNT;
        case 88: return BLK_WOOD;
        case 89: return BLK_SHINGLE;
        case 90: return BLK_GLASS;
        case 91: return BLK_GRADIENT;
        case 92: return BLK_TREE;
        case 93: return BLK_LEAVES;
        case 94: return BLK_BRICK;
        case 95: return BLK_COBBLESTONE;
        case 96: return BLK_VINE;
        case 97: return BLK_LADDER;
        case 98: return BLK_ICE;
        case 99: return BLK_CRYSTAL;
        case 100: return BLK_TRAMPOLINE;
        case 101: return BLK_CLOUD;
        case 102: return BLK_STONE;
        case 103: return BLK_WOOD;
        case 104: return BLK_ICE;
        case 105: return BLK_SHINGLE;
        case 106: return BLK_WEAVE;
        case 107: return BLK_WATER;
        case 108: return BLK_LAVA;
        case 109: return BLK_FIREWORK;
        case 110: return BLK_LIGHTBOX;
        case 111: return BLK_STEEL;
        default: return BLK_AIR;
    }
}

/// For the four side variants (102..105), the first ramp of their material, which
/// fills the four corner columns of the box; 0 for every other block.
constexpr int expansion_ramp(int type) {
    switch (type) {
        case BLK_BT_STONESIDE: return BLK_STONE_RAMP1;
        case BLK_BT_WOODSIDE: return BLK_WOOD_RAMP1;
        case BLK_BT_ICESIDE: return BLK_ICE_RAMP1;
        case BLK_BT_SHINGLESIDE: return BLK_SHINGLE_RAMP1;
        default: return 0;
    }
}

/// The full block a ramp turns into when three or more of its faces are against
/// something solid (getRampType2's n > 2 case).
constexpr int ramp_solid_material(int ramp1) {
    return ramp1 == BLK_STONE_RAMP1 ? BLK_STONE
         : ramp1 == BLK_ICE_RAMP1 ? BLK_ICE
         : ramp1 == BLK_WOOD_RAMP1 ? BLK_WOOD
         : ramp1 == BLK_SHINGLE_RAMP1 ? BLK_SHINGLE
         : ramp1;
}

/// Horizontal neighbour directions, in the game's isFaceSolid order: +x, +z, -x, -z.
constexpr int FACE_DX[4] = {1, 0, -1, 0};
constexpr int FACE_DZ[4] = {0, 1, 0, -1};

/// Whether face `d` (0..3 as above; 4/5 are down/up) of a ramp or side block of
/// rotation `rot` (= type % 4) is solid. The game's isRampFaceSolid / isSideFaceSolid.
constexpr bool RAMP_FACE_SOLID[4][6] = {
    {false, false, false, true, false, true},
    {true, false, false, false, false, true},
    {false, true, false, false, false, true},
    {false, false, true, false, false, true},
};
constexpr bool SIDE_FACE_SOLID[4][6] = {
    {false, false, true, true, false, false},
    {true, false, false, true, false, false},
    {true, true, false, false, false, false},
    {false, true, true, false, false, false},
};

/// Whether a neighbour of type `type`, seen from direction `d`, counts as a solid
/// face (the game's isFaceSolid, given the neighbour's type). Air and an unknown
/// cell (< 0) are not solid; a ramp or side is solid only on its solid faces.
constexpr bool face_solid(int type, int d) {
    if (type <= 0) return false;
    if (type >= BLK_STONE_RAMP1 && type <= BLK_ICE_RAMP4) return RAMP_FACE_SOLID[type % 4][d];
    if (type >= BLK_STONE_SIDE1 && type <= BLK_ICE_SIDE4) return SIDE_FACE_SOLID[type % 4][d];
    return true;
}

/// The block a side variant's corner cell becomes (the game's getRampType2), given
/// whether each of its four horizontal faces is against something solid.
///
/// Two adjacent solid faces make a side block facing them; three or more make the
/// full block. Otherwise the game rotates the ramp by the **local player's** yaw,
/// which no server can know (and which differs between clients), so the unrotated
/// ramp is used. In the expansion's own corner cells that case only arises when a
/// cell beside the corner was not filled, which is rare.
constexpr int ramp_for_faces(int ramp1, const bool solid[4]) {
    int n = 0;
    for (int i = 0; i < 4; ++i) n += solid[i] ? 1 : 0;
    if (n == 2)
        for (int i = 0; i < 4; ++i)
            if (solid[i] && solid[(i + 1) % 4]) return ramp1 + (BLK_STONE_SIDE1 - BLK_STONE_RAMP1) + i;
    if (n > 2) return ramp_solid_material(ramp1);
    return ramp1;
}

}  // namespace ewb
