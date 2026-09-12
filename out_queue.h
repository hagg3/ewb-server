// out_queue.h — per-client output queue policy: what a client's writer thread
// sends next, and what happens when it cannot keep up.
//
// ROADMAP-SERVER stages 7.3 / 7.4. Everything here is pure and header-only so the
// policy can be unit-tested offline (`out_queue_test.cpp`) with no socket and no
// threads; `server_posix.cpp` supplies the writer thread, the fd and the locking.
//
// ## Why this exists
//
// Before 7.3 nothing serialised writes to a client socket. `serveRegion()`
// streamed `SNAPZ` frames with no lock held while `broadcastMessage()`, `/msg`,
// `weSay()`, `weTeleport()` and the control socket's `ctlKick()` wrote to the same
// fd from other threads. A blocking `send()` larger than the available send buffer
// releases the CPU while it waits, so another thread's `POSVEL` landed *inside* a
// base64 payload: the frame arrived short or undecodable, and because records are
// sorted `(z, x, y, flag)` and framed at a flat 3000, one lost frame is one
// 1-block-wide row across the whole reply box — the "world resets in strips" bug.
//
// The fix is structural rather than a lock: **one writer thread per client owns
// every write to that fd**, producers only ever enqueue, and a queue entry is
// always a whole line or a whole frame. Interleaving becomes impossible to express
// rather than something callers must remember not to do.
//
// ## Two queues, and why a line goes in one rather than the other
//
//   hi — order-*insensitive*, latency-sensitive: `POS`/`VEL`/`POSVEL`, `PONG`,
//        chat, `[Server]` notices, the join line, `CAPS`, `SPAWN`.
//   lo — the ordered **world-state stream**: `ACTION` relays (single and batch),
//        `SIGNP` writes and bursts, `SNAPZ` region frames, the legacy snapshot.
//
// The split is about *ordering*, not just priority. Every line that changes what a
// client's world looks like shares one FIFO, so a block edit can never overtake
// the bulk reply it belongs after — a hazard the old code had for free from
// `clientsMutex` and one a naive "small lines first" priority queue would
// reintroduce. Movement and chat carry no world state, so letting them jump a
// multi-megabyte region burst costs nothing and is what keeps other players
// moving while one of them downloads terrain.
//
// Region replies sit in `lo` as *jobs* — the scanned, sorted record vector plus a
// cursor — and are encoded one frame at a time by the writer. That keeps the
// memory cost of an in-flight region at what it already was (the record vector)
// instead of adding a queue of encoded base64 on top, and it moves deflate off
// the client's own thread.
//
// ## Overflow
//
// A client that stops reading has to be bounded somewhere:
//
//   hi  full  -> drop the oldest lines and count them. They are movement and chat;
//                a `POS` from four seconds ago is worthless, and the alternative
//                (disconnecting) punishes exactly the weak-link players 7.3 is for.
//   lo  full  -> refuse. World state must never be silently dropped — that is the
//                bug class this file exists to kill — so the caller either refuses
//                a client-initiated request (`REGION`, `SIGNQ`: the client re-asks)
//                or disconnects the client (a broadcast relay: it cannot re-ask,
//                and a rejoin resyncs it properly).
//
// A client whose writer makes no progress at all is handled by the server's
// write timeout, not here: this header has no clock.

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "region_query.h"   // SnapRec, SNAPZ_FRAME_RECORDS, encode_snapz

namespace ewb {

// --- region jobs -------------------------------------------------------------

/// A `REGION` reply that has been scanned and sorted but not yet encoded.
///
/// `recs` is shared rather than owned so the client thread can hand it off and
/// return; the server gives it a deleter that maintains the global pending-record
/// counter, which is why this is a `shared_ptr` and not a `unique_ptr`.
struct RegionJob {
    std::shared_ptr<const std::vector<SnapRec>> recs;
    size_t cursor        = 0;                    ///< records already handed to the writer
    size_t frame_records = SNAPZ_FRAME_RECORDS;  ///< split width (the capture's flat 3000)
    /// Emit one `SNAPZ:0:` frame when `recs` is empty. An unbuilt region is
    /// answered with a real frame by default — see serveRegion()'s note.
    bool   empty_frame   = false;

    size_t total()     const { return recs ? recs->size() : 0; }
    size_t remaining() const { return total() - cursor; }
};

// --- what the writer is handed ----------------------------------------------

/// One unit of work. Either bytes that are already formatted (`Bytes`) or a slice
/// of a region job the writer still has to deflate (`Frame`). Deliberately not a
/// `std::string` in both cases: encoding a frame costs milliseconds and must
/// happen *outside* the client's queue lock.
struct OutItem {
    enum class Kind { None, Bytes, Frame };

