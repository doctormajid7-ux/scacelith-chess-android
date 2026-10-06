// Tests for the seam between the two halves of the Android touch input.
//
// A touch crosses two languages, and neither half's tests can see the whole: GameView turns the
// framework's MotionEvents into (action, pointer id, position, how many fingers are down) and pushes
// them into the native layer (java/com/scacelith/game/GameView.java, platform_android.cpp), and
// plat::TouchInput turns them into the mouse and keyboard state the game was written against. The
// Java tests (android/app/src/test/java, under Robolectric) check the first half against what the
// native layer is *supposed* to be told, and tests/touch_input_tests.cpp checks the second against
// events a C++ test made up -- and neither can run the other's toolchain, so a mismatch between them
// is invisible to both.
//
// The two are joined by a file. TouchTraceTest drives random gestures through the real View and
// writes down what it sent, frame by frame; this replays that file through plat::TouchInput and
// checks what the game would have received:
//   * a finger drags the game's cursor by exactly the distance Java reported, and a two-finger drag
//     turns the view by the movement of the centroid -- from the trace's own coordinates;
//   * a pinch leans by the pixels-per-notch the *View's* reported size gives it;
//   * a tap is a click even when it arrived whole inside one frame (the native layer holds the
//     release back), and a two-finger tap is the clock, a three-finger one the menu;
//   * a cancelled gesture is not a tap;
//   * and no gesture of any shape leaves a piece held or a button down: the machine ends every
//     scenario neutral.
//
// The file is a build artefact of the Android unit tests: this test skips when it is not there (a
// desktop-only build), and the command that produces it is in docs/ANDROID.md.
#include "test.h"

#include "platform/touch_input.h"   // plat::TouchInput and plat::TouchEvent (pulls in platform.h)
#include "repo_files.h"             // readRepoFile: SCACELITH_SOURCE_DIR, the cwd, its parents

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

// ---------------------------------------------------------------------------------------------
// The trace, as TouchTraceTest writes it
// ---------------------------------------------------------------------------------------------
struct Line {
    enum Kind { Touch, Key, Wheel } kind = Touch;
    int action = 0, id = 0, count = 0, key = 0;   // Touch: the action, the pointer, how many are down
    bool down = false;                            // Key
    float x = 0, y = 0;                           // Touch: the pointer's position
    float wheel = 0;                              // Wheel: notches
};

struct Frame {
    std::vector<Line> lines;
};

struct Scenario {
    std::string kind;
    std::vector<Frame> frames;
};

struct Trace {
    int width = 0, height = 0;
    std::vector<Scenario> scenarios;
};

