// zone_guard.h — the pure half of ROADMAP-SERVER stage 8.2: what the server does
// when a protected zone (zones.h) refuses a player's edit.
//
// zones.h answers "is this cell protected?". This header holds everything the
// server needs *after* the answer is yes that can be tested without a socket:
//
//   * zone_restore_wire — the `ACTION:server:0:…` lines that put one cell back on
//     a client's screen as the model holds it (the "visible undo");
//   * ZoneCellSet       — a deduplicated, capped set of cells to restore, which is
//     what bounds the restore a TNT blast can cause;
//   * RevertQueue       — delayed restores, coalesced per (client, cell), so a
//     player hammering a protected wall costs one restore per cell per delay;
//   * ZoneAuditAgg      — folds repeated denials into at most one audit line per
//     player, per zone, per window.
//
// No clock of its own: everything time-dependent takes `now` (monotonic seconds),
// the same rule hardening.h's token bucket follows. Unit-tested in protocol_test.cpp.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "base_profile.h"  // eden_default_profile — what an untouched cell looks like
#include "region_query.h"  // CELL_AIR, CELL_PAINTED_BASE, CELL_MAX_PAINT

namespace ewb {

// --- tunables (the server's defaults; see docs/configuration.md) -------------

/// `--zone-revert-delay-ms` default. ⚠️ A placeholder: whether a restore needs
/// to wait for the client's own local handling of the edit, and for how long, is
/// not yet measured against the retail client. 0 sends the restore at once.
constexpr int ZONE_REVERT_DEFAULT_DELAY_MS = 1000;
constexpr int ZONE_REVERT_MAX_DELAY_MS = 60000;

/// Most cells one burn's restore may carry. A single blast is ~923 cells and a
/// chain is budgeted by --burn-max-cells, so a chain across a zone could name
/// hundreds of thousands; past this the burst is truncated and the player told.
constexpr size_t ZONE_BURN_RESTORE_MAX = 16384;

/// Most restore cells waiting in a RevertQueue at once, across every client. A
/// restore past it is dropped (the server's copy is still right; the client
/// sees it on rejoin) rather than letting a flood of denials grow memory.
constexpr size_t ZONE_REVERT_MAX_PENDING = size_t(1) << 20;

/// One audit line per player per zone per this many seconds (plus a count).
constexpr double ZONE_AUDIT_WINDOW_SEC = 10.0;

// --- the restore wire --------------------------------------------------------

/// Pack a cell the way the server keys its world (x and z in 24 bits, y in 16),
/// for coalescing. Only equality matters here.
inline uint64_t zone_cell_key(int x, int y, int z) {
    return (((uint64_t)(uint32_t)x & 0xFFFFFFull) << 40) |
           (((uint64_t)(uint32_t)z & 0xFFFFFFull) << 16) |
            ((uint64_t)(uint32_t)y & 0xFFFFull);
}

struct RevertCell {
    int x = 0, y = 0, z = 0;
    bool operator==(const RevertCell& o) const { return x == o.x && y == o.y && z == o.z; }
};

/// Append the lines that make a client draw cell (x, y, z) as the model holds it,
/// **whatever the client currently shows there**. `present` is whether the model
/// stores the cell; an absent cell is the base profile's block at that height.
///
///   air (stored, or natural sky)   mine
///   a block                        mine -> build -> paint (if painted)
///   painted natural block (255)    mine -> build <natural block> -> paint
///
/// This differs from the relay shape (`we_emit_edit_wire`) in one place, on
/// purpose: a relay describes a change to a screen that still matches the model,
/// so a painted natural block is a lone paint there. A restore follows an edit the
/// client already drew and the server refused — the block may be gone from that
/// screen — so every non-air restore rebuilds the block before painting it.
/// The leading mine is what lets the build land: a build does not overwrite an
/// occupied cell on a client.
inline void zone_restore_wire(std::string& wire, int x, int y, int z, bool present, int type,
                              int color, const BaseProfile& base) {
    if (!present) {
        const BaseVoxel b = base.at(y);
        type = b.type;
        color = b.paint;
    } else if (type == CELL_PAINTED_BASE) {
        type = base.at(y).type;   // the natural block, keeping the stored paint
    }
    const bool paintable = color != 0 && color <= (int)CELL_MAX_PAINT;
    char line[96];
    wire.append(line, (size_t)std::snprintf(line, sizeof(line), "ACTION:server:0:%d:%d:%d:1\n", x, y, z));
    if (type == CELL_AIR) return;
    wire.append(line, (size_t)std::snprintf(line, sizeof(line), "ACTION:server:0:%d:%d:%d:0:%d\n", x, y, z, type));
    if (paintable)
        wire.append(line, (size_t)std::snprintf(line, sizeof(line), "ACTION:server:0:%d:%d:%d:3:%d\n",
                                                x, y, z, color));
}

/// Same, against the measured default terrain — the table the server's WorldEdit
/// path reads an untouched cell as (base_profile.h).
inline void zone_restore_wire(std::string& wire, int x, int y, int z, bool present, int type, int color) {
    static const BaseProfile profile = eden_default_profile();
    zone_restore_wire(wire, x, y, z, present, type, color, profile);
}

// --- a capped, deduplicated cell set -----------------------------------------

/// The cells one refusal will restore, in first-seen order. A TNT chain visits the
/// same cell many times (overlapping spheres), so dedup is what keeps its restore
/// to one entry per cell; the cap is what bounds it at all.
class ZoneCellSet {
public:
    explicit ZoneCellSet(size_t cap = ZONE_BURN_RESTORE_MAX) : cap_(cap) {}

