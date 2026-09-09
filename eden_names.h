// eden_names.h — community/RE-derived name tables for Eden's blocks, paint
// palette and character types (ROADMAP-SERVER stage 3.6, plan §0.5.1).
//
// Harvested verbatim from `server_posix_modded.cpp`'s `BLOCK_MAP` / `COLOR_MAP`
// / `charNames[]`. **Neither this list nor the companion editor's is
// authoritative** — both are community reverse-engineering. Known unresolved
// conflicts (plan §0.5.1), none of which change a numeric id:
//
//   * blocks 14 / 19 / 21 / 56 / 57 / 74 — the two lists pick different names
//     for the same id; naming taste, no functional effect.
//   * blocks 24–27 and 40–55 — a 180° disagreement about which compass
//     direction the ramp faces. The names here follow the modded convention.
//     The `//copy`→`//rotate`→`//paste` experiment that would settle the
//     direction needs a retail client and is tracked as stage 3.6a.
//   * blocks 66–69 have no community name and are exposed as `unknown66`..;
//     block 71 collides with 16 on the name `gem` in the source map and is
//     exposed here as `gem71`.
//
// Blocks are 0..111. Ids in 112..MAX_BLOCK_TYPE and paint ids above 54 have no
// name — numeric ids still work everywhere they are accepted.

#pragma once

#include <cctype>
#include <string>
#include <unordered_map>

