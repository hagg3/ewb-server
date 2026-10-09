// Offline checks for log_fold.h (ROADMAP-SERVER 12.2).
#include <cstdio>
#include <string>
#include "log_fold.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main() {
    using ewb::EditLogFold;
    using ewb::isGriefRelevantEdit;

    // first edit in a window is written, the rest fold
    {
        EditLogFold f;
        CHECK(f.note(10.0, "Eve", "[Eve] MINE at (1,2,3)", false));
        CHECK(!f.note(10.1, "Eve", "[Eve] MINE at (1,2,4)", false));
        CHECK(!f.note(10.9, "Eve", "[Eve] MINE at (1,2,5)", false));
        CHECK(f.flush(10.5).empty());                       // window still open
        auto s = f.flush(11.0);
        CHECK(s.size() == 1);
        CHECK(s[0] == "[Eve] 2 more edit(s) in the last window (last: MINE at (1,2,5))");
        CHECK(f.flush(11.5).empty());                       // already reported
        CHECK(f.flush(12.1).empty());                       // nothing folded: forgotten
        CHECK(f.size() == 0);
    }
    // a new window after the old one closes writes again; players are independent
    {
        EditLogFold f;
        CHECK(f.note(0, "A", "[A] MINE at (0,0,0)", false));
        CHECK(f.note(0, "B", "[B] MINE at (0,0,0)", false));
        CHECK(f.note(2.0, "A", "[A] MINE at (0,0,1)", false));
        CHECK(!f.note(2.5, "A", "[A] MINE at (0,0,2)", false));
    }
    // never-fold edits pass straight through and do not disturb the window
    {
        EditLogFold f;
        CHECK(f.note(0, "A", "[A] MINE at (0,0,0)", false));
        CHECK(f.note(0.1, "A", "[A] BUILD at (1,1,1) type=9", true));
        CHECK(!f.note(0.2, "A", "[A] MINE at (0,0,1)", false));
        CHECK(f.flush(1.5).size() == 1);
    }
    // sustained stream: one summary per window, never two
    {
        EditLogFold f;
        int written = 0, summaries = 0;
        for (int i = 0; i < 100; ++i) {
            double t = i * 0.1;
            if (f.note(t, "A", "[A] MINE at (0,0,0)", false)) ++written;
            summaries += (int)f.flush(t).size();
        }
        CHECK(written <= 10);
        CHECK(summaries <= 10);
    }
    CHECK(isGriefRelevantEdit(2, 0));
    CHECK(isGriefRelevantEdit(0, 9) && isGriefRelevantEdit(0, 87) && isGriefRelevantEdit(0, 23));
    CHECK(isGriefRelevantEdit(0, 62) && isGriefRelevantEdit(0, 64) && isGriefRelevantEdit(0, 108));
    CHECK(isGriefRelevantEdit(0, 65) && isGriefRelevantEdit(0, 109));
    CHECK(!isGriefRelevantEdit(0, 1) && !isGriefRelevantEdit(1, 9) && !isGriefRelevantEdit(3, 9));

    if (fails) { std::printf("%d FAILED\n", fails); return 1; }
    std::printf("log_fold_test: all passed\n");
    return 0;
}
