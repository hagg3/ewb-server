// out_queue_test.cpp — offline checks for ROADMAP-SERVER stages 7.3 / 7.4
// (per-client output serialisation).
//
//   clang++ -std=c++17 -O2 -Wall out_queue_test.cpp -lz -o out_queue_test
//   ./out_queue_test
//
// Covers, with no socket and no threads:
//   * the **no-split invariant** — every item a writer is handed is a whole line
//     or a whole frame, never a byte range of one. This is the property that makes
//     the "world resets in strips" splice unrepresentable.
//   * drain order: all of hi, then one lo item, then back to hi (movement is never
//     starved by a region burst, and hi never reorders the world-state stream)
//   * the world-state stream stays FIFO across blobs and region frames
//   * byte accounting for both queues, and the peak the `who` line reports
//   * hi overflow drops the *oldest* lines and counts them; the newest survives
//   * lo overflow refuses (never drops) and flags `over_budget`
//   * region-job admission, the empty-region `SNAPZ:0:` answer, first/last flags
//   * a whole region drains to exactly its input records, in order, once each
//   * drop_lo() releases world state and its accounting but keeps hi

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "out_queue.h"
#include "snapz_codec.h"

using ewb::OutItem;
using ewb::OutQueue;
using ewb::RegionJob;
using ewb::SnapRec;

static int g_fail = 0;

#define CHECK(cond, msg)                                                             \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__);    \
            ++g_fail;                                                                \
        }                                                                            \
    } while (0)

// A record vector with distinguishable contents, so a drained region can be
// compared against its input record for record.
static std::shared_ptr<const std::vector<SnapRec>> make_recs(size_t n) {
    auto v = std::make_shared<std::vector<SnapRec>>();
    v->reserve(n);
    for (size_t i = 0; i < n; ++i)
        v->push_back(SnapRec{(int32_t)(65500 + i % 97), (int32_t)(33 + i % 11),
                             (int32_t)(65400 + i / 97), 1, (int32_t)(1 + i % 5)});
    return v;
}

static RegionJob job_of(size_t n, size_t frame = 3000, bool emptyFrame = false) {
    RegionJob j;
    j.recs          = make_recs(n);
    j.frame_records = frame;
    j.empty_frame   = emptyFrame;
    return j;
}

// --- drain order and the no-split invariant ----------------------------------

static void test_priority_and_whole_items() {
    OutQueue q(OutQueue::Limits{1024, 4096, 4});

    CHECK(q.idle(), "a fresh queue is idle");
    OutItem it;
    CHECK(!q.take(it), "an idle queue hands out nothing");

    q.push_world("ACTION:server:0:1:2:3:1\n");
    q.push_hi("POS:a:17:1:2:3\n");
    q.push_hi("POS:b:17:4:5:6\n");
    CHECK(!q.idle(), "a queue with work is not idle");

    // hi first, in order, even though the world line was queued before them.
    CHECK(q.take(it) && it.kind == OutItem::Kind::Bytes, "first take is a hi line");
    CHECK(it.bytes == "POS:a:17:1:2:3\n", "hi drains in push order");
    CHECK(q.take(it) && it.bytes == "POS:b:17:4:5:6\n", "second hi line");
    CHECK(q.take(it) && it.bytes == "ACTION:server:0:1:2:3:1\n", "then the world line");
    CHECK(!q.take(it), "and then nothing");

    // Every item is a whole line: what comes out always ends in '\n' and never
    // starts mid-line. This is the splice the writer must be unable to produce.
    OutQueue q2;
    q2.push_hi("[Server] one\n");
    q2.push_world("ACTION:server:0:9:9:9:1\nACTION:server:0:9:9:9:0:5\n");
    size_t items = 0;
    while (q2.take(it)) {
        ++items;
        CHECK(it.kind == OutItem::Kind::Bytes, "no frames queued here");
        CHECK(!it.bytes.empty() && it.bytes.back() == '\n', "every item ends on a line boundary");
    }
    CHECK(items == 2, "two whole items, neither split");
}