// A line the parser does not know is a failure of the pair (the Java test writes these lines and
// asserts its own output carries nothing else), never something to skip past.
bool parseTrace(const std::string& text, Trace& out, std::string& error) {
    Scenario* scenario = nullptr;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find('\n', start);
        const std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        start = end == std::string::npos ? text.size() + 1 : end + 1;
        if (line.empty() || line[0] == '#') continue;

        if (line.compare(0, 9, "viewport ") == 0) {
            if (std::sscanf(line.c_str(), "viewport %d %d", &out.width, &out.height) != 2) {
                error = "malformed viewport line: " + line;
                return false;
            }
            continue;
        }
        if (line.compare(0, 9, "scenario ") == 0) {
            out.scenarios.push_back(Scenario{line.substr(9), {}});
            scenario = &out.scenarios.back();
            scenario->frames.push_back(Frame{});
            continue;
        }
        if (!scenario) {
            error = "a line before the first scenario: " + line;
            return false;
        }
        if (line == "frame") {
            scenario->frames.push_back(Frame{});
            continue;
        }

        Line l;
        if (line.compare(0, 6, "touch ") == 0) {
            if (std::sscanf(line.c_str(), "touch %d %d %f %f %d", &l.action, &l.id, &l.x, &l.y, &l.count) != 5) {
                error = "malformed touch line: " + line;
                return false;
            }
            l.kind = Line::Touch;
        } else if (line.compare(0, 4, "key ") == 0) {
            char state[8] = {};
            if (std::sscanf(line.c_str(), "key %d %7s", &l.key, state) != 2) {
                error = "malformed key line: " + line;
                return false;
            }
            l.kind = Line::Key;
            l.down = std::string(state) == "down";
        } else if (line.compare(0, 6, "wheel ") == 0) {
            if (std::sscanf(line.c_str(), "wheel %f", &l.wheel) != 1) {
                error = "malformed wheel line: " + line;
                return false;
            }
            l.kind = Line::Wheel;
        } else {
            error = "a line the parser does not know: " + line;
            return false;
        }
        scenario->frames.back().lines.push_back(l);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// What the trace says the fingers are doing
// ---------------------------------------------------------------------------------------------
// The positions the events name, and which fingers are down: the file says everything the gestures
// need, so this side can work out where the centroid and the grip were without asking the machine it
// is checking. Its 'order' is the order the fingers arrived, which is the order the machine's slots
// hold them in too -- the generator never frees a slot and reuses it for a later finger.
struct Fingers {
    std::map<int, std::pair<float, float>> pos;
    std::vector<int> order;

    void apply(const Line& l) {
        if (l.kind != Line::Touch) return;
        pos[l.id] = {l.x, l.y};
        if (l.action == 0 || l.action == 5) {
            if (std::find(order.begin(), order.end(), l.id) == order.end()) order.push_back(l.id);
        } else if (l.action == 1 || l.action == 6) {
            order.erase(std::remove(order.begin(), order.end(), l.id), order.end());
        } else if (l.action == 3) {   // a cancelled gesture: every finger is gone
            order.clear();
            pos.clear();
        }
    }

    int count() const { return int(order.size()); }

    void centroid(float& cx, float& cy) const {
        float sx = 0, sy = 0;
        for (int id : order) {
            sx += pos.at(id).first;
            sy += pos.at(id).second;
        }
        const float n = float(order.size());
        cx = n > 0 ? sx / n : 0.0f;
        cy = n > 0 ? sy / n : 0.0f;
    }

    float spread() const {
        if (order.size() < 2) return 0.0f;
        const float ax = pos.at(order[0]).first, ay = pos.at(order[0]).second;
        const float bx = pos.at(order[1]).first, by = pos.at(order[1]).second;
        return std::sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by));
    }
};

void pushLine(plat::TouchInput& touch, const Line& l) {
    plat::TouchEvent e;
    if (l.kind == Line::Touch) {
        e.kind = plat::TouchEvent::Touch;
        e.action = l.action;
        e.pointerId = l.id;
        e.x = l.x;
        e.y = l.y;
        e.pointerCount = l.count;
    } else if (l.kind == Line::Key) {
        e.kind = plat::TouchEvent::Key;
        e.key = l.key;
        e.down = l.down;
    } else {
        e.kind = plat::TouchEvent::Wheel;
        e.wheel = l.wheel;
    }
    touch.push(e);
}

bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

// What one scenario made the game see, gathered frame by frame.
struct Totals {
    int leftPresses = 0, leftReleases = 0, rightPresses = 0, rightReleases = 0;
    int spaceTaps = 0, escapeTaps = 0;   // a frame with both edges of that key and neither held after
    bool anyKeyEdge = false;
    int leftPressFrame = -1, leftReleaseFrame = -1;
};

}  // namespace

