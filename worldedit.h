// worldedit.h — the pure, testable half of ROADMAP-SERVER stage 3.3: the Tier 2
// player command surface.
//
// Tier 2 is **in band**: these are chat lines. A player types `//set 2` into the
// game's chat box, the client sends it as an ordinary `MSG:`, and the server
// answers on the same socket. That is the whole reason this tier is the
// security-critical one — every byte here arrives from an untrusted peer that
// has done nothing but complete a `JOIN`.
//
// The vocabulary is adopted verbatim from the community WorldEdit patch (plan
// §0.5.6) because anyone who has used WorldEdit or Minecraft's `~` syntax
// already knows it. The *mechanism* is rebuilt, and the differences are the
// point of the stage:
//
//   * a `static const` dispatch table with a **minimum permission level baked
//     into every row** — a new command cannot forget to check, because the check
//     happens before the handler is reached (plan §0.5.7 defect 2);
//   * **one bounded edit path**: every selection is volume-checked at the moment
//     it is read, not per command, so a new command physically cannot inherit
//     the `//copy` denial of service (defect 1);
//   * **undo bounded by bytes**, oldest-first, across the undo *and* redo stacks
//     (defect 5) — the patch capped batch *count*, which is ~1.8 GB at 64
//     players;
//   * air is `type 0`, never an erase (defect 4) — that is `server_posix.cpp`'s
//     `worldSet`, which this header's edit records feed.
//
// `server_posix.cpp` owns the sockets, the world model, the locks and the
// broadcast; this header owns the grammar, the table, the bounds arithmetic, the
// undo store and the clipboard transform. Everything below is a pure function or
// a self-contained struct, so `worldedit_test.cpp` can exercise it with no
// server running.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

#include "eden_names.h"  // eden_block_id / eden_paint_id — the 3.6 name tables
#include "hardening.h"   // MAX_BLOCK_TYPE, MAX_PAINT_INDEX