static void test_hi_never_reorders_world_state() {
    // The ordering hazard the hi/lo split is designed around: a block edit must
    // not overtake the bulk reply it belongs after. Interleaving hi traffic
    // through a world-state stream must leave that stream in push order.
    OutQueue q(OutQueue::Limits{1 << 20, 1 << 20, 4});
    q.push_world("W1\n");
    q.push_reply("R1\n");
    q.push_hi("H1\n");
    q.push_world("W2\n");
    q.push_hi("H2\n");

    std::vector<std::string> world, hi;
    OutItem it;
    while (q.take(it)) {
        if (it.bytes[0] == 'H') hi.push_back(it.bytes);
        else                    world.push_back(it.bytes);
    }
    CHECK(hi.size() == 2 && hi[0] == "H1\n" && hi[1] == "H2\n", "hi lines keep their order");
    CHECK(world.size() == 3 && world[0] == "W1\n" && world[1] == "R1\n" && world[2] == "W2\n",
          "the world-state stream stays FIFO across blobs");
}

// --- byte accounting ----------------------------------------------------------

static void test_byte_accounting() {
    OutQueue q(OutQueue::Limits{1024, 4096, 2});
    CHECK(q.queued_bytes() == 0 && q.peak_bytes() == 0, "empty queue accounts for nothing");

    q.push_hi(std::string(100, 'a') + "\n");
    q.push_world(std::string(200, 'b') + "\n");
    CHECK(q.hi_bytes() == 101, "hi bytes counted");
    CHECK(q.lo_bytes() == 201, "lo bytes counted");
    CHECK(q.queued_bytes() == 302, "queued bytes is the sum");
    CHECK(q.peak_bytes() == 302, "peak tracks the high-water mark");

    OutItem it;
    q.take(it);
    CHECK(q.hi_bytes() == 0, "taking a hi line releases its bytes");
    q.take(it);
    CHECK(q.lo_bytes() == 0 && q.queued_bytes() == 0, "taking the blob releases its bytes");
    CHECK(q.peak_bytes() == 302, "peak is not reset by draining");

    // An empty push is a no-op rather than an empty queue entry: `emitEditBatch`
    // returns "" for a batch that changed nothing.
    CHECK(q.push_hi("") && q.push_world("") && q.push_reply(""), "empty pushes succeed");
    CHECK(q.idle(), "...and queue nothing");
}

// --- overflow -----------------------------------------------------------------

static void test_hi_overflow_drops_oldest() {
    OutQueue q(OutQueue::Limits{100, 4096, 2});
    for (int i = 0; i < 5; ++i) CHECK(q.push_hi(std::string(20, 'a' + i)), "under budget");
    CHECK(q.hi_bytes() == 100, "exactly at budget");
    CHECK(q.dropped_lines() == 0, "nothing dropped yet");

    CHECK(!q.push_hi(std::string(20, 'z')), "over budget reports the drop");
    CHECK(q.dropped_lines() == 1, "one line dropped");
    CHECK(q.dropped_bytes() == 20, "the dropped line's bytes are counted");
    CHECK(q.hi_bytes() == 100, "still within budget after dropping");

    // The oldest went; the newest is what a client actually wants.
    OutItem it;
    CHECK(q.take(it) && it.bytes[0] == 'b', "the oldest line was the one dropped");
    std::string last;
    while (q.take(it)) last = it.bytes;
    CHECK(last == std::string(20, 'z'), "the newest line survived");

    // A line bigger than the whole budget is still delivered rather than lost.
    OutQueue q2(OutQueue::Limits{16, 4096, 2});
    q2.push_hi(std::string(64, 'x'));
    CHECK(q2.take(it) && it.bytes.size() == 64, "an oversized hi line is still queued");
}