    /// True if the cell was added. False if it was already here, or if the set is
    /// full — the latter is counted in overflow().
    bool add(int x, int y, int z) {
        const uint64_t k = zone_cell_key(x, y, z);
        if (seen_.count(k)) return false;
        if (cells_.size() >= cap_) { ++overflow_; return false; }
        seen_.insert(k);
        cells_.push_back({x, y, z});
        return true;
    }
    bool contains(int x, int y, int z) const { return seen_.count(zone_cell_key(x, y, z)) != 0; }
    const std::vector<RevertCell>& cells() const { return cells_; }
    size_t size() const { return cells_.size(); }
    bool empty() const { return cells_.empty(); }
    /// add() calls refused for the cap. Repeats of one refused cell count again.
    size_t overflow() const { return overflow_; }

private:
    size_t cap_;
    std::unordered_set<uint64_t> seen_;
    std::vector<RevertCell> cells_;
    size_t overflow_ = 0;
};

// --- delayed, coalesced restores ---------------------------------------------

/// A min-heap of restores by due time. `target` identifies who a restore is for
/// (the server uses a per-connection id, and one reserved id for "everyone");
/// `P` is whatever the caller needs to deliver it. The queue owns no thread and no
/// lock — the server wraps it in both.
///
/// Coalescing: a (target, cell) already waiting is not queued again. A player
/// mining one protected block a hundred times inside the delay gets one restore,
/// which is the point — the restore traffic a griefer can cause is bounded by the
/// cells they touch, not by how fast they click.
template <class P>
class RevertQueue {
public:
    struct Job {
        double due = 0;
        uint64_t seq = 0;
        uint64_t target = 0;
        P payload{};
        std::vector<RevertCell> cells;
    };

    explicit RevertQueue(size_t maxPending = ZONE_REVERT_MAX_PENDING) : maxPending_(maxPending) {}

    /// Queue `cells` for `target`, due at `due`. Returns how many were queued;
    /// cells already waiting for this target are skipped, and cells past the
    /// pending cap are dropped and counted in `*dropped`.
    size_t push(double due, uint64_t target, P payload, const std::vector<RevertCell>& cells,
                size_t* dropped = nullptr) {
        Job j;
        j.due = due;
        j.seq = seq_++;
        j.target = target;
        j.payload = std::move(payload);
        for (const RevertCell& c : cells) {
            const Key k{target, zone_cell_key(c.x, c.y, c.z)};
            if (pending_.count(k)) continue;
            if (pending_.size() >= maxPending_) { if (dropped) ++*dropped; continue; }
            pending_.insert(k);
            j.cells.push_back(c);
        }
        const size_t n = j.cells.size();
        if (n == 0) return 0;
        heap_.push_back(std::move(j));
        std::push_heap(heap_.begin(), heap_.end(), Later{});
        return n;
    }