namespace ewb {

// --- permission levels -------------------------------------------------------
//
// Stored by `control.h`'s OpsFile (`eden_ops.txt`), maintained by the Tier 1
// `op` / `deop` verbs since stage 3.2, and consumed here for the first time.
//   0 visitor  — read-only and self-scoped: help, lookups, whispers, resync.
//   1 builder  — WorldEdit: everything that changes cells inside a bounded box.
//   2 operator — anything cross-player: teleporting to a named player discloses
//                that player's exact position (defect 8).
// The server's `--default-level` decides which of these an unlisted player gets,
// so an open creative server can hand out level 1 deliberately rather than by
// omission.
constexpr int WE_LEVEL_VISITOR  = 0;
constexpr int WE_LEVEL_BUILDER  = 1;
constexpr int WE_LEVEL_OPERATOR = 2;

// --- bounds ------------------------------------------------------------------

/// Largest box any single Tier 2 command may *read or write*. Enforced when a
/// selection point is set, again when the selection is read, and again on the
/// bounding box of every radius-driven shape — the three places a box can enter
/// the edit path.
///
/// 131072 cells is a 50³ box, or 512×256 columns. The number is a compromise
/// between "a builder can flatten a plot in one command" and "one chat line
/// cannot stall every other player": at 131072 cells an edit batch is 2 MiB of
/// undo (see `WE_UNDO_BUDGET_BYTES`) and roughly 5 MB of `ACTION` relay.
/// `--we-max-cells` moves it.
constexpr long long WE_MAX_EDIT_CELLS = 131072;

/// Per-player undo budget in bytes, spanning the undo *and* redo stacks
/// together. The patch this replaces bounded batch *count* (10 batches × up to
/// 100 000 edits × 28 B ≈ 28 MB per player, ×64 players ≈ 1.8 GB, plus an
/// equal-sized redo stack). Bytes are the resource that actually runs out, so
/// bytes are what we count. Default is 2 MiB — one full-cap batch — so the
/// 64-player worst case is ~128 MiB. `--we-undo-budget` moves it.
constexpr size_t WE_UNDO_BUDGET_BYTES = 2u * 1024u * 1024u;

/// Cells/second a player may spend, and the burst they may spend at once. The
/// unit is deliberately **cells, not commands**: a single `//sphere 40 2` is one
/// command and ~268 000 cells, so a per-command limiter would be no limiter at
/// all. Charged against the *bounding box* the command will scan, before any of
/// it runs, because the scan is what holds the world lock.
constexpr double WE_CELL_BURST = 262144.0;
constexpr double WE_CELL_RATE  = 32768.0;

/// Hard ceiling on a radius/height argument, so the bounding-box arithmetic
/// stays far inside `long long` and an absurd number is refused by name rather
/// than by overflow. The volume check below is the real bound; this is a guard
/// rail in front of it.
constexpr int WE_MAX_RADIUS = 512;

// --- command table -----------------------------------------------------------

/// One Tier 2 verb. `min_args` / `max_args` count whitespace-separated arguments
/// *after* the verb; `max_args < 0` means "unbounded" (`/msg`, whose tail is free
/// text). `min_level` is the floor — `/tp` raises its own for the player-target
/// form, see `we_tp_required_level`.
struct WeSpec {
    const char* name;
    int         min_level;
    int         min_args;
    int         max_args;
    const char* usage;
};

inline const std::vector<WeSpec>& we_specs() {
    static const std::vector<WeSpec> specs = {
        // --- level 0: read-only and self-scoped ------------------------------
        {"/help",         WE_LEVEL_VISITOR,  0,  1, "/help [page]                       - list the commands you can run"},
        {"/msg",          WE_LEVEL_VISITOR,  2, -1, "/msg <player> <text>               - private message"},
        {"/r",            WE_LEVEL_VISITOR,  1, -1, "/r <text>                          - reply to the last whisper"},
        {"/id",           WE_LEVEL_VISITOR,  1,  1, "/id <block|color>                  - look an id up"},
        {"/searchblocks", WE_LEVEL_VISITOR,  1,  1, "/searchblocks <name>               - find a block by name"},
        {"/searchcolors", WE_LEVEL_VISITOR,  1,  1, "/searchcolors <name>               - find a color by name"},
        {"/resync",       WE_LEVEL_VISITOR,  0,  0, "/resync                            - resend the world around you"},
        // --- level 1: bounded edits ------------------------------------------
        {"/tp",           WE_LEVEL_BUILDER,  1,  3, "/tp <x> <y> <z> | /tp <player>     - teleport (~ is relative)"},
        {"//pos1",        WE_LEVEL_BUILDER,  0,  0, "//pos1                             - first selection corner"},
        {"//pos2",        WE_LEVEL_BUILDER,  0,  0, "//pos2                             - second selection corner"},
        {"//set",         WE_LEVEL_BUILDER,  1,  2, "//set <block> [color]              - fill the selection"},
        {"//walls",       WE_LEVEL_BUILDER,  1,  2, "//walls <block> [color]            - the selection's four sides"},
        {"//replace",     WE_LEVEL_BUILDER,  2,  4, "//replace <old> <new> [color] [oldColor]"},
        {"//replacenear", WE_LEVEL_BUILDER,  3,  5, "//replacenear <radius> <old> <new> [color] [oldColor]"},
        {"//paint",       WE_LEVEL_BUILDER,  1,  1, "//paint <color>                    - paint the selection"},
        {"//unpaint",     WE_LEVEL_BUILDER,  0,  0, "//unpaint                          - strip paint (alias //strip)"},
        {"//strip",       WE_LEVEL_BUILDER,  0,  0, "//strip                            - alias of //unpaint"},
        {"//undo",        WE_LEVEL_BUILDER,  0,  0, "//undo                             - undo your last edit"},
        {"//redo",        WE_LEVEL_BUILDER,  0,  0, "//redo                             - redo what you undid"},
        {"//copy",        WE_LEVEL_BUILDER,  0,  0, "//copy                             - selection to clipboard"},
        {"//paste",       WE_LEVEL_BUILDER,  0,  0, "//paste                            - clipboard at your feet"},
        {"//rotate",      WE_LEVEL_BUILDER,  1,  1, "//rotate <90|180|270>              - rotate the clipboard"},
        {"//up",          WE_LEVEL_BUILDER,  1,  1, "//up <dist>                        - rise on a placed block"},
        {"//sphere",      WE_LEVEL_BUILDER,  2,  3, "//sphere <radius> <block> [color]"},
        {"//hsphere",     WE_LEVEL_BUILDER,  2,  3, "//hsphere <radius> <block> [color] - hollow"},
        {"//cyl",         WE_LEVEL_BUILDER,  3,  4, "//cyl <radius> <height> <block> [color]"},
        {"//hcyl",        WE_LEVEL_BUILDER,  3,  4, "//hcyl <radius> <height> <block> [color] - hollow"},
    };
    return specs;
}

inline const WeSpec* we_find(const std::string& verb) {
    for (const WeSpec& s : we_specs())
        if (verb == s.name) return &s;
    return nullptr;
}

/// True if `argc` arguments satisfy the row's arity.
inline bool we_args_ok(const WeSpec& s, int argc) {
    return argc >= s.min_args && (s.max_args < 0 || argc <= s.max_args);
}

/// `/tp` is two commands sharing a name. Three arguments is a coordinate
/// teleport — self-scoped, level 1. One argument is a player name, which
/// discloses that player's exact position to whoever asks, so it is level 2
/// (plan §0.5.7 defect 8).
inline int we_tp_required_level(int argc) {
    return argc == 1 ? WE_LEVEL_OPERATOR : WE_LEVEL_BUILDER;
}

/// The `/help` body a player at `level` should see: rows above their level are
/// omitted entirely rather than listed and refused.
inline std::vector<std::string> we_help_lines(int level) {
    std::vector<std::string> out;
    for (const WeSpec& s : we_specs()) {
        if (s.min_level > level) continue;
        if (std::string(s.name) == "//strip") continue;   // listed as //unpaint's alias
        out.push_back(s.usage);
    }
    return out;
}

// --- line grammar ------------------------------------------------------------

/// Split a chat command on runs of whitespace. `out[0]` is the verb (including
/// its leading '/'); the rest are arguments. Empty for a blank line.
inline std::vector<std::string> we_split_args(const std::string& line) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= line.size()) break;
        const size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
        out.push_back(line.substr(start, i - start));
    }
    return out;
}