namespace ewb {

/// Block id -> name, indexed 0..111. `count` is set to the array length.
inline const char* const* eden_block_table(int& count) {
    static const char* const names[] = {
        "air", "bedrock", "stone", "dirt", "sand", "leaves", "log", "planks",
        "grass", "tnt", "darkstone", "clover", "grass2", "brick", "tile", "ice",
        "gem", "spring", "ladder", "blank", "water", "lattice", "vines", "lava",
        "stone_slope_n", "stone_slope_e", "stone_slope_s", "stone_slope_w",
        "planks_slope_n", "planks_slope_e", "planks_slope_s", "planks_slope_w",
        "stonebrick_slope_n", "stonebrick_slope_e", "stonebrick_slope_s",
        "stonebrick_slope_w", "ice_slope_n", "ice_slope_e", "ice_slope_s",
        "ice_slope_w", "stone_slope_nw", "stone_slope_ne", "stone_slope_se",
        "stone_slope_sw", "planks_slope_nw", "planks_slope_ne", "planks_slope_se",
        "planks_slope_sw", "stonebrick_slope_nw", "stonebrick_slope_ne",
        "stonebrick_slope_se", "stonebrick_slope_sw", "ice_slope_nw",
        "ice_slope_ne", "ice_slope_se", "ice_slope_sw", "stonebrick", "shade",
        "glass", "water2", "water3", "water4", "lava2", "lava3", "lava4",
        "firework", "unknown66", "unknown67", "unknown68", "unknown69", "door",
        "gem71", "lamp", "flower", "metal", "portal1", "portal2", "portal3",
        "portal4", "portal5", "petrified_leaves", "exploding_expand",
        "expand_grass", "expand_darkstone", "expand_stone", "expand_dirt",
        "expand_sand", "expand_tnt", "expand_planks", "expand_stonebrick",
        "expand_glass", "expand_shade", "expand_log", "expand_leaves",
        "expand_brick", "expand_tile", "expand_moss", "expand_ladder",
        "expand_ice", "expand_gem", "expand_spring", "expand_blank",
        "expand_stone_curve", "expand_planks_curve", "expand_ice_curve",
        "expand_stonebrick_curve", "expand_lattice", "expand_water",
        "expand_lava", "expand_firework", "expand_lamp", "expand_metal",
    };
    count = (int)(sizeof(names) / sizeof(names[0]));
    return names;
}

/// Paint id -> name, indexed 0..54. Index 0 is the no-paint sentinel (what
/// `//unpaint` writes); 1..54 are the 6-tone × 9-hue palette. This matches the
/// modded `getColorId`, which returns `COLOR_MAP[name] + 1`.
inline const char* const* eden_paint_table(int& count) {
    static const char* const names[] = {
        "unpaint",
        "pale_red", "pale_orange", "pale_yellow", "pale_green", "pale_cyan",
        "pale_blue", "pale_purple", "pale_pink", "white",
        "light_red", "light_orange", "light_yellow", "light_green", "light_cyan",
        "light_blue", "light_purple", "light_pink", "light_grey",
        "red", "orange", "yellow", "green", "cyan", "blue", "purple", "pink",
        "grey",
        "dark_red", "dark_orange", "dark_yellow", "dark_green", "dark_cyan",
        "dark_blue", "dark_purple", "dark_pink", "dark_grey",
        "deep_red", "deep_orange", "deep_yellow", "deep_green", "deep_cyan",
        "deep_blue", "deep_purple", "deep_pink", "deep_grey",
        "darkest_red", "darkest_orange", "darkest_yellow", "darkest_green",
        "darkest_cyan", "darkest_blue", "darkest_purple", "darkest_pink",
        "black",
    };
    count = (int)(sizeof(names) / sizeof(names[0]));
    return names;
}

/// Character type -> name, indexed 0..6 (JOIN field 2, when it is a plain index
/// — see plan §0.5.3). Not recorded anywhere else in this repo.
inline const char* const* eden_char_table(int& count) {
    static const char* const names[] = {
        "Moof", "Batty", "Green", "Nergle", "Stumpy", "Charger", "Stalker",
    };
    count = (int)(sizeof(names) / sizeof(names[0]));
    return names;
}

inline const char* eden_block_name(int id) {
    int n; const char* const* t = eden_block_table(n);
    return (id >= 0 && id < n) ? t[id] : "";
}

inline const char* eden_paint_name(int id) {
    int n; const char* const* t = eden_paint_table(n);
    return (id >= 0 && id < n) ? t[id] : "";
}

inline const char* eden_char_name(int id) {
    int n; const char* const* t = eden_char_table(n);
    return (id >= 0 && id < n) ? t[id] : "";
}

inline std::string eden_tolower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

/// Block name -> id, case-insensitive. `-1` when the name is unknown. Numeric
/// strings are *not* handled here — that is `we_parse_block`'s job.
inline int eden_block_id(const std::string& name) {
    static const std::unordered_map<std::string, int> m = [] {
        std::unordered_map<std::string, int> mm;
        int n; const char* const* t = eden_block_table(n);
        for (int i = 0; i < n; ++i) mm.emplace(t[i], i);
        mm["adminium"] = 1;   // BLOCK_MAP alias for bedrock
        return mm;
    }();
    auto it = m.find(eden_tolower(name));
    return it == m.end() ? -1 : it->second;
}

/// Paint name -> id in 0..54, case-insensitive. `-1` when unknown. `unpaint` and
/// its `COLOR_MAP` aliases all resolve to the 0 sentinel.
inline int eden_paint_id(const std::string& name) {
    static const std::unordered_map<std::string, int> m = [] {
        std::unordered_map<std::string, int> mm;
        int n; const char* const* t = eden_paint_table(n);
        for (int i = 0; i < n; ++i) mm.emplace(t[i], i);
        mm["none"] = 0; mm["base"] = 0; mm["original"] = 0; mm["scrape"] = 0;
        return mm;
    }();
    auto it = m.find(eden_tolower(name));
    return it == m.end() ? -1 : it->second;
}

/// Append `name=id` for every table entry whose name contains `query` (a
/// case-insensitive substring) to `out`, comma-separated, stopping once `out`
/// would exceed `budget` bytes (a trailing `, ...` marks the truncation). `skip0`
/// drops the index-0 sentinel, which the paint table has and the block one does
/// not. Returns the number of matches emitted.
inline int eden_search(const char* const* table, int n, const std::string& query,
                       bool skip0, size_t budget, std::string& out) {
    const std::string q = eden_tolower(query);
    int emitted = 0;
    for (int i = skip0 ? 1 : 0; i < n; ++i) {
        if (eden_tolower(table[i]).find(q) == std::string::npos) continue;
        const std::string entry = std::string(table[i]) + "=" + std::to_string(i);
        if (!out.empty() && out.size() + 2 + entry.size() > budget) {
            out += ", ...";
            return emitted;
        }
        if (!out.empty()) out += ", ";
        out += entry;
        ++emitted;
    }
    return emitted;
}

}  // namespace ewb