static void test_lo_overflow_refuses() {
    OutQueue q(OutQueue::Limits{1024, 100, 2});
    CHECK(q.push_world(std::string(60, 'a')), "first world blob fits");
    CHECK(!q.over_budget(), "not over budget yet");
    CHECK(q.push_world(std::string(60, 'b')), "a second fits: the backlog was still under budget");
    CHECK(q.lo_bytes() == 120, "the backlog is now over budget");

    // World state is never dropped: the push is refused so the caller disconnects.
    CHECK(!q.push_world(std::string(10, 'c')), "the next update is refused");
    CHECK(q.over_budget(), "over_budget tells the caller to disconnect");
    CHECK(q.lo_bytes() == 120, "the refused blob was not queued");

    // An indivisible update larger than the whole budget is still delivered: one
    // operator `fill` must not disconnect the entire server.
    OutQueue q3(OutQueue::Limits{1024, 100, 2});
    CHECK(q3.push_world(std::string(4096, 'f')), "an oversized single update is accepted");
    CHECK(!q3.over_budget(), "...and nobody is disconnected for it");
    OutItem big;
    CHECK(q3.take(big) && big.bytes.size() == 4096, "...and it arrives whole");

    // A request the client can re-ask is refused without the disconnect flag.
    OutQueue q2(OutQueue::Limits{1024, 100, 2});
    CHECK(q2.push_reply(std::string(60, 'a')), "first reply fits");
    CHECK(!q2.push_reply(std::string(60, 'b')), "a reply that would exceed the budget is refused");
    CHECK(!q2.over_budget(), "a refused reply does not disconnect anyone");
    CHECK(q2.lo_bytes() == 60, "the refused reply was not queued");
}

static void test_region_admission() {
    OutQueue q(OutQueue::Limits{1024, 4096, 2});
    CHECK(q.push_region(job_of(10)), "first region admitted");
    CHECK(q.push_region(job_of(10)), "second region admitted");
    CHECK(!q.push_region(job_of(10)), "third region refused at the job cap");
    CHECK(q.region_jobs() == 2, "two jobs in flight");
    CHECK(q.lo_bytes() == 0, "region jobs cost no queued bytes until they are encoded");

    OutItem it;
    while (q.take(it)) {}   // drain both
    CHECK(q.region_jobs() == 0, "jobs are released as they finish");
    CHECK(q.push_region(job_of(10)), "room again once they drained");

    // An empty region with no explicit answer is not queued at all.
    OutQueue q2;
    RegionJob none;
    none.recs = make_recs(0);
    CHECK(q2.push_region(none), "an empty, silent region is accepted");
    CHECK(q2.idle(), "...and queues nothing");
    CHECK(q2.region_jobs() == 0, "...and holds no job");

    // With --region-empty-frame (the default) it becomes exactly one frame.
    OutQueue q3;
    RegionJob empty;
    empty.recs        = make_recs(0);
    empty.empty_frame = true;
    CHECK(q3.push_region(std::move(empty)), "an empty region wanting a frame is queued");
    CHECK(q3.take(it), "it hands out one item");
    CHECK(it.kind == OutItem::Kind::Frame && it.count == 0, "a zero-record frame");
    CHECK(it.first && it.last, "which is both the first and the last of its job");
    CHECK(ewb::encode_item(it).rfind("SNAPZ:0:", 0) == 0, "and encodes to SNAPZ:0:");
    CHECK(!q3.take(it), "nothing follows it");
    CHECK(q3.region_jobs() == 0, "the job is released");
}

// --- a whole region, drained --------------------------------------------------