/// The raw remainder of `line` after the first `n` whitespace-separated tokens,
/// with leading whitespace trimmed. `/msg Bob hello  world` keeps its double
/// space; re-joining split tokens would not.
inline std::string we_rest_after(const std::string& line, int n) {
    size_t i = 0;
    for (int t = 0; t < n; ++t) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
    }
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    return line.substr(i);
}

/// Strict decimal integer. `std::stoi` accepts trailing garbage ("5x" -> 5) and
/// throws on empty input; neither is what a command parser wants from a hostile
/// peer.
inline bool we_parse_int(const std::string& tok, int& out) {
    if (tok.empty() || tok.size() > 11) return false;
    size_t i = 0;
    bool neg = false;
    if (tok[0] == '+' || tok[0] == '-') { neg = (tok[0] == '-'); i = 1; }
    if (i >= tok.size()) return false;
    long long v = 0;
    for (; i < tok.size(); ++i) {
        if (tok[i] < '0' || tok[i] > '9') return false;
        v = v * 10 + (tok[i] - '0');
        if (v > 2147483647LL) return false;
    }
    out = (int)(neg ? -v : v);
    return true;
}

/// Minecraft's `~` syntax, kept verbatim because the vocabulary is: `~` is the
/// player's current value, `~n` is an offset from it, anything else is absolute.
/// Fractions are accepted (positions are floats on the wire), integers are the
/// normal case.
inline bool we_parse_coord(const std::string& tok, float current, float& out) {
    auto to_float = [](const std::string& s, float& v) -> bool {
        if (s.empty() || s.size() > 24) return false;
        size_t i = 0;
        if (s[0] == '+' || s[0] == '-') i = 1;
        bool digits = false, dot = false;
        for (; i < s.size(); ++i) {
            if (s[i] >= '0' && s[i] <= '9') { digits = true; continue; }
            if (s[i] == '.' && !dot) { dot = true; continue; }
            return false;
        }
        if (!digits) return false;
        v = std::strtof(s.c_str(), nullptr);
        return true;
    };
    if (tok.empty()) return false;
    if (tok[0] == '~') {
        if (tok.size() == 1) { out = current; return true; }
        float off;
        if (!to_float(tok.substr(1), off)) return false;
        out = current + off;
        return true;
    }
    return to_float(tok, out);
}

// --- block / colour arguments ------------------------------------------------
//
// The community name tables (`BLOCK_MAP` 0–111, `COLOR_MAP` 6×9, plan §0.5.1)
// live in `eden_names.h` since stage 3.6. These two hooks route every call site
// — `//set stone`, `//paint red`, `/id`, `/searchblocks`, `/searchcolors` — into
// that table. An unknown name still returns `-1` and the caller refuses it;
// numeric ids work everywhere regardless.

inline int we_lookup_block(const std::string& name) { return eden_block_id(name); }
inline int we_lookup_color(const std::string& name) { return eden_paint_id(name); }

/// Parse a block argument: a numeric id in 0..MAX_BLOCK_TYPE, or a name from the
/// 3.6 table. `0` is air, and air is a legitimate thing to `//set`.
inline bool we_parse_block(const std::string& tok, int& out) {
    int v;
    if (we_parse_int(tok, v)) {
        if (v < 0 || v > MAX_BLOCK_TYPE) return false;
        out = v;
        return true;
    }
    const int named = we_lookup_block(tok);
    if (named < 0) return false;
    out = named;
    return true;
}

