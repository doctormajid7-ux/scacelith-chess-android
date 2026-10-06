// Android touch input: the event queue the Java thread fills and the gesture state machine the game
// thread drains (src/platform/touch_input.cpp). Kept apart from platform_android.cpp, and free of
// every Android header, so it can be unit-tested on a desktop (tests/touch_input_tests.cpp) -- the
// port cannot run on a device here, and this mapping is the part a player would notice most.
//
// The game is a first-person view driven by a mouse and a keyboard; this turns touches into exactly
// that state (see the mapping at the top of the implementation).
//
// Two mappings live side by side, chosen by the Android settings page (a button of the overlay):
// "touch" -- the finger is the pointer -- and "touchpad" (the default): the finger moves a visible
// arrow across the screen like a laptop's touchpad, one that never lifts the pointer as a finger
// crosses the screen. The pinch is still the wheel and the taps still reach the keyboard.
#pragma once
#include "platform.h"

#include <mutex>
#include <string>
#include <vector>

namespace plat {

// One event pushed by Java, drained by the game thread. Touch: 'action' is MotionEvent's own code,
// 0 down, 1 up, 2 move, 3 cancel, 5 pointer down, 6 pointer up; 'pointerCount' is how many pointers
// are down after the event.
struct TouchEvent {
    enum Kind { Touch, Key, Text, Wheel } kind = Touch;
    int action = 0;
    int pointerId = 0;
    int pointerCount = 0;
    float x = 0, y = 0;       // Touch: pixels, origin top-left
    int key = 0;              // Key: plat::Key
    bool down = false;        // Key
    std::string text;         // Text: UTF-8, may hold several characters at once (an IME result)
    float wheel = 0;          // Wheel: notches, + = lean towards the board
};

// The Java thread calls push(); the game thread calls update() once per frame. 'nowMs' is a
// monotonic clock in milliseconds: only the tap timing needs it, and injecting it is what makes a
// two- and a three-finger tap testable without waiting.
class TouchInput {
public:
    // "touch": the finger is the pointer (one finger = the left button, two = the look).
    // "touchpad": the finger moves a visible arrow without ever pressing, as on a laptop's
    // touchpad -- tap = click, two = the wheel, three = Escape. The touchpad is the default: it
    // keeps every widget of the game reachable until a direct mapping is tuned on a device.
    enum Mapping { Touch, Touchpad };
    void setMapping(Mapping m) { mapping_ = m; }
    Mapping mapping() const { return mapping_; }

    void setViewport(int width, int height);

    void push(const TouchEvent& e);              // Java UI thread
    void update(Input& io, double nowMs);        // game thread, once per frame

    // Diagnostics for the tests.
    int pointerCount() const { return downCount_; }
    int maxPointers() const { return maxPointers_; }
    float moved() const { return moved_; }
    bool sequenceActive() const { return sequenceActive_; }
    int queued() const;
    int deferred() const;

private:
    struct Pointer {
        int id = -1;
        float x = 0, y = 0;
        bool down = false;
    };

    static void setKey(Input& io, int key, bool down);
    static void setButton(Input& io, int button, bool down);
    static void tapKey(Input& io, int key);   // one short press, both edges in the frame it lands
    void updateTouchpad(Input& io, double nowMs);   // the touchpad mapping (a laptop's touchpad)
    float cursorGain() const;                 // arrow pixels per pixel of finger (the long side)
    float stillSlop() const;                  // movement (px) below which a finger counts as still
    Pointer* findPointer(int id);
    Pointer* freePointer();   // a slot whose finger has left, or a fresh one: a finger is never dropped
    void centroid(float& cx, float& cy, float& spread) const;
    void down(int id, float x, float y, double nowMs);
    void move(int id, float x, float y);
    void up(int id, float x, float y);
    void updateGestures(Input& io, double nowMs);

    int width_ = 0, height_ = 0;
    // One slot per finger down. Android reports every finger a hand puts down (ten and more), and
    // a finger the machine forgot would still send its release: that release must not be counted
    // as one finger leaving, or a two-finger look could read as a one-finger drag. The table grows
    // with the fingers, and the slots of fingers that left are reused.
    std::vector<Pointer> pointers_;
    int downCount_ = 0;
    int maxPointers_ = 0;
    float moved_ = 0;
    double sequenceStartMs_ = 0;
    bool leftDown_ = false;
    bool lookDown_ = false;
    float lastCentroidX_ = 0, lastCentroidY_ = 0, lastSpread_ = 0;
    bool haveCentroid_ = false;
    bool sequenceActive_ = false;

    Mapping mapping_ = Touchpad;
    // The touchpad's pointer (screen pixels), where the finger last put it. Hovering moves only
    // this: io.mouseX/Y follows it the frame after, so a look that reads the pointer directly
    // (the gaze drift, the widgets) sees the same arrow the game draws. The tap's reference point
    // is the finger's, not the pointer's.
    float cursorX_ = 0, cursorY_ = 0;
    bool haveCursor_ = false;
    bool touchpadLeftDown_ = false;   // the left button, held by a finger past the tap window
    bool tapPending_ = false;         // a tap's press went out: its release comes the next frame
    bool tapFromHold_ = false;        // this sequence already clicked through its held button
    bool sequenceStartIsFinger_ = true;  // the next one-finger frame resets the finger reference
    float seqFingerX_ = 0, seqFingerY_ = 0;

    // The event queues. push() is called from the Java thread, update() from the game thread, so
    // the shared vectors are behind a mutex (the pointers and the gesture state are only ever
    // touched by update()).
    mutable std::mutex mutex_;
    std::vector<TouchEvent> queue_;
    std::vector<TouchEvent> deferred_;
};

}  // namespace plat