    Kind kind = Kind::None;

    std::string bytes;                                  ///< Kind::Bytes — write as-is

    std::shared_ptr<const std::vector<SnapRec>> recs;   ///< Kind::Frame — encode [off, off+count)
    size_t off   = 0;
    size_t count = 0;
    bool   first = false;   ///< first frame of its job (start the drain timer)
    bool   last  = false;   ///< last frame of its job (log the completed reply)

    bool empty() const { return kind == Kind::None; }
    void reset() { kind = Kind::None; bytes.clear(); recs.reset(); off = count = 0; first = last = false; }
};

/// Encode a `Kind::Frame` item into its wire line. Needs zlib (via snapz_codec.h).
/// Separate from `OutQueue` on purpose: the writer calls this with no lock held.
inline std::string encode_item(const OutItem& it) {
    if (it.kind != OutItem::Kind::Frame) return std::string();
    if (!it.recs || it.count == 0) return encode_snapz(nullptr, 0);   // the `SNAPZ:0:` answer
    return encode_snapz(it.recs->data() + it.off, it.count);
}

// --- the queue ---------------------------------------------------------------

class OutQueue {
public:
    struct Limits {
        /// Movement + chat backlog before the oldest lines start being dropped.
        size_t hi_max_bytes = 1u << 20;
        /// Materialised world-state backlog (`ACTION` relays, sign bursts) a client
        /// may fall behind by before it is dropped. See `push_world` for why this
        /// bounds the backlog rather than each update.
        size_t lo_max_bytes = 16u << 20;
        /// Region replies that may be queued at once for one client.
        size_t max_region_jobs = 2;
    };

    OutQueue() = default;
    explicit OutQueue(Limits lim) : lim_(lim) {}

    void set_limits(Limits lim) { lim_ = lim; }
    const Limits& limits() const { return lim_; }

    // --- producers ------------------------------------------------------------

    /// Queue a latency-sensitive, order-insensitive line. Always accepted.
    /// Returns false when older lines had to be dropped to make room, so the
    /// caller can log it once; the new line is queued either way.
    ///
    /// A single line larger than the whole budget is still queued (hi lines are
    /// bounded by the chat/notice limits, so this is a misconfiguration, not a
    /// flood) — dropping it would lose the one line nobody else can resend.
    bool push_hi(std::string line) {
        if (line.empty()) return true;
        const size_t add = line.size();
        bool dropped = false;
        while (!hi_.empty() && hi_bytes_ + add > lim_.hi_max_bytes) {
            const size_t lost = hi_.front().size();
            hi_bytes_ -= lost;
            hi_.pop_front();
            ++dropped_lines_;
            dropped_bytes_ += lost;
            dropped = true;
        }
        hi_bytes_ += add;
        hi_.push_back(std::move(line));
        note_peak();
        return !dropped;
    }

    /// Queue world state that the client cannot ask for again (an `ACTION` relay,
    /// a `SIGNP` relay). False means the backlog is over budget: the caller must
    /// disconnect this client rather than drop the update, because a dropped relay
    /// silently diverges their world with nothing to resync it.
    ///
    /// The budget bounds the **backlog**, not any single update: one indivisible
    /// relay can legitimately be larger than the whole budget (a full `fill` is
    /// ~27 MB of `ACTION` lines), and refusing that would disconnect every player
    /// on the server the first time an operator ran one. So a client is only
    /// dropped once it is *already* over budget when the next update arrives,
    /// which bounds the queue at `lo_max_bytes` plus one update.
    bool push_world(std::string blob) {
        if (blob.empty()) return true;
        if (lo_bytes_ > lim_.lo_max_bytes) { over_budget_ = true; return false; }
        lo_bytes_ += blob.size();
        lo_.push_back(Slot{std::move(blob), RegionJob{}});
        note_peak();
        return true;
    }

    /// Queue the answer to a request the client made and can make again (the
    /// `SIGNQ` burst, the legacy snapshot). False means refuse the request; the
    /// client re-asks and nothing is lost.
    bool push_reply(std::string blob) {
        if (blob.empty()) return true;
        if (lo_bytes_ + blob.size() > lim_.lo_max_bytes) return false;
        lo_bytes_ += blob.size();
        lo_.push_back(Slot{std::move(blob), RegionJob{}});
        note_peak();
        return true;
    }