/// Parse a colour argument: 0..MAX_PAINT_INDEX, where 0 is the no-paint
/// sentinel (what `//unpaint` writes).
inline bool we_parse_color(const std::string& tok, int& out) {
    int v;
    if (we_parse_int(tok, v)) {
        if (v < 0 || v > MAX_PAINT_INDEX) return false;
        out = v;
        return true;
    }
    const int named = we_lookup_color(tok);
    if (named < 0) return false;
    out = named;
    return true;
}

// --- selection ---------------------------------------------------------------

/// A normalised inclusive box: `x0 <= x1` on every axis.
struct WeBox {
    int x0 = 0, y0 = 0, z0 = 0;
    int x1 = 0, y1 = 0, z1 = 0;
};

/// Inclusive cell count, computed on `long long` throughout so a hostile pair of
/// coordinates cannot overflow into a small positive number and slip the cap.
inline long long we_volume(long long x0, long long y0, long long z0,
                           long long x1, long long y1, long long z1) {
    const long long dx = (x0 < x1 ? x1 - x0 : x0 - x1) + 1;
    const long long dy = (y0 < y1 ? y1 - y0 : y0 - y1) + 1;
    const long long dz = (z0 < z1 ? z1 - z0 : z0 - z1) + 1;
    return dx * dy * dz;
}

inline long long we_box_volume(const WeBox& b) {
    return we_volume(b.x0, b.y0, b.z0, b.x1, b.y1, b.z1);
}

/// Build a normalised box from two arbitrary corners.
inline WeBox we_make_box(int ax, int ay, int az, int bx, int by, int bz) {
    WeBox b;
    b.x0 = ax < bx ? ax : bx; b.x1 = ax < bx ? bx : ax;
    b.y0 = ay < by ? ay : by; b.y1 = ay < by ? by : ay;
    b.z0 = az < bz ? az : bz; b.z1 = az < bz ? bz : az;
    return b;
}

/// A player's two selection points. `box()` is the **only** way to read them,
/// and it refuses an oversized selection — this is plan §0.5.7 defect 1. The cap
/// lives at the point of *reading* rather than in each command so that a command
/// added later cannot forget it: there is no other accessor to forget it with.
struct Selection {
    bool has1 = false, has2 = false;
    int  x1 = 0, y1 = 0, z1 = 0;
    int  x2 = 0, y2 = 0, z2 = 0;

    void set1(int x, int y, int z) { x1 = x; y1 = y; z1 = z; has1 = true; }
    void set2(int x, int y, int z) { x2 = x; y2 = y; z2 = z; has2 = true; }
    bool complete() const { return has1 && has2; }

    /// Fills `out` and returns true only when both points are set and the box is
    /// within `max_cells`. `volume` is always written when both points are set,
    /// so a caller can report the size it refused.
    bool box(WeBox& out, long long max_cells, long long& volume) const {
        volume = 0;
        if (!complete()) return false;
        out = we_make_box(x1, y1, z1, x2, y2, z2);
        volume = we_box_volume(out);
        return volume <= max_cells;
    }
};

// --- shape predicates --------------------------------------------------------
//
// Kept here so `//sphere` and `//hsphere` cannot drift apart, and so the
// off-by-one in a hollow shell is testable without a world.

inline bool we_in_sphere(int dx, int dy, int dz, int radius) {
    const long long d2 = (long long)dx * dx + (long long)dy * dy + (long long)dz * dz;
    return d2 <= (long long)radius * radius;
}

/// Hollow shell: inside the outer radius, outside the next one in.
inline bool we_in_sphere_shell(int dx, int dy, int dz, int radius) {
    const long long d2 = (long long)dx * dx + (long long)dy * dy + (long long)dz * dz;
    const long long inner = (long long)(radius - 1) * (radius - 1);
    return d2 <= (long long)radius * radius && d2 > inner;
}

inline bool we_in_disc(int dx, int dz, int radius) {
    const long long d2 = (long long)dx * dx + (long long)dz * dz;
    return d2 <= (long long)radius * radius;
}

inline bool we_in_ring(int dx, int dz, int radius) {
    const long long d2 = (long long)dx * dx + (long long)dz * dz;
    const long long inner = (long long)(radius - 1) * (radius - 1);
    return d2 <= (long long)radius * radius && d2 > inner;
}

// --- edit records, undo / redo ----------------------------------------------

