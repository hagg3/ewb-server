// save_sched.h — when the autosave thread writes the world (ROADMAP-SERVER stage
// 12.0a, PERF-001). Pure and clock-free: the caller passes monotonic seconds, so
// `save_sched_test.cpp` checks the policy without threads or sleeps.
//
// ## Why this exists
//
// Before 12.0a every client handler ended with an inline `savePlayerPos();
// saveWorld(); saveSigns();` — for every socket, joined or not. While anyone was
// building, a bare TCP connect/close (no `JOIN`, no password) bought a full world
// save: a world-lock stall of ~0.1–0.3 s plus a whole-file rewrite and `fsync`.
// Ten bare connects measured eleven saves; twenty join/build/leave cycles measured
// twenty saves in four seconds.
//
// Now a departure only *requests* a save, and only for a session that joined. The
// autosave thread runs it, at most once per `min_gap` seconds however many players
// leave, and its periodic save still runs every `period` seconds. An operator's
// `save`/`stop` and the shutdown path still save synchronously; they never come
// through here.
//
// ## The rule
//
//   * a periodic save is due every `period` seconds (counted from the last one,
//     not from the last save of any kind, so `--idle-timeout`'s accounting, which
//     rides on this tick, stays exact);
//   * a requested save is due once `min_gap` has passed since the last save of any
//     kind;
//   * starting either save clears the request, so a request that arrives *while* a
//     save is running stays set and is honoured by the next one rather than lost.

#pragma once

namespace ewb {

/// Why the autosave thread is writing. Appended to the `Saved world` log line.
enum class SaveReason { None, Autosave, Departure };

inline const char* save_reason_name(SaveReason r) {
    switch (r) {
        case SaveReason::Autosave:  return "autosave";
        case SaveReason::Departure: return "departure";
        default:                    return "none";
    }
}

/// A connection's close asks for a save only if the session joined. A peer that
/// never sent a valid `JOIN` cannot have edited anything (stage 7.17's gate), so
/// its close has nothing to persist — and it is the unauthenticated half of PERF-001.
inline bool departure_requests_save(bool joined) { return joined; }

struct SaveSched {
    double period  = 15.0;   ///< periodic autosave interval, seconds
    double min_gap = 5.0;    ///< requested saves run at most once per this, seconds

    double last_save = -1e18; ///< start of the last save of any kind
    double next_tick = 0.0;   ///< when the next periodic save is due
    bool   requested = false;

    SaveSched() = default;
    SaveSched(double periodSec, double minGapSec, double now)
        : period(periodSec), min_gap(minGapSec), next_tick(now + periodSec) {}

    void request() { requested = true; }

    /// What is due at `now`, if anything. The periodic save wins a tie: it saves
    /// the same state and also drives the thread's other per-tick work.
    SaveReason due(double now) const {
        if (now >= next_tick) return SaveReason::Autosave;
        if (requested && now - last_save >= min_gap) return SaveReason::Departure;
        return SaveReason::None;
    }

    /// Seconds until something could be due (never negative). The thread sleeps
    /// this long or until a new request wakes it.
    double next_wake(double now) const {
        double at = next_tick;
        if (requested) {
            const double req = last_save + min_gap;
            if (req < at) at = req;
        }
        return at > now ? at - now : 0.0;
    }

    /// Call when a save of kind `r` starts (before writing), not when it ends: the
    /// request is consumed by the snapshot this save is about to take, so a new
    /// request during the write must survive it.
    void begin(double now, SaveReason r) {
        if (r == SaveReason::None) return;
        requested = false;
        last_save = now;
        if (r == SaveReason::Autosave) {
            next_tick += period;
            // A thread that overslept (a suspended VM, a long stall) runs one
            // catch-up save, not a burst of them.
            if (next_tick <= now) next_tick = now + period;
        }
    }
};

}  // namespace ewb