    /// Take the earliest job if it is due by `now`. Its cells stop counting as
    /// pending, so a denial after this point queues a fresh restore.
    bool pop_due(double now, Job& out) {
        if (heap_.empty() || heap_.front().due > now) return false;
        std::pop_heap(heap_.begin(), heap_.end(), Later{});
        out = std::move(heap_.back());
        heap_.pop_back();
        for (const RevertCell& c : out.cells) pending_.erase(Key{out.target, zone_cell_key(c.x, c.y, c.z)});
        return true;
    }

    bool empty() const { return heap_.empty(); }
    size_t jobs() const { return heap_.size(); }
    size_t pending() const { return pending_.size(); }
    /// Due time of the earliest job. Only meaningful when !empty().
    double next_due() const { return heap_.empty() ? 0.0 : heap_.front().due; }

private:
    struct Key {
        uint64_t target, cell;
        bool operator==(const Key& o) const { return target == o.target && cell == o.cell; }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const {
            return std::hash<uint64_t>()(k.cell ^ (k.target * 0x9E3779B97F4A7C15ull));
        }
    };
    // Earliest due on top; equal due times leave in the order they were pushed.
    struct Later {
        bool operator()(const Job& a, const Job& b) const {
            return a.due != b.due ? a.due > b.due : a.seq > b.seq;
        }
    };
    size_t maxPending_;
    uint64_t seq_ = 0;
    std::vector<Job> heap_;
    std::unordered_set<Key, KeyHash> pending_;
};

// --- aggregated audit --------------------------------------------------------

/// Folds denials into at most one audit line per (player, zone) per window.
///
/// The first denial in a quiet (player, zone) pair is written at once, so the
/// audit trail shows when an attempt started. Denials inside the window after it
/// are counted, not written; flush() writes the count once the window closes (the
/// server calls it from its autosave tick) and opens a new window, so a sustained
/// attempt produces one line per window with the number folded into it. A spammer
/// cannot make the audit file grow faster than that.
class ZoneAuditAgg {
public:
    explicit ZoneAuditAgg(double window = ZONE_AUDIT_WINDOW_SEC) : window_(window) {}

    /// Record one denial. Returns true, with `line` set, when it should be written
    /// now; false when it was folded into the open window.
    bool note(double now, const std::string& player, const std::string& zone, const std::string& verb,
              int x, int y, int z, std::string& line) {
        Entry& e = entries_[{player, zone}];
        if (e.open && now - e.start < window_) {
            ++e.folded;
            e.verb = verb; e.x = x; e.y = y; e.z = z;
            return false;
        }
        line = "denied " + verb + " in zone " + zone + " at " + coords(x, y, z);
        if (e.folded) line += " (" + std::to_string(e.folded) + " more before this)";
        e = Entry{};
        e.open = true;
        e.start = now;
        return true;
    }

    /// Lines for windows that have closed with denials folded into them, as
    /// (player, line). A pair whose window closed with nothing folded is forgotten.
    std::vector<std::pair<std::string, std::string>> flush(double now) {
        std::vector<std::pair<std::string, std::string>> out;
        for (auto it = entries_.begin(); it != entries_.end();) {
            Entry& e = it->second;
            if (now - e.start < window_) { ++it; continue; }
            if (e.folded == 0) { it = entries_.erase(it); continue; }
            out.emplace_back(it->first.first,
                             "denied " + std::to_string(e.folded) + " more edit(s) in zone " +
                                 it->first.second + " (last: " + e.verb + " at " +
                                 coords(e.x, e.y, e.z) + ")");
            // The line just written opens a window of its own, so a steady stream
            // of denials still yields one line per window, never two.
            e.folded = 0;
            e.start = now;
            ++it;
        }
        return out;
    }

    size_t size() const { return entries_.size(); }

private:
    struct Entry {
        bool open = false;
        double start = 0;
        size_t folded = 0;
        std::string verb;
        int x = 0, y = 0, z = 0;
    };
    static std::string coords(int x, int y, int z) {
        return std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(z);
    }
    double window_;
    std::map<std::pair<std::string, std::string>, Entry> entries_;
};

}  // namespace ewb
