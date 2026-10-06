// Tests for the Android touch input layer (src/platform/touch_input.cpp).
//
// The port cannot run on a device here (no /dev/kvm, x86-64 system images only), and this mapping
// is the part of it a player would notice first: it is what stands between a finger and the game's
// mouse-and-keyboard input. The module is free of every Android header for exactly this reason, so
// these tests drive it directly: push the events Java would push, run one frame, and read what the
// game's plat::Input got.
//
// What is under test is the whole translation: one finger as the left button, two as the right one
// plus the wheel and the look, the taps that stand in for Space and Escape, the tap that arrives
// whole inside one frame (the release waits for the next one, or the game never sees the click),
// the keys and text an IME sends, and what a cancelled gesture must not do.
#include "test.h"

#include "platform/touch_input.h"   // plat::TouchInput and plat::TouchEvent (pulls in platform.h)

#include <cmath>
#include <cstdint>
#include <string>

namespace {

// One game, one device. The clock is the test's, so a tap and a long press differ by a number and
// not by 350 ms of real waiting.
struct Fixture {
    Fixture() {
        touch.setViewport(1200, 800);   // landscape, as the manifest asks for
        // The tests below are the original mapping's (the finger is the pointer); the touchpad
        // tests set their own mapping.
        touch.setMapping(plat::TouchInput::Touch);
    }

    void frame() { touch.update(io, now); }

    void ev(int action, int id, float x, float y) {
        plat::TouchEvent e;
        e.kind = plat::TouchEvent::Touch;
        e.action = action;
        e.pointerId = id;
        e.x = x;
        e.y = y;
        touch.push(e);
    }
    void down(int id, float x, float y) { ev(0, id, x, y); }          // ACTION_DOWN
    void pointerDown(int id, float x, float y) { ev(5, id, x, y); }   // ACTION_POINTER_DOWN
    void moveTo(int id, float x, float y) { ev(2, id, x, y); }        // ACTION_MOVE
    void up(int id, float x, float y) { ev(1, id, x, y); }            // ACTION_UP
    void pointerUp(int id, float x, float y) { ev(6, id, x, y); }     // ACTION_POINTER_UP
    void cancel() { ev(3, 0, 0, 0); }                                 // ACTION_CANCEL

    void key(plat::Key k, bool isDown) {
        plat::TouchEvent e;
        e.kind = plat::TouchEvent::Key;
        e.key = k;
        e.down = isDown;
        touch.push(e);
    }
    void text(const std::string& s) {
        plat::TouchEvent e;
        e.kind = plat::TouchEvent::Text;
        e.text = s;
        touch.push(e);
    }
    void wheel(float notches) {
        plat::TouchEvent e;
        e.kind = plat::TouchEvent::Wheel;
        e.wheel = notches;
        touch.push(e);
    }