/// One cell's before/after. 16 bytes, and the size is load-bearing: the undo
/// budget is expressed in bytes and `WE_UNDO_BUDGET_BYTES / sizeof(WeEdit)` is
/// how many edits a player may keep.
struct WeEdit {
    int           x, y, z;
    unsigned char oldType, oldColor, newType, newColor;
};

/// Reverse of a batch: what you apply to put the world back.
inline std::vector<WeEdit> we_invert(const std::vector<WeEdit>& in) {
    std::vector<WeEdit> out;
    out.reserve(in.size());
    for (const WeEdit& e : in)
        out.push_back({e.x, e.y, e.z, e.newType, e.newColor, e.oldType, e.oldColor});
    return out;
}

/// Per-player undo/redo, bounded by **bytes across both stacks**, evicting the
/// oldest batch first (plan §3.4, defect 5). One instance per connection, owned
/// by that connection's thread, so it carries no lock.
struct UndoStore {
    size_t budget_bytes = WE_UNDO_BUDGET_BYTES;
    std::deque<std::vector<WeEdit>> undo;
    std::deque<std::vector<WeEdit>> redo;

    static size_t batch_bytes(const std::vector<WeEdit>& b) {
        return b.size() * sizeof(WeEdit);
    }

    size_t bytes() const {
        size_t n = 0;
        for (const auto& b : undo) n += batch_bytes(b);
        for (const auto& b : redo) n += batch_bytes(b);
        return n;
    }

    /// Oldest-first eviction across undo then redo. A single batch larger than
    /// the whole budget is kept rather than dropped — it is already bounded by
    /// `WE_MAX_EDIT_CELLS`, and silently discarding the undo for the edit a
    /// player just made is worse than briefly exceeding the budget.
    void trim() {
        while (bytes() > budget_bytes) {
            if (undo.size() > 1) { undo.pop_front(); continue; }
            if (!redo.empty())   { redo.pop_front(); continue; }
            break;
        }
    }

    /// A fresh edit: pushed to undo, and the redo stack is dropped — the future
    /// it described no longer exists.
    void record(std::vector<WeEdit> batch) {
        if (batch.empty()) return;
        redo.clear();
        undo.push_back(std::move(batch));
        trim();
    }

    /// Move the newest undo batch to the redo stack and hand it back. The caller
    /// applies `we_invert(out)`.
    bool take_undo(std::vector<WeEdit>& out) {
        if (undo.empty()) return false;
        out = std::move(undo.back());
        undo.pop_back();
        redo.push_back(out);
        trim();
        return true;
    }

    /// Move the newest redo batch back to the undo stack and hand it back. The
    /// caller applies it forwards.
    bool take_redo(std::vector<WeEdit>& out) {
        if (redo.empty()) return false;
        out = std::move(redo.back());
        redo.pop_back();
        undo.push_back(out);
        trim();
        return true;
    }

    void clear() { undo.clear(); redo.clear(); }
};

// --- clipboard ---------------------------------------------------------------

/// A copied cell, stored relative to the player's feet at `//copy` time.
struct ClipCell {
    int           rx, ry, rz;
    unsigned char type, color;
};

/// Rotate a ramp/wedge block id one 90° step.
///
/// ⚠️ **Direction is unverified.** This is the modded patch's `(offset + 1) % 4`;
/// VuencEdit's clipboard rotation uses `(offset + 3) & 3`, and the two codebases'
/// orientation *names* for 24–27 and 40–55 also disagree (plan §0.5.1, §0.5.4).
/// One of the two is inverted and no capture settles it, so ramps in a rotated
/// paste may face the wrong way. Stage 3.6a owns the experiment that decides it
/// (it needs a retail client); flipping this is a one-constant change. The
/// *position* rotation below is not in doubt.
inline int we_rotate_slope(int type) {
    for (int start : {24, 28, 32, 36, 40, 44, 48, 52})
        if (type >= start && type <= start + 3)
            return start + ((type - start + 1) % 4);
    return type;
}

/// Rotate a clipboard `steps` × 90° clockwise about the player's own column.
/// `steps` is taken mod 4, so 0 is a no-op and the transform is a group action —
/// four steps return the original, whichever way `we_rotate_slope` turns out to
/// point.
inline void we_rotate_clipboard(std::vector<ClipCell>& clip, int steps) {
    steps = ((steps % 4) + 4) % 4;
    if (steps == 0) return;
    for (ClipCell& c : clip) {
        for (int i = 0; i < steps; ++i) {
            const int ox = c.rx, oz = c.rz;
            c.rx = -oz;
            c.rz = ox;
            c.type = (unsigned char)we_rotate_slope(c.type);
        }
    }
}

}  // namespace ewb