TEST(touch_bridge_replays_what_the_view_sent) {
    std::string text;
    std::string foundAt;
    if (const char* path = std::getenv("SCACELITH_TOUCH_TRACE")) {
        if (!path[0]) path = nullptr;
        if (path) net::sys::readFile(path, text, size_t(8) << 20);
    }
    if (text.empty()) text = readRepoFile("build/android-touch-trace.txt", size_t(8) << 20, &foundAt);
    if (text.empty()) {
        SKIP("build/android-touch-trace.txt is not there: run the Android unit tests "
             "(TouchTraceTest writes it) or set SCACELITH_TOUCH_TRACE");
    }

    Trace trace;
    std::string error;
    REQUIRE(parseTrace(text, trace, error));
    REQUIRE(trace.scenarios.size() > 8);   // the generator writes thirteen kinds, several times each
    // The size the View reported to the native layer. The pinch's pixels-per-notch is 6% of the
    // longer side of it, so the exact wheel the oracle below checks is also a check that the number
    // the Java half reports is the one the C++ half uses.
    REQUIRE(trace.width > 0 && trace.height > 0);
    const float unit = std::max(240.0f, float(std::max(trace.width, trace.height))) * 0.06f;

    plat::TouchInput touch;
    // The trace's oracle was written for the finger-is-the-pointer mapping (the centroid of the
    // fingers is the pointer, the first finger presses); the touchpad's own behavior is unit
    // tested in touch_input_tests.cpp.
    touch.setMapping(plat::TouchInput::Touch);
    touch.setViewport(trace.width, trace.height);
    plat::Input io;
    double now = 1000.0;

    Fingers fingers;
    int prevCount = 0;
    float prevCx = 0, prevCy = 0, prevSpread = 0;

    for (const Scenario& s : trace.scenarios) {
        Totals seen;
        int frameIndex = 0;

        for (const Frame& f : s.frames) {
            float wheelLines = 0;
            for (const Line& l : f.lines) {
                pushLine(touch, l);
                if (l.kind == Line::Wheel) wheelLines += l.wheel;
            }
            for (const Line& l : f.lines) fingers.apply(l);

            now += 16.0;   // one 60 Hz frame: the clock the game thread would come back on
            touch.update(io, now);

            // ---- what the game was told, against what the trace says happened -------------------
            const int count = fingers.count();
            float cx = 0, cy = 0;
            fingers.centroid(cx, cy);
            const float spread = fingers.spread();

            // The machine takes its reference (what it measures the next movement against) whenever a
            // gesture starts, ends, or loses a finger, and on the frame a look begins: those frames
            // move nothing. Every other frame moves the cursor by the centroid's own movement. (The
            // one shape the generator never produces -- a third finger leaving while two stay -- would
            // need one more rule; it is not in the file.)
            float expectedDX = 0, expectedDY = 0;
            if (prevCount > 0 && count > 0 && count >= prevCount && !(count >= 2 && prevCount < 2)) {
                expectedDX = cx - prevCx;
                expectedDY = cy - prevCy;
            }
            // The lean: a notch per 'unit' pixels of grip change, and only with exactly two fingers
            // down on both frames (the wheel event of a real mouse comes through as it is).
            float expectedWheel = wheelLines;
            if (count == 2 && prevCount == 2) expectedWheel += (spread - prevSpread) / unit;

            CHECK(near(io.mouseDX, expectedDX, 0.01f));
            CHECK(near(io.mouseDY, expectedDY, 0.01f));
            CHECK(near(io.wheel, expectedWheel, 1e-4f));

            // ---- the invariants of the machine itself -------------------------------------------
            if (touch.pointerCount() >= 2) {
                // A second finger takes the piece out of the hand: a look never drags one.
                CHECK(!io.mouseDown[plat::MOUSE_LEFT]);
                CHECK(!io.mousePressed[plat::MOUSE_LEFT]);
            }
            if (touch.pointerCount() == 0) {
                CHECK(!io.mouseDown[plat::MOUSE_LEFT]);
                CHECK(!io.mouseDown[plat::MOUSE_RIGHT]);
            }

            if (io.mousePressed[plat::MOUSE_LEFT]) {
                ++seen.leftPresses;
                seen.leftPressFrame = frameIndex;
            }
            if (io.mouseReleased[plat::MOUSE_LEFT]) {
                ++seen.leftReleases;
                seen.leftReleaseFrame = frameIndex;
            }
            if (io.mousePressed[plat::MOUSE_RIGHT]) ++seen.rightPresses;
            if (io.mouseReleased[plat::MOUSE_RIGHT]) ++seen.rightReleases;
            // A tap of the clock or of the menu: both edges of the key inside one frame, nothing held
            // after -- which is what the game's widgets read a click from.
            if (io.keyPressed[plat::KEY_SPACE] && io.keyReleased[plat::KEY_SPACE] && !io.keyDown[plat::KEY_SPACE])
                ++seen.spaceTaps;
            if (io.keyPressed[plat::KEY_ESCAPE] && io.keyReleased[plat::KEY_ESCAPE] && !io.keyDown[plat::KEY_ESCAPE])
                ++seen.escapeTaps;
            if (io.keyPressed[plat::KEY_SPACE] || io.keyReleased[plat::KEY_SPACE] ||
                io.keyPressed[plat::KEY_ESCAPE] || io.keyReleased[plat::KEY_ESCAPE])
                seen.anyKeyEdge = true;

            prevCount = count;
            prevCx = cx;
            prevCy = cy;
            prevSpread = spread;
            ++frameIndex;
        }

        // ---- what the gesture was supposed to mean ----------------------------------------------
        const std::string& k = s.kind;
        if (k == "tap" || k == "tap_in_one_frame") {
            CHECK_EQ(seen.leftPresses, 1);        // pressing picks the piece up
            CHECK_EQ(seen.leftReleases, 1);       // releasing plays it
            CHECK(!seen.anyKeyEdge);              // one finger is the mouse, never the keyboard
            CHECK(!seen.rightPresses);
        }
        if (k == "tap_in_one_frame") {
            // The whole tap arrived inside one frame (the two lines above are in the same frame), and
            // the game still saw a click: the press and the release landed in different frames, which
            // is what the native layer's deferred release is for.
            CHECK(seen.leftPressFrame >= 0 && seen.leftReleaseFrame > seen.leftPressFrame);
        }
        if (k == "drag") {
            CHECK_EQ(seen.leftPresses, 1);
            CHECK_EQ(seen.leftReleases, 1);
            CHECK(!seen.rightPresses);
            CHECK(!seen.anyKeyEdge);
        }
        if (k == "look") {
            // Two fingers arriving together must never grab a piece, and letting both go at once must
            // not read as a tap.
            CHECK_EQ(seen.leftPresses, 0);
            CHECK_EQ(seen.rightPresses, 1);
            CHECK_EQ(seen.rightReleases, 1);
            CHECK(!seen.anyKeyEdge);
        }
        if (k == "look_lift_one") {
            // Here is what the game does today when a look ends one finger at a time: the finger left
            // behind is a drag, so the piece under the cursor is picked up and put back down where it
            // was released. The replay pins it rather than hiding it -- it is the shape that would
            // move a piece by accident, and a change to it should be deliberate.
            CHECK_EQ(seen.leftPresses, 1);
            CHECK_EQ(seen.leftReleases, 1);
            CHECK_EQ(seen.rightPresses, 1);
            CHECK_EQ(seen.rightReleases, 1);
            CHECK(!seen.anyKeyEdge);
        }
        if (k == "pinch_out" || k == "pinch_in") {
            CHECK_EQ(seen.leftPresses, 0);   // a pinch leans, it does not grab
            CHECK(!seen.anyKeyEdge);
        }
        if (k == "two_finger_tap") {
            CHECK_EQ(seen.spaceTaps, 1);    // the clock, both edges in the frame the tap lands on
            CHECK_EQ(seen.escapeTaps, 0);
            CHECK_EQ(seen.leftPresses, 0);
        }
        if (k == "three_finger_tap") {
            CHECK_EQ(seen.escapeTaps, 1);   // the menu
            CHECK_EQ(seen.spaceTaps, 0);
            CHECK_EQ(seen.leftPresses, 0);
        }
        if (k == "cancel") {
            // The system took the gesture over: no clock, no menu, whatever the fingers were doing.
            CHECK(!seen.anyKeyEdge);
        }
        if (k == "grab_then_look") {
            CHECK_EQ(seen.leftPresses, 1);    // picked up...
            CHECK_EQ(seen.leftReleases, 1);   // ...and handed back to the look
            CHECK_EQ(seen.rightPresses, 1);
            CHECK(!seen.anyKeyEdge);
        }
        if (k == "button") {
            // The overlay's clock button, pressed on the Java side: the key reaches the game with
            // both edges in one frame, as the widget that opens the clock needs.
            CHECK_EQ(seen.spaceTaps, 1);
            CHECK_EQ(seen.leftPresses, 0);
        }

        // Every gesture ends with the fingers gone and nothing held, whatever it was: a leak here is
        // the game stuck dragging a piece or holding the right button for the next frame onward.
        CHECK_EQ(touch.pointerCount(), 0);
        CHECK(!io.mouseDown[plat::MOUSE_LEFT]);
        CHECK(!io.mouseDown[plat::MOUSE_RIGHT]);
        CHECK(!io.keyDown[plat::KEY_SPACE]);
        CHECK(!io.keyDown[plat::KEY_ESCAPE]);
    }
}
