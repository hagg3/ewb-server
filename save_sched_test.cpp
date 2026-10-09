// save_sched_test.cpp — offline checks for ROADMAP-SERVER stage 12.0a: when the
// autosave thread writes the world (save_sched.h). Departures request a save and
// are coalesced to at most one per min_gap; the periodic save still fires with no
// requests; a request made during a save is honoured by the next one; a session
// that never joined requests nothing.
//
//   c++ -std=c++17 -O2 -Wall save_sched_test.cpp -o save_sched_test
//   ./save_sched_test

#include <cstdio>
#include <string>

#include "save_sched.h"

using namespace ewb;

static int g_fail = 0;

#define CHECK(cond, msg)                                                          \
    do {                                                                          \
        if (!(cond)) {                                                            \
            std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_fail;                                                             \
        }                                                                         \
    } while (0)

// Drive the scheduler the way the autosave thread does, in 10 ms steps, and count
// the saves of each kind. `requestAt` is called each step and may request.
template <class F>
static void run(SaveSched& s, double from, double to, int& autos, int& deps, F requestAt) {
    const long steps = (long)((to - from) * 100.0 + 0.5);   // integer steps: no float drift
    for (long i = 0; i < steps; ++i) {
        const double t = from + (double)i / 100.0;
        requestAt(s, t);
        const SaveReason r = s.due(t);
        if (r == SaveReason::Autosave)  ++autos;
        if (r == SaveReason::Departure) ++deps;
        s.begin(t, r);
    }
}

static void test_never_joined() {
    CHECK(!departure_requests_save(false), "a close that never joined requests nothing");
    CHECK(departure_requests_save(true), "a joined departure requests a save");
}

static void test_periodic_without_requests() {
    SaveSched s(15.0, 5.0, 0.0);
    CHECK(s.due(0.0) == SaveReason::None, "nothing due at start");
    CHECK(s.due(14.99) == SaveReason::None, "nothing due before the period");
    CHECK(s.due(15.0) == SaveReason::Autosave, "periodic save at the period");
    int a = 0, d = 0;
    SaveSched t(15.0, 5.0, 0.0);
    run(t, 0.0, 60.01, a, d, [](SaveSched&, double) {});
    CHECK(a == 4, "four periodic saves in 60 s");
    CHECK(d == 0, "no departure saves without requests");
}

static void test_coalescing() {
    // A departure every 200 ms for 10 s (50 leaves): at most one save per 5 s gap.
    SaveSched s(15.0, 5.0, 0.0);
    int a = 0, d = 0;
    run(s, 0.0, 10.0, a, d, [](SaveSched& ss, double t) {
        const long ms = (long)(t * 1000.0 + 0.5);
        if (ms % 200 == 0) ss.request();
    });
    CHECK(a == 0, "no periodic save inside the first period");
    CHECK(d >= 1 && d <= 3, "50 departures in 10 s coalesce to <= 3 saves");

    // The first request with no prior save is served at once.
    SaveSched f(15.0, 5.0, 0.0);
    f.request();
    CHECK(f.due(0.0) == SaveReason::Departure, "first departure saves immediately");
    f.begin(0.0, SaveReason::Departure);
    f.request();
    CHECK(f.due(4.99) == SaveReason::None, "second departure waits out the min gap");
    CHECK(f.due(5.0) == SaveReason::Departure, "...and saves once the gap has passed");
    CHECK(f.next_wake(1.0) > 3.99 && f.next_wake(1.0) < 4.01, "next_wake points at the gap's end");
}

static void test_request_during_save_not_lost() {
    SaveSched s(15.0, 5.0, 0.0);
    s.request();
    s.begin(1.0, s.due(1.0));     // a departure save starts at t=1
    CHECK(!s.requested, "starting the save consumes the request");
    s.request();                  // another player leaves mid-write
    CHECK(s.requested, "a request during the write is kept");
    CHECK(s.due(5.0) == SaveReason::None, "still inside the gap");
    CHECK(s.due(6.0) == SaveReason::Departure, "the next save picks it up");
}

static void test_autosave_consumes_request() {
    SaveSched s(15.0, 5.0, 0.0);
    s.begin(14.0, SaveReason::Departure);
    s.request();
    CHECK(s.due(15.0) == SaveReason::Autosave, "the periodic save wins a tie");
    s.begin(15.0, SaveReason::Autosave);
    CHECK(!s.requested, "...and saves the departure's state with it");
    CHECK(s.due(19.0) == SaveReason::None, "no extra departure save afterwards");
    CHECK(s.next_tick == 30.0, "the periodic tick stays on its grid");
}

static void test_next_wake() {
    SaveSched s(15.0, 5.0, 0.0);
    CHECK(s.next_wake(0.0) == 15.0, "idle: sleep to the period");
    CHECK(s.next_wake(20.0) == 0.0, "overdue: wake now");
    s.request();
    CHECK(s.next_wake(0.0) == 0.0, "a request with no recent save wakes now");
}

static void test_oversleep_catches_up_once() {
    SaveSched s(15.0, 5.0, 0.0);
    CHECK(s.due(100.0) == SaveReason::Autosave, "overdue tick is due");
    s.begin(100.0, SaveReason::Autosave);
    CHECK(s.due(100.01) == SaveReason::None, "one catch-up save, not a burst");
    CHECK(s.next_tick == 115.0, "the grid restarts from the catch-up");
}

static void test_reason_names() {
    CHECK(std::string(save_reason_name(SaveReason::Autosave)) == "autosave", "autosave name");
    CHECK(std::string(save_reason_name(SaveReason::Departure)) == "departure", "departure name");
}

int main() {
    test_never_joined();
    test_periodic_without_requests();
    test_coalescing();
    test_request_during_save_not_lost();
    test_autosave_consumes_request();
    test_next_wake();
    test_oversleep_catches_up_once();
    test_reason_names();
    if (g_fail) {
        std::fprintf(stderr, "\n%d check(s) FAILED\n", g_fail);
        return 1;
    }
    std::printf("save_sched_test: all checks passed\n");
    return 0;
}
