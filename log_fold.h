// log_fold.h — fold the --verbose per-edit log lines (ROADMAP-SERVER 12.2, PERF-012).
//
// With --verbose every terrain edit is a journal line. A player dragging a
// build tool makes dozens a second, which is what trips journald's rate limit
// and drops lines around them (the [Audit] ones too). EditLogFold writes the
// first edit a player makes in a window as the usual line and counts the rest,
// then emits one summary per closed window:
//
//     [name] 37 more edit(s) in the last window (last: MINE at (x,y,z))
//
// Edits that matter to a grief review are never folded (isGriefRelevant): any
// BURN, and a BUILD of TNT, lava or fireworks, so scripts/grief.sh still sees
// every one. No clock of its own — `now` is monotonic seconds, as in
// hardening.h. Unit-tested in log_fold_test.cpp.

#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ewb {

constexpr double EDIT_LOG_FOLD_WINDOW_SEC = 1.0;

/// BURN, and a BUILD of tnt (9, 87), lava (23, 62-64, 108) or fireworks (65, 109).
/// Mirrors the ids scripts/grief.sh looks for (eden_names.h).
inline bool isGriefRelevantEdit(int mode, int type) {
    if (mode == 2) return true;
    if (mode != 0) return false;
    return type == 9 || type == 87 || type == 23 || (type >= 62 && type <= 64) || type == 108 ||
           type == 65 || type == 109;
}

class EditLogFold {
public:
    explicit EditLogFold(double window = EDIT_LOG_FOLD_WINDOW_SEC) : window_(window) {}

    /// Record one edit whose normal log line is `line`. Returns true when the
    /// caller should write `line` now (the first in a window, or one that is
    /// never folded); false when it was folded into the open window.
    bool note(double now, const std::string& player, const std::string& line, bool neverFold) {
        if (neverFold) return true;
        Entry& e = entries_[player];
        if (e.open && now - e.start < window_) {
            ++e.folded;
            e.last = line;
            return false;
        }
        e.open = true;
        e.start = now;
        e.folded = 0;
        return true;
    }

    /// Summaries for windows that have closed with edits folded into them, as
    /// complete log lines. A player whose window closed with nothing folded is
    /// forgotten, so the table does not grow with every name ever seen.
    std::vector<std::string> flush(double now) {
        std::vector<std::string> out;
        for (auto it = entries_.begin(); it != entries_.end();) {
            Entry& e = it->second;
            if (now - e.start < window_) { ++it; continue; }
            if (e.folded == 0) { it = entries_.erase(it); continue; }
            out.push_back("[" + it->first + "] " + std::to_string(e.folded) +
                          " more edit(s) in the last window (last: " + stripName(it->first, e.last) + ")");
            e.folded = 0;
            e.start = now;   // the summary opens the next window: one line per window at most
            ++it;
        }
        return out;
    }

    size_t size() const { return entries_.size(); }

private:
    struct Entry {
        bool open = false;
        double start = 0;
        unsigned folded = 0;
        std::string last;
    };

    // "[name] MINE at (1,2,3)" -> "MINE at (1,2,3)"
    static std::string stripName(const std::string& player, const std::string& line) {
        const std::string pre = "[" + player + "] ";
        return line.compare(0, pre.size(), pre) == 0 ? line.substr(pre.size()) : line;
    }

    double window_;
    std::map<std::string, Entry> entries_;
};

}  // namespace ewb