    /// Queue a `REGION` reply for lazy encoding. False means this client already
    /// has `max_region_jobs` in flight: refuse the request, the same answer the
    /// 750 ms gap limiter gives. Region jobs are bounded by count (and, in the
    /// server, by a global pending-record ceiling), not by `lo_max_bytes` — their
    /// bytes do not exist yet.
    bool push_region(RegionJob job) {
        if (region_jobs_ >= lim_.max_region_jobs) return false;
        if (job.total() == 0 && !job.empty_frame) return true;   // nothing to say
        if (job.frame_records == 0) job.frame_records = SNAPZ_FRAME_RECORDS;
        ++region_jobs_;
        lo_.push_back(Slot{std::string(), std::move(job)});
        return true;
    }

    // --- the writer -----------------------------------------------------------

    /// Hand the writer the next thing to send. False when nothing is pending.
    ///
    /// Drain order: everything in `hi`, then one `lo` frame or blob, then back to
    /// `hi`. A region burst therefore never starves movement updates, and the
    /// world-state stream keeps a full frame of bandwidth per turn.
    bool take(OutItem& out) {
        out.reset();
        if (!hi_.empty()) {
            out.kind  = OutItem::Kind::Bytes;
            out.bytes = std::move(hi_.front());
            hi_bytes_ -= out.bytes.size();
            hi_.pop_front();
            return true;
        }
        while (!lo_.empty()) {
            Slot& s = lo_.front();
            if (!s.blob.empty()) {
                out.kind  = OutItem::Kind::Bytes;
                out.bytes = std::move(s.blob);
                lo_bytes_ -= out.bytes.size();
                lo_.pop_front();
                return true;
            }
            RegionJob& j = s.job;
            if (j.total() == 0) {
                // An unbuilt region, answered with one explicit `SNAPZ:0:` frame.
                // `push_region` drops an empty job that did not ask for one, so
                // reaching here always means the frame is wanted.
                out.kind  = OutItem::Kind::Frame;
                out.count = 0;
                out.first = true;
                out.last  = true;
                pop_region();
                return true;
            }
            const size_t left = j.remaining();
            if (left == 0) { pop_region(); continue; }   // defensive: already drained
            const size_t n = left < j.frame_records ? left : j.frame_records;
            out.kind  = OutItem::Kind::Frame;
            out.recs  = j.recs;
            out.off   = j.cursor;
            out.count = n;
            out.first = (j.cursor == 0);
            j.cursor += n;
            out.last  = (j.remaining() == 0);
            if (out.last) pop_region();
            return true;
        }
        return false;
    }

    /// Nothing left to send.
    bool idle() const { return hi_.empty() && lo_.empty(); }

    /// Discard pending world state. Used on the close path: a client that is
    /// leaving has no use for a half-delivered region, and dropping it releases
    /// the record vectors (and their pending-record accounting) immediately.
    /// `hi` is kept so a queued denial or kick notice still goes out.
    void drop_lo() {
        lo_.clear();
        lo_bytes_    = 0;
        region_jobs_ = 0;
    }

    // --- accounting (for `who`, `region-stats` and the startup sizing note) ----

    size_t   hi_bytes()      const { return hi_bytes_; }
    size_t   lo_bytes()      const { return lo_bytes_; }
    size_t   queued_bytes()  const { return hi_bytes_ + lo_bytes_; }
    size_t   peak_bytes()    const { return peak_bytes_; }
    size_t   region_jobs()   const { return region_jobs_; }
    size_t   hi_lines()      const { return hi_.size(); }
    size_t   lo_items()      const { return lo_.size(); }
    uint64_t dropped_lines() const { return dropped_lines_; }
    uint64_t dropped_bytes() const { return dropped_bytes_; }
    /// A `push_world` has been refused: this client is being dropped as too slow.
    bool     over_budget()   const { return over_budget_; }

private:
    struct Slot {
        std::string blob;   ///< non-empty for a ready blob
        RegionJob   job;    ///< used when `blob` is empty
    };

    void pop_region() {
        if (region_jobs_) --region_jobs_;
        lo_.pop_front();
    }

    void note_peak() {
        const size_t q = hi_bytes_ + lo_bytes_;
        if (q > peak_bytes_) peak_bytes_ = q;
    }

    Limits lim_{};
    std::deque<std::string> hi_;
    std::deque<Slot>        lo_;
    size_t   hi_bytes_      = 0;
    size_t   lo_bytes_      = 0;
    size_t   region_jobs_   = 0;
    size_t   peak_bytes_    = 0;
    uint64_t dropped_lines_ = 0;
    uint64_t dropped_bytes_ = 0;
    bool     over_budget_   = false;
};

}  // namespace ewb