    plat::TouchInput touch;
    plat::Input io;
    double now = 1000.0;   // a monotonic clock in milliseconds, as the layer's is
};

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

bool anyKeyEdge(const plat::Input& io) {
    for (int k = 0; k < plat::KEY_COUNT; ++k)
        if (io.keyPressed[k] || io.keyReleased[k]) return true;
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// One finger: the left button
// ---------------------------------------------------------------------------------------------
TEST(touch_one_finger_is_the_left_button) {
    Fixture f;
    f.down(7, 100, 200);
    f.frame();
    CHECK(f.io.mousePressed[plat::MOUSE_LEFT]);   // pressing is what picks a piece up
    CHECK(f.io.mouseDown[plat::MOUSE_LEFT]);
    CHECK(!f.io.mousePressed[plat::MOUSE_RIGHT]);
    CHECK_EQ(f.io.mouseX, 100.0f);
    CHECK_EQ(f.io.mouseY, 200.0f);
    CHECK(!anyKeyEdge(f.io));   // a finger is the mouse, not the keyboard
    CHECK_EQ(f.touch.pointerCount(), 1);

    f.moveTo(7, 130, 210);   // carrying the piece
    f.frame();
    CHECK(near(f.io.mouseDX, 30.0f));
    CHECK(near(f.io.mouseDY, 10.0f));
    CHECK_EQ(f.io.mouseX, 130.0f);
    CHECK(!f.io.mousePressed[plat::MOUSE_LEFT]);   // the press edge was a frame ago
    CHECK(f.io.mouseDown[plat::MOUSE_LEFT]);

    f.up(7, 130, 210);
    f.frame();
    CHECK(f.io.mouseReleased[plat::MOUSE_LEFT]);   // and releasing is what plays it
    CHECK(!f.io.mouseDown[plat::MOUSE_LEFT]);
    CHECK_EQ(f.touch.pointerCount(), 0);
}

TEST(touch_pointer_actions_are_fingers_too) {
    Fixture f;
    f.down(1, 100, 100);
    f.frame();
    f.pointerDown(2, 200, 100);
    f.frame();
    CHECK_EQ(f.touch.pointerCount(), 2);
    f.pointerUp(2, 200, 100);
    f.frame();
    CHECK_EQ(f.touch.pointerCount(), 1);
    f.up(1, 100, 100);
    f.frame();
    CHECK_EQ(f.touch.pointerCount(), 0);
    CHECK(!f.io.mouseDown[plat::MOUSE_LEFT]);
    CHECK(!f.io.mouseDown[plat::MOUSE_RIGHT]);
}

TEST(touch_a_second_finger_hands_the_piece_back) {
    Fixture f;
    f.down(1, 100, 200);
    f.frame();
    CHECK(f.io.mouseDown[plat::MOUSE_LEFT]);

    f.down(2, 300, 200);   // the piece is dropped and the look starts, in one step
    f.frame();
    CHECK(f.io.mouseReleased[plat::MOUSE_LEFT]);
    CHECK(!f.io.mouseDown[plat::MOUSE_LEFT]);
    CHECK(f.io.mousePressed[plat::MOUSE_RIGHT]);
    CHECK(f.io.mouseDown[plat::MOUSE_RIGHT]);
    CHECK(!anyKeyEdge(f.io));   // and it is not a tap: the fingers are still down
    CHECK_EQ(f.touch.maxPointers(), 2);
}

TEST(touch_two_fingers_arriving_together_never_grabs) {
    Fixture f;
    // Both downs in the same frame: the gesture machine steps once per frame, so it only ever sees
    // two fingers, and a two-finger touch must not pick a piece up on its way to the look.
    f.down(1, 300, 400);
    f.down(2, 500, 400);
    f.frame();
    CHECK(!f.io.mousePressed[plat::MOUSE_LEFT]);
    CHECK(!f.io.mouseDown[plat::MOUSE_LEFT]);
    CHECK(f.io.mousePressed[plat::MOUSE_RIGHT]);
    CHECK_EQ(f.io.mouseX, 400.0f);   // the centroid
    CHECK_EQ(f.io.mouseY, 400.0f);
    CHECK_EQ(f.io.mouseDX, 0.0f);   // the first frame only takes the reference
    CHECK_EQ(f.io.wheel, 0.0f);

    f.moveTo(1, 320, 430);   // both fingers, same vector: the view turns
    f.moveTo(2, 520, 430);
    f.frame();
    CHECK(near(f.io.mouseDX, 20.0f));
    CHECK(near(f.io.mouseDY, 30.0f));
    CHECK_EQ(f.io.wheel, 0.0f);   // the grip kept its width: no lean
}

// ---------------------------------------------------------------------------------------------
// The pinch: the wheel
// ---------------------------------------------------------------------------------------------
TEST(touch_pinch_is_the_wheel) {
    Fixture f;
    f.down(1, 500, 400);
    f.down(2, 600, 400);
    f.frame();
    CHECK_EQ(f.io.wheel, 0.0f);   // a wider grip on the first frame is not a lean

    f.moveTo(2, 672, 400);   // 100 px apart becomes 172
    f.frame();
    // A notch per 6% of the window's long side here (1200 * 0.06 = 72 px).
    CHECK(near(f.io.wheel, 1.0f, 0.01f));

    f.moveTo(2, 600, 400);   // and closing the grip leans the other way
    f.frame();
    CHECK(near(f.io.wheel, -1.0f, 0.01f));
}

TEST(touch_pinch_follows_the_viewport) {
    Fixture f;
    f.touch.setViewport(320, 240);   // a smaller window: the same movement leans much further
    f.down(1, 100, 120);
    f.down(2, 200, 120);
    f.frame();
    f.moveTo(2, 272, 120);   // 100 px apart becomes 172, as above
    f.frame();
    // 320 * 0.06 = 19.2 px per notch, so the same 72 px of grip is nearly four notches.
    CHECK(near(f.io.wheel, 3.75f, 0.05f));
}

TEST(touch_three_fingers_are_not_a_pinch) {
    Fixture f;
    f.down(1, 500, 400);
    f.down(2, 600, 400);
    f.frame();
    f.down(3, 700, 400);
    f.frame();
    CHECK_EQ(f.touch.pointerCount(), 3);
    CHECK_EQ(f.touch.maxPointers(), 3);

    f.moveTo(2, 900, 400);   // the grip opens wide with a third finger down
    f.frame();
    CHECK_EQ(f.io.wheel, 0.0f);          // the wheel is a two-finger gesture
    CHECK(near(f.io.mouseDX, 100.0f));   // while the look still follows the centroid
}

// ---------------------------------------------------------------------------------------------
// Taps: Space, Escape, and the tap that arrives inside one frame
// ---------------------------------------------------------------------------------------------
TEST(touch_two_finger_tap_is_space) {
    Fixture f;
    // A whole tap inside one frame: the Java thread pushed all four events before the game thread
    // ran. The releases are held back -- otherwise the game's widgets, which act on the frame the
    // button goes up while reading mouseDown from that same frame, would never see the click.
    f.down(1, 300, 400);
    f.down(2, 500, 400);
    f.up(1, 300, 400);
    f.up(2, 500, 400);
    f.frame();
    CHECK_EQ(f.touch.deferred(), 2);
    CHECK(!anyKeyEdge(f.io));
    CHECK(f.io.mouseDown[plat::MOUSE_RIGHT]);   // as far as the game knows, the fingers are down

    f.now += 16.0;   // the next frame
    f.frame();
    CHECK(f.io.keyPressed[plat::KEY_SPACE]);   // the clock, both edges in one frame
    CHECK(f.io.keyReleased[plat::KEY_SPACE]);
    CHECK(!f.io.keyDown[plat::KEY_SPACE]);
    CHECK_EQ(f.touch.deferred(), 0);
    CHECK_EQ(f.touch.pointerCount(), 0);
    CHECK(!f.io.mouseDown[plat::MOUSE_RIGHT]);
}

TEST(touch_three_finger_tap_is_escape) {
    Fixture f;
    for (int i = 0; i < 3; ++i) f.down(i, 300.0f + i * 100.0f, 400);
    f.frame();
    CHECK_EQ(f.touch.deferred(), 0);   // nothing to hold back yet
    for (int i = 0; i < 3; ++i) f.up(i, 300.0f + i * 100.0f, 400);
    f.now += 16.0;
    f.frame();
    CHECK(f.io.keyPressed[plat::KEY_ESCAPE]);   // the menu
    CHECK(f.io.keyReleased[plat::KEY_ESCAPE]);
    CHECK(!f.io.keyPressed[plat::KEY_SPACE]);
}

TEST(touch_a_single_finger_tap_is_only_a_click) {
    Fixture f;
    f.down(1, 300, 400);
    f.up(1, 300, 400);   // whole inside one frame
    f.frame();
    CHECK(f.io.mousePressed[plat::MOUSE_LEFT]);
    CHECK_EQ(f.touch.deferred(), 1);

    f.now += 16.0;
    f.frame();
    CHECK(f.io.mouseReleased[plat::MOUSE_LEFT]);
    CHECK(!anyKeyEdge(f.io));   // Space and Escape are two- and three-finger gestures only
}

TEST(touch_a_held_press_is_not_a_tap) {
    Fixture f;
    f.down(1, 300, 400);
    f.down(2, 500, 400);
    f.frame();
    f.up(1, 300, 400);
    f.up(2, 500, 400);
    f.now += 400.0;   // longer than the tap window
    f.frame();
    CHECK(!anyKeyEdge(f.io));
    CHECK(!f.io.mouseDown[plat::MOUSE_RIGHT]);   // the fingers did leave
}

TEST(touch_a_dragged_press_is_not_a_tap) {
    Fixture f;
    f.down(1, 300, 400);
    f.down(2, 500, 400);
    f.frame();
    f.moveTo(1, 340, 400);   // 40 px, the two fingers together: a look, not a tap
    f.moveTo(2, 540, 400);
    f.frame();
    CHECK(near(f.io.mouseDX, 40.0f));
    f.up(1, 340, 400);
    f.up(2, 540, 400);
    f.now += 16.0;
    f.frame();
    CHECK(!anyKeyEdge(f.io));
    CHECK(f.touch.moved() >= 79.0f);   // 40 px twice, and no more
}

// ---------------------------------------------------------------------------------------------
// A cancelled gesture
// ---------------------------------------------------------------------------------------------
TEST(touch_a_cancelled_gesture_is_not_a_tap) {
    Fixture f;
    f.down(1, 300, 400);
    f.down(2, 500, 400);
    f.frame();
    CHECK(f.io.mouseDown[plat::MOUSE_RIGHT]);

    f.cancel();   // the system took the gesture over
    f.frame();
    CHECK_EQ(f.touch.pointerCount(), 0);
    CHECK(!f.io.mouseDown[plat::MOUSE_RIGHT]);
    CHECK(f.io.mouseReleased[plat::MOUSE_RIGHT]);
    // A cancelled gesture is not a tap: pressing the clock or opening the menu because the system
    // interrupted a touch would be a surprise, and the two edges above are what the game needs.
    CHECK(!anyKeyEdge(f.io));
    CHECK(!f.touch.sequenceActive());
}

// ---------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------
// The touchpad mapping (the default): the finger drives an arrow, a tap clicks
// ---------------------------------------------------------------------------------------------
TEST(touchpad_first_touch_places_the_arrow_without_moving_it) {
    Fixture f;
    f.touch.setViewport(1200, 800);
    f.touch.setMapping(plat::TouchInput::Touchpad);
    f.down(1, 100, 100);
    f.frame();
    // The arrow starts mid-screen (on the table) where the finger landed: no ghost movement.
    CHECK(near(f.io.mouseX, 600.0f));
    CHECK(near(f.io.mouseY, 496.0f));
    CHECK_EQ(f.io.mouseDX, 0.0f);
    CHECK_EQ(f.io.mouseDY, 0.0f);
    // The touchpad hovers: a finger down is not a press.
    CHECK(!f.io.mousePressed[plat::MOUSE_LEFT]);
    CHECK(!f.io.mouseDown[plat::MOUSE_LEFT]);
}

TEST(touchpad_finger_moves_the_arrow_faster_than_the_finger) {
    Fixture f;
    f.touch.setViewport(1200, 800);
    f.touch.setMapping(plat::TouchInput::Touchpad);
    f.down(1, 100, 100);
    f.frame();   // reference frame
    f.moveTo(1, 160, 130);   // 60 px of finger
    f.frame();
    // Gain = longSide/800 = 1.5: the arrow moves 90 px, the button stays up.
    CHECK(near(f.io.mouseX, 690.0f));
    CHECK(near(f.io.mouseY, 541.0f));
    CHECK(near(f.io.mouseDX, 90.0f));
    CHECK(near(f.io.mouseDY, 45.0f));
    CHECK(!f.io.mouseDown[plat::MOUSE_LEFT]);
}

TEST(touchpad_a_tap_clicks_and_does_not_stick) {
    Fixture f;
    f.touch.setViewport(1200, 800);
    f.touch.setMapping(plat::TouchInput::Touchpad);
    f.down(1, 500, 400);
    f.up(1, 500, 400);
    f.frame();   // down+up in one frame: the release is held back, the finger just hovers
    CHECK(!f.io.mousePressed[plat::MOUSE_LEFT]);
    CHECK_EQ(f.touch.deferred(), 1);

    f.now += 16.0;
    f.frame();   // the release lands: the tap clicks (the press edge, at the arrow)
    CHECK(f.io.mousePressed[plat::MOUSE_LEFT]);
    CHECK(f.io.mouseDown[plat::MOUSE_LEFT]);
    CHECK(!f.io.mouseReleased[plat::MOUSE_LEFT]);

    f.frame();   // the click's release, one frame later: the pair the game's widgets read
    CHECK(f.io.mouseReleased[plat::MOUSE_LEFT]);
    CHECK(!f.io.mouseDown[plat::MOUSE_LEFT]);

    f.frame();   // and the frame after is quiet again
    CHECK(!f.io.mousePressed[plat::MOUSE_LEFT]);
    CHECK(!f.io.mouseReleased[plat::MOUSE_LEFT]);
}

TEST(touchpad_a_held_finger_presses_and_drags) {
    Fixture f;
    f.touch.setViewport(1200, 800);
    f.touch.setMapping(plat::TouchInput::Touchpad);
    f.down(1, 500, 400);
    f.frame();   // hovering: the arrow is placed, nothing pressed
    CHECK(!f.io.mouseDown[plat::MOUSE_LEFT]);

    f.now += 400.0;   // the finger stays past the tap window: the button goes down (a piece
    f.frame();        // can be picked up, a slider dragged)
    CHECK(f.io.mousePressed[plat::MOUSE_LEFT]);
    CHECK(f.io.mouseDown[plat::MOUSE_LEFT]);

    f.moveTo(1, 560, 430);   // the moving hold: the Winlator drag, the arrow glides under it
    f.frame();
    CHECK(f.io.mouseDown[plat::MOUSE_LEFT]);
    CHECK(near(f.io.mouseDX, 90.0f));   // 60 px of finger at the 1.5 gain

    f.up(1, 560, 430);
    f.frame();   // the finger leaves: the button follows it
    CHECK(f.io.mouseReleased[plat::MOUSE_LEFT]);
    CHECK(!f.io.mouseDown[plat::MOUSE_LEFT]);
}

TEST(touchpad_a_slow_glide_never_presses) {
    // Bringing the arrow slowly over the board: the finger stays down well past the tap window
    // but keeps moving. It points; it must not pick up the piece the arrow passes over.
    Fixture f;
    f.touch.setMapping(plat::TouchInput::Touchpad);
    f.down(1, 300, 400);
    f.frame();
    for (int i = 1; i <= 40; ++i) {
        f.now += 25.0;   // a second of gliding, 5 px a frame
        f.moveTo(1, 300.0f + 5.0f * float(i), 400);
        f.frame();
        CHECK(!f.io.mouseDown[plat::MOUSE_LEFT]);
    }
    f.now += 500.0;   // and resting there afterwards does not press either: it was a glide
    f.frame();
    CHECK(!f.io.mouseDown[plat::MOUSE_LEFT]);
    f.up(1, 500, 400);
    f.now += 16.0;
    f.frame();   // nor does lifting the finger click
    CHECK(!f.io.mousePressed[plat::MOUSE_LEFT]);
    CHECK(!f.io.mouseReleased[plat::MOUSE_LEFT]);
}

TEST(touchpad_a_short_flick_is_not_a_tap) {
    // A quick little slide (well under the tap window) moves the arrow; it does not click.
    Fixture f;
    f.touch.setMapping(plat::TouchInput::Touchpad);
    f.down(1, 300, 400);
    f.frame();
    f.now += 60.0;
    f.moveTo(1, 330, 400);   // 30 px: more than a still finger wanders (12 px on this 1200 px screen)
    f.frame();
    f.up(1, 330, 400);
    f.now += 16.0;
    f.frame();
    CHECK(!f.io.mousePressed[plat::MOUSE_LEFT]);
}

TEST(touchpad_two_fingers_are_the_look) {
    Fixture f;
    f.touch.setViewport(1200, 800);
    f.touch.setMapping(plat::TouchInput::Touchpad);
    f.down(1, 300, 400);
    f.frame();   // hovering
    CHECK(!f.io.mouseDown[plat::MOUSE_RIGHT]);
    f.down(2, 500, 400);
    f.frame();   // the look takes over, the hover's press never happened
    CHECK(f.io.mousePressed[plat::MOUSE_RIGHT]);
    CHECK(!f.io.mouseDown[plat::MOUSE_LEFT]);
    f.moveTo(1, 320, 430);
    f.moveTo(2, 520, 430);
    f.frame();
    CHECK(near(f.io.mouseDX, 20.0f));
    CHECK(near(f.io.mouseDY, 30.0f));
    f.up(1, 320, 430);
    f.up(2, 520, 430);
    f.frame();   // the fingers leave: neither button sticks
    CHECK(!f.io.mouseDown[plat::MOUSE_LEFT]);
    CHECK(!f.io.mouseDown[plat::MOUSE_RIGHT]);
}

TEST(touchpad_three_finger_tap_is_escape) {
    Fixture f;
    f.touch.setMapping(plat::TouchInput::Touchpad);
    for (int i = 0; i < 3; ++i) f.down(i, 300.0f + i * 100.0f, 400);
    f.frame();   // the look takes the three fingers (the releases would be deferred)
    for (int i = 0; i < 3; ++i) f.up(i, 300.0f + i * 100.0f, 400);
    f.now += 16.0;
    f.frame();
    CHECK(f.io.keyPressed[plat::KEY_ESCAPE]);
    CHECK(!f.io.mouseDown[plat::MOUSE_RIGHT]);
}

TEST(touchpad_a_drag_is_not_a_click) {
    Fixture f;
    f.touch.setMapping(plat::TouchInput::Touchpad);
    f.down(1, 300, 400);
    f.now += 100.0;
    f.moveTo(1, 400, 400);   // a long, slow slide: pointing, not tapping
    f.frame();
    f.up(1, 400, 400);
    f.now += 16.0;
    f.frame();   // nothing but the movement
    CHECK(!f.io.mousePressed[plat::MOUSE_LEFT]);
    CHECK(!f.io.mouseReleased[plat::MOUSE_LEFT]);
    CHECK(!anyKeyEdge(f.io));
}

// ---------------------------------------------------------------------------------------------
// Bookkeeping: the pointer table, and a duplicate down
// ---------------------------------------------------------------------------------------------
TEST(touch_every_finger_gets_its_own_slot) {
    Fixture f;
    // Android reports every finger a hand puts down -- ten and more. The table grows, so none is
    // dropped: a dropped finger would still send its release, and a release the machine cannot
    // match to a finger must not count one down, or a two-finger look could read as a one-finger
    // drag (which grabs a piece) on the way back up.
    for (int i = 0; i < 10; ++i) f.down(i, 100.0f + i * 10.0f, 400);
    f.frame();
    CHECK_EQ(f.touch.pointerCount(), 10);
    CHECK_EQ(f.touch.maxPointers(), 10);

    // A release for a finger that never went down changes nothing: not a finger, not the count.
    f.up(99, 0, 0);
    f.frame();
    CHECK_EQ(f.touch.pointerCount(), 10);

    for (int i = 0; i < 10; ++i) f.up(i, 100.0f + i * 10.0f, 400);
    f.frame();
    CHECK_EQ(f.touch.pointerCount(), 0);
    CHECK_EQ(f.touch.maxPointers(), 10);
}

TEST(touch_a_repeated_down_is_not_a_second_finger) {
    Fixture f;
    f.down(1, 100, 100);
    f.frame();
    f.down(1, 100, 100);   // the same finger twice: ACTION_DOWN again for a pointer already down
    f.frame();
    CHECK_EQ(f.touch.pointerCount(), 1);
    CHECK_EQ(f.touch.maxPointers(), 1);
    // One finger, so the left button stays the left button: a duplicated down must not turn the
    // drag into a look.
    CHECK(f.io.mouseDown[plat::MOUSE_LEFT]);
    CHECK(!f.io.mouseDown[plat::MOUSE_RIGHT]);
}

// ---------------------------------------------------------------------------------------------
// The keys, the text and the wheel Java sends beside the touches
// ---------------------------------------------------------------------------------------------
TEST(touch_key_events_pass_through) {
    Fixture f;
    f.key(plat::KEY_TAB, true);
    f.frame();
    CHECK(f.io.keyDown[plat::KEY_TAB]);
    CHECK(f.io.keyPressed[plat::KEY_TAB]);
    CHECK(!f.io.keyReleased[plat::KEY_TAB]);

    f.key(plat::KEY_TAB, false);
    f.frame();
    CHECK(!f.io.keyDown[plat::KEY_TAB]);
    CHECK(f.io.keyReleased[plat::KEY_TAB]);
    CHECK(!f.io.keyPressed[plat::KEY_TAB]);

    f.key(plat::KEY_COUNT, true);   // out of range: dropped, not written out of bounds
    f.frame();
    CHECK(!anyKeyEdge(f.io));
}

TEST(touch_text_events_are_decoded) {
    Fixture f;
    f.text("\xC3\x89" "a\xF0\x9F\x91\x8D");   // É a 👍, as an IME hands them over
    f.frame();
    REQUIRE(f.io.textCount == 3);
    CHECK_EQ(f.io.text[0], 0xC9u);
    CHECK_EQ(f.io.text[1], 0x61u);
    CHECK_EQ(f.io.text[2], 0x1F44Du);

    // Control characters belong to the keys, and a truncated sequence at the end is skipped
    // rather than decoded into something else.
    f.text("\t\x01\xC3");
    f.frame();
    CHECK_EQ(f.io.textCount, 0);
}

TEST(touch_wheel_events_accumulate) {
    Fixture f;
    f.wheel(1.0f);
    f.wheel(0.5f);
    f.frame();
    CHECK(near(f.io.wheel, 1.5f));
    f.frame();
    CHECK_EQ(f.io.wheel, 0.0f);   // the wheel amount is per frame
}

TEST(touch_the_queue_drops_when_the_game_thread_is_wedged) {
    Fixture f;
    for (int i = 0; i < 600; ++i) f.key(plat::KEY_A, true);
    CHECK_EQ(f.touch.queued(), 513);   // the cap, and not one more
    f.frame();
    CHECK_EQ(f.touch.queued(), 0);
}

TEST(touch_the_edges_are_per_frame_and_the_held_state_is_not) {
    Fixture f;
    f.key(plat::KEY_TAB, true);
    f.wheel(2.0f);
    f.down(1, 100, 100);
    f.frame();
    CHECK(f.io.keyPressed[plat::KEY_TAB]);
    CHECK(f.io.keyDown[plat::KEY_TAB]);
    CHECK(f.io.mouseDown[plat::MOUSE_LEFT]);
    CHECK(near(f.io.wheel, 2.0f));

    f.frame();   // an empty frame
    CHECK(!f.io.keyPressed[plat::KEY_TAB]);
    CHECK(!f.io.keyReleased[plat::KEY_TAB]);
    CHECK(f.io.keyDown[plat::KEY_TAB]);   // what is held carries over
    CHECK(f.io.mouseDown[plat::MOUSE_LEFT]);
    CHECK_EQ(f.io.wheel, 0.0f);
    CHECK_EQ(f.io.mouseDX, 0.0f);
    CHECK_EQ(f.io.textCount, 0);
}