static void test_region_drains_exactly_once() {
    const size_t kRecs = 7001, kFrame = 3000;
    RegionJob j = job_of(kRecs, kFrame);
    auto expect = j.recs;

    OutQueue q(OutQueue::Limits{1024, 4096, 2});
    CHECK(q.push_region(std::move(j)), "region queued");

    std::vector<SnapRec> got;
    size_t frames = 0, firsts = 0, lasts = 0;
    OutItem it;
    while (q.take(it)) {
        CHECK(it.kind == OutItem::Kind::Frame, "a region drains as frames");
        CHECK(it.count > 0 && it.count <= kFrame, "no frame exceeds the split width");
        firsts += it.first ? 1 : 0;
        lasts  += it.last ? 1 : 0;
        ++frames;
        // Decode through the real wire path: what the writer would actually send.
        const std::string line = ewb::encode_item(it);
        CHECK(line.rfind("SNAPZ:", 0) == 0 && line.back() == '\n', "a whole SNAPZ line");
        const size_t c1 = line.find(':'), c2 = line.find(':', c1 + 1);
        CHECK(std::strtoul(line.substr(c1 + 1, c2 - c1 - 1).c_str(), nullptr, 10) == it.count,
              "the header count matches the payload");
        const std::vector<uint8_t> b = ewb::b64_decode(line.substr(c2 + 1, line.size() - c2 - 2));
        const std::vector<uint8_t> raw = ewb::raw_inflate(b.data(), b.size(), it.count * 20);
        CHECK(raw.size() == it.count * 20, "the payload inflates to exactly its records");
        for (size_t i = 0; i < it.count; ++i) {
            const uint8_t* p = raw.data() + i * 20;
            got.push_back(SnapRec{ewb::get_le_i32(p), ewb::get_le_i32(p + 4), ewb::get_le_i32(p + 8),
                                  ewb::get_le_i32(p + 12), ewb::get_le_i32(p + 16)});
        }
    }
    CHECK(frames == ewb::snapz_frame_count(kRecs), "frame count matches the split arithmetic");
    CHECK(firsts == 1 && lasts == 1, "exactly one first and one last frame per job");
    CHECK(got.size() == kRecs, "every record delivered exactly once");
    bool same = got.size() == expect->size();
    for (size_t i = 0; same && i < got.size(); ++i)
        same = got[i].x == (*expect)[i].x && got[i].y == (*expect)[i].y &&
               got[i].z == (*expect)[i].z && got[i].flag == (*expect)[i].flag &&
               got[i].type == (*expect)[i].type;
    CHECK(same, "records arrive in order, unaltered");
    std::printf("  %zu records -> %zu frame(s), all delivered in order\n", kRecs, frames);
}

static void test_region_interleaves_with_hi() {
    // A 17 MB region burst must not stop a player from seeing anyone move: hi
    // pushed *during* the burst is handed out before the burst's next frame.
    OutQueue q(OutQueue::Limits{1 << 20, 1 << 20, 2});
    q.push_region(job_of(9000, 3000));

    OutItem it;
    CHECK(q.take(it) && it.kind == OutItem::Kind::Frame && it.first, "frame 1");
    q.push_hi("POSVEL:mover:17:1:2:3:0:0:0\n");
    CHECK(q.take(it) && it.kind == OutItem::Kind::Bytes, "movement jumps the queue");
    CHECK(q.take(it) && it.kind == OutItem::Kind::Frame && !it.first && !it.last, "frame 2");
    CHECK(q.take(it) && it.kind == OutItem::Kind::Frame && it.last, "frame 3 ends the job");
    CHECK(!q.take(it), "burst complete");
}

// --- the close path -----------------------------------------------------------

static void test_drop_lo_keeps_hi() {
    OutQueue q(OutQueue::Limits{1024, 4096, 2});
    q.push_hi("[Server] Wrong password.\n");
    q.push_world(std::string(200, 'w'));
    q.push_region(job_of(9000));
    CHECK(q.region_jobs() == 1 && q.lo_bytes() == 200, "world state queued");

    q.drop_lo();
    CHECK(q.region_jobs() == 0, "pending regions released");
    CHECK(q.lo_bytes() == 0, "and their accounting with them");

    OutItem it;
    CHECK(q.take(it) && it.bytes == "[Server] Wrong password.\n",
          "a queued denial still goes out on the close path");
    CHECK(!q.take(it), "and nothing else does");
    CHECK(q.idle(), "the queue is drained");
}

int main() {
    test_priority_and_whole_items();
    test_hi_never_reorders_world_state();
    test_byte_accounting();
    test_hi_overflow_drops_oldest();
    test_lo_overflow_refuses();
    test_region_admission();
    test_region_drains_exactly_once();
    test_region_interleaves_with_hi();
    test_drop_lo_keeps_hi();
    if (g_fail) {
        std::fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
