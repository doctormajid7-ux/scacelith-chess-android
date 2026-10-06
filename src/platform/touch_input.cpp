// The touch gesture state machine of the Android platform layer, moved out of platform_android.cpp
// (which keeps only the window, EGL and JNI wiring) so that it can be exercised by the unit tests.
// Nothing here is Android-specific: it reads plat::TouchEvent and writes plat::Input.
//
// The game is a first-person view driven by a mouse. Two mappings share this file:
//
// "touch" (the finger is the pointer):
//   1 finger           press / drag / release the left button (touch a piece, carry it, play it)
//   2 fingers, drag    hold the right button and move (look around: yaw / pitch)
//   2 fingers, pinch   the wheel (lean towards the board: fingers apart leans in)
//   2 fingers, tap     Space (press the chess clock)
//   3 fingers, tap     Escape (the menu)
//
// "touchpad" (the default; a laptop's touchpad, the way Winlator drives a mouse):
//   1 finger           moves the pointer (a visible arrow), nothing pressed
//   tap                a click of the left button (both edges one frame apart)
//   finger held still  the button stays down (hold to carry a piece, release to drop it)
//   2 fingers          the look: hold the right button and move
//   2 fingers, pinch   the wheel
//   2 fingers, tap     Space
//   3 fingers, tap     Escape
// The on-screen buttons of the Java overlay send the keys that have no gesture (Tab, C, S, Enter,
// Backspace) and show the software keyboard when a name has to be typed.
#include "touch_input.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace plat {

// The touchpad's pointer gain: how far the arrow moves for one pixel of finger. A comfortable
// swipe of the thumb (about 600 px of screen) should cross the screen, whatever the panel's
// resolution: the gain scales with the long side. The finger's travel and the arrow's travel are
// both in screen pixels, so the gain is the ratio of the two.
float TouchInput::cursorGain() const {
    const float longSide = float(std::max(width_, height_));
    return longSide > 0.0f ? longSide / 800.0f : 1.0f;
}

void TouchInput::setViewport(int width, int height) {
    width_ = width;
    height_ = height;
}

int TouchInput::queued() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return int(queue_.size());
}

int TouchInput::deferred() const { return int(deferred_.size()); }

void TouchInput::push(const TouchEvent& e) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (queue_.size() > 512) return;   // the game thread is wedged: drop rather than grow
    queue_.push_back(e);
}

void TouchInput::setKey(Input& io, int key, bool down) {
    if (key <= 0 || key >= KEY_COUNT) return;
    if (down) io.keyPressed[key] = true;
    else if (io.keyDown[key]) io.keyReleased[key] = true;
    io.keyDown[key] = down;
}

void TouchInput::setButton(Input& io, int button, bool down) {
    if (down && !io.mouseDown[button]) io.mousePressed[button] = true;
    if (!down && io.mouseDown[button]) io.mouseReleased[button] = true;
    io.mouseDown[button] = down;
}

void TouchInput::tapKey(Input& io, int key) {   // one short press, both edges in the frame it lands
    setKey(io, key, true);
    setKey(io, key, false);
}

TouchInput::Pointer* TouchInput::findPointer(int id) {
    for (Pointer& p : pointers_)
        if (p.down && p.id == id) return &p;
    return nullptr;
}

TouchInput::Pointer* TouchInput::freePointer() {
    for (Pointer& p : pointers_)
        if (!p.down) return &p;
    // Every slot is taken: grow rather than drop the finger. A dropped finger would still send its
    // release, and a release with no finger to give back would leave the count one too low.
    pointers_.push_back(Pointer{});
    return &pointers_.back();
}

void TouchInput::centroid(float& cx, float& cy, float& spread) const {
    float sx = 0, sy = 0;
    int n = 0;
    for (const Pointer& p : pointers_)
        if (p.down) {
            sx += p.x;
            sy += p.y;
            ++n;
        }
    cx = n ? sx / float(n) : 0.0f;
    cy = n ? sy / float(n) : 0.0f;
    spread = 0.0f;
    if (n >= 2) {   // distance between the two first pointers, enough for the pinch
        float ax = 0, ay = 0, bx = 0, by = 0;
        int seen = 0;
        for (const Pointer& p : pointers_)
            if (p.down) {
                if (seen == 0) { ax = p.x; ay = p.y; }
                else if (seen == 1) { bx = p.x; by = p.y; }
                ++seen;
            }
        spread = std::sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by));
    }
}

void TouchInput::down(int id, float x, float y, double nowMs) {
    Pointer* p = findPointer(id);
    const bool duplicate = p != nullptr;   // ACTION_DOWN again for a pointer already down: one
                                           // finger, and counting it twice would turn a drag into
                                           // a two-finger look
    if (!p) p = freePointer();   // a free slot, or a fresh one: never null
    if (!sequenceActive_) {
        sequenceActive_ = true;
        maxPointers_ = 0;
        moved_ = 0;
        sequenceStartMs_ = nowMs;
        tapFromHold_ = false;   // the touchpad's held-button marker, one sequence at a time
    }
    p->id = id;
    p->x = x;
    p->y = y;
    p->down = true;
    if (!duplicate) ++downCount_;
    maxPointers_ = std::max(maxPointers_, downCount_);
}

void TouchInput::move(int id, float x, float y) {
    Pointer* p = findPointer(id);
    if (!p) return;
    moved_ += std::abs(x - p->x) + std::abs(y - p->y);
    p->x = x;
    p->y = y;
}

void TouchInput::up(int id, float x, float y) {
    Pointer* p = findPointer(id);
    if (!p) return;   // a release for a finger that never went down: nothing to give back, and it
                      // must not count one down (the count is the state the gestures read)
    moved_ += std::abs(x - p->x) + std::abs(y - p->y);
    p->x = x;
    p->y = y;
    p->down = false;
    downCount_ = std::max(0, downCount_ - 1);
}

// One gesture step of the touchpad mapping, run on the game thread once the pending events are
// drained. The pointer is an arrow the finger drives like a touchpad: it moves relative to where
// the finger is, faster than it (the whole screen in a thumb's swipe). A finger down presses the
// left button for as long as it stays down -- a tap is a click, a hold carries a piece, and a
// moving hold is the Winlator drag: the arrow glides under the finger while the button is down.
// Two fingers are the look, the pinch is the wheel, and a quick tap of two or three fingers is
// Space or Escape. The widget hit-test of the game reads the pointer's position (mouseX/Y, and
// gameCursor draws the arrow there); where the finger is stays inside this class (the two-finger
// look's centroid), so the two never disagree.
// How far a finger may wander and still count as still (a tap, a long press): a few millimetres
// of a phone screen, whatever its pixel density (the long side of a phone is ~15 cm).
float TouchInput::stillSlop() const { return std::max(12.0f, 0.012f * float(std::max(width_, height_))); }

void TouchInput::updateTouchpad(Input& io, double nowMs) {
    if (tapPending_) {   // the frame after a tap's press: its release (the game sees the two
        setButton(io, MOUSE_LEFT, false);   // edges on two frames, like a mouse click)
        tapPending_ = false;
    }
    if (downCount_ == 1) {
        float px = 0, py = 0;
        for (const Pointer& p : pointers_)
            if (p.down) { px = p.x; py = p.y; break; }   // exactly one: downCount_ == 1
        bool jumped = false;
        if (!haveCursor_) {   // the very first touch: the pointer starts mid-screen, on the table
            cursorX_ = 0.5f * std::max(1, width_);
            cursorY_ = 0.62f * std::max(1, height_);
            haveCursor_ = true;
            jumped = true;
        }
        if (sequenceStartIsFinger_) {
            seqFingerX_ = px;                 // this frame only takes the references
            seqFingerY_ = py;
            sequenceStartIsFinger_ = false;
        } else {
            const float ax = px - seqFingerX_, ay = py - seqFingerY_;   // finger distance
            cursorX_ += ax * cursorGain();
            cursorY_ += ay * cursorGain();
            seqFingerX_ = px;                 // the gain reads a per-frame delta, not the
            seqFingerY_ = py;                 // distance from where the finger came down
        }
        cursorX_ = std::clamp(cursorX_, 0.0f, float(std::max(1, width_) - 1));
        cursorY_ = std::clamp(cursorY_, 0.0f, float(std::max(1, height_) - 1));
        if (!jumped) {   // the landing frame only places the arrow: no delta (the game's widgets
                         // and the gaze drift would read a move the finger did not make)
            io.mouseDX += cursorX_ - io.mouseX;
            io.mouseDY += cursorY_ - io.mouseY;
        }
        io.mouseX = cursorX_;
        io.mouseY = cursorY_;
        // The finger points, it does not press: gliding never clicks (a press would act on
        // whatever sits under the arrow, not under the finger). A finger that stays *still* past
        // the tap window holds the button -- carry a piece, drag a slider -- until it leaves; one
        // that has started to glide is pointing, however long it stays (a slow approach of the
        // arrow to a square must not pick up the piece it passes over).
        if (!touchpadLeftDown_ && !tapFromHold_ && nowMs - sequenceStartMs_ >= 350.0 && moved_ < stillSlop()) {
            setButton(io, MOUSE_LEFT, true);
            touchpadLeftDown_ = true;
            tapFromHold_ = true;
        }
    } else if (downCount_ >= 2) {
        if (touchpadLeftDown_) {   // the second finger releases the click before the look
            setButton(io, MOUSE_LEFT, false);
            touchpadLeftDown_ = false;
        }
        float cx = 0, cy = 0, spread = 0;
        centroid(cx, cy, spread);
        if (!lookDown_) {
            lookDown_ = true;
            setButton(io, MOUSE_RIGHT, true);
            io.mouseX = cx;   // the look reads deltas; its reference is the centroid
            io.mouseY = cy;
            lastCentroidX_ = cx;
            lastCentroidY_ = cy;
            lastSpread_ = spread;
            haveCentroid_ = true;
            return;
        }
        if (haveCentroid_) {
            io.mouseDX += cx - lastCentroidX_;
            io.mouseDY += cy - lastCentroidY_;
            const float unit = std::max(240.0f, float(std::max(width_, height_))) * 0.06f;
            if (downCount_ == 2 && unit > 0.0f) io.wheel += (spread - lastSpread_) / unit;
        }
        io.mouseX = cx;
        io.mouseY = cy;
        lastCentroidX_ = cx;
        lastCentroidY_ = cy;
        lastSpread_ = spread;
        haveCentroid_ = true;
        // The arrow keeps its place through a look: the next one-finger frame reports the jump
        // back to it as a delta (that is where the visible arrow is).
    } else {
        if (touchpadLeftDown_) {
            setButton(io, MOUSE_LEFT, false);
            touchpadLeftDown_ = false;
        }
        if (lookDown_) {
            setButton(io, MOUSE_RIGHT, false);
            lookDown_ = false;
        }
        haveCentroid_ = false;
        if (sequenceActive_) {
            const double ms = nowMs - sequenceStartMs_;
            // A quick, still single finger is a tap: its click goes out now, at the arrow (the
            // touchpad's pointer -- the finger's place is its own), and the release follows the
            // frame after. Only the keyboard taps are recognized for two and three fingers
            // (the clock, the menu); a one-finger sequence that already held the button clicked
            // through it.
            if (ms < 350.0 && moved_ < (maxPointers_ == 1 ? stillSlop() : 40.0f)) {
                if (maxPointers_ == 1) {
                    if (!tapFromHold_) {
                        if (!haveCursor_) {   // a tap without a hover before it: the arrow starts
                            cursorX_ = 0.5f * std::max(1, width_);   // at its rest place
                            cursorY_ = 0.62f * std::max(1, height_);
                            haveCursor_ = true;
                        }
                        io.mouseX = cursorX_;   // the click lands where the arrow is (a look may
                        io.mouseY = cursorY_;   // have left the reported pointer elsewhere)
                        setButton(io, MOUSE_LEFT, true);
                        tapPending_ = true;
                    }
                } else if (maxPointers_ == 2) {
                    tapKey(io, KEY_SPACE);
                } else if (maxPointers_ >= 3) {
                    tapKey(io, KEY_ESCAPE);
                }
            }
            sequenceActive_ = false;
        }
        sequenceStartIsFinger_ = true;   // the next one-finger frame is its own reference
    }
}

// One gesture step of the finger-is-the-pointer mapping, run on the game thread once the pending
// events are drained.
void TouchInput::updateGestures(Input& io, double nowMs) {
    float cx = 0, cy = 0, spread = 0;
    centroid(cx, cy, spread);
    if (downCount_ == 1) {
        if (lookDown_) {
            setButton(io, MOUSE_RIGHT, false);
            lookDown_ = false;
        }
        if (!leftDown_) {
            setButton(io, MOUSE_LEFT, true);
            leftDown_ = true;
            io.mouseX = cx;
            io.mouseY = cy;
        } else {
            io.mouseDX += cx - io.mouseX;
            io.mouseDY += cy - io.mouseY;
            io.mouseX = cx;
            io.mouseY = cy;
        }
        haveCentroid_ = false;
    } else if (downCount_ >= 2) {
        if (leftDown_) {   // a second finger takes the piece out of the hand: the drag ends here
            setButton(io, MOUSE_LEFT, false);
            leftDown_ = false;
        }
        if (!lookDown_) {
            lookDown_ = true;
            setButton(io, MOUSE_RIGHT, true);
            io.mouseX = cx;
            io.mouseY = cy;
        }
        if (haveCentroid_) {
            io.mouseDX += cx - lastCentroidX_;
            io.mouseDY += cy - lastCentroidY_;
            // The pinch is a wheel notch per 6% of the window height: about 20 notches from the
            // closest grip to the widest one, a full lean in a comfortable movement.
            const float unit = std::max(240.0f, float(std::max(width_, height_))) * 0.06f;
            if (downCount_ == 2 && unit > 0.0f) io.wheel += (spread - lastSpread_) / unit;
        }
        io.mouseX = cx;
        io.mouseY = cy;
        lastCentroidX_ = cx;
        lastCentroidY_ = cy;
        lastSpread_ = spread;
        haveCentroid_ = true;
    } else {
        if (leftDown_) {
            setButton(io, MOUSE_LEFT, false);
            leftDown_ = false;
        }
        if (lookDown_) {
            setButton(io, MOUSE_RIGHT, false);
            lookDown_ = false;
        }
        haveCentroid_ = false;
        // A tap: the fingers came down and left again quickly without moving (a drag of two
        // fingers is a look, a drag of one is a piece). 2 fingers press the clock, 3 open the
        // menu, exactly like Space and Escape at the table.
        if (sequenceActive_) {
            const double ms = nowMs - sequenceStartMs_;
            if (ms < 350.0 && moved_ < 40.0f) {
                if (maxPointers_ == 2) tapKey(io, KEY_SPACE);
                else if (maxPointers_ >= 3) tapKey(io, KEY_ESCAPE);
            }
            sequenceActive_ = false;
        }
    }
}

void TouchInput::update(Input& io, double nowMs) {
    // The per-frame edges. The held state (keyDown, mouseDown, mouseX/Y) carries over.
    for (int k = 0; k < KEY_COUNT; ++k) io.keyPressed[k] = io.keyReleased[k] = false;
    for (int b = 0; b < MOUSE_BUTTON_COUNT; ++b) io.mousePressed[b] = io.mouseReleased[b] = false;
    io.mouseDX = io.mouseDY = 0;
    io.wheel = 0;
    io.textCount = 0;

    std::vector<TouchEvent> batch;
    std::vector<TouchEvent> postponed;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        batch.swap(queue_);
    }
    // Releases postponed by the previous frame are drained first (their order among themselves is
    // the order they arrived in).
    if (!deferred_.empty()) {
        std::vector<TouchEvent> merged;
        merged.swap(deferred_);
        merged.insert(merged.end(), batch.begin(), batch.end());
        batch.swap(merged);
    }
    bool pressed = false;
    for (size_t i = 0; i < batch.size(); ++i) {
        const TouchEvent& e = batch[i];
        if (e.kind == TouchEvent::Key) {
            setKey(io, e.key, e.down);
            continue;
        }
        if (e.kind == TouchEvent::Wheel) {   // a mouse wheel (a phone in a dock, a tablet with one)
            io.wheel += e.wheel;
            continue;
        }
        if (e.kind == TouchEvent::Text) {   // an IME result can type several characters at once
            size_t n = 0;
            while (n < e.text.size() && io.textCount < int(sizeof(io.text) / sizeof(io.text[0]))) {
                uint32_t cp = 0;
                unsigned char c = (unsigned char)e.text[n];
                if (c < 0x80) { cp = c; n += 1; }
                else if ((c >> 5) == 0x6 && n + 1 < e.text.size()) { cp = uint32_t((c & 0x1F) << 6) | (e.text[n + 1] & 0x3F); n += 2; }
                else if ((c >> 4) == 0xE && n + 2 < e.text.size()) {
                    cp = uint32_t((c & 0x0F) << 12) | ((e.text[n + 1] & 0x3F) << 6) | (e.text[n + 2] & 0x3F);
                    n += 3;
                } else if ((c >> 3) == 0x1E && n + 3 < e.text.size()) {
                    cp = uint32_t((c & 0x07) << 18) | ((e.text[n + 1] & 0x3F) << 12) | ((e.text[n + 2] & 0x3F) << 6) |
                         (e.text[n + 3] & 0x3F);
                    n += 4;
                } else { n += 1; }
                if (cp >= 32 && cp != 127) io.text[io.textCount++] = cp;
            }
            continue;
        }
        // Touch. A tap that arrives whole inside one frame (a quick finger: down and up in the
        // same batch) would be a click the game never sees, because its widgets act on the frame
        // the button goes up while it reads mouseDown from that same frame: the release waits for
        // the next frame. It is put back at the front of the queue (the next update), so the order
        // of the events the game sees never changes.
        const bool press = e.action == 0 || e.action == 5;
        const bool release = e.action == 1 || e.action == 6;
        if (release && pressed) {
            deferred_.push_back(e);
            continue;
        }
        pressed = pressed || press;
        switch (e.action) {
            case 0: down(e.pointerId, e.x, e.y, nowMs); break;   // down
            case 5: down(e.pointerId, e.x, e.y, nowMs); break;   // pointer down
            case 1: up(e.pointerId, e.x, e.y); break;            // up
            case 6: up(e.pointerId, e.x, e.y); break;            // pointer up
            case 3:   // cancelled: the system took the gesture over. Every finger is gone, and
                      // this was not a tap -- the update below only taps while a sequence is
                      // active, so dropping it here is what keeps an interruption from pressing
                      // the clock or opening the menu.
                for (Pointer& p : pointers_) p.down = false;
                downCount_ = 0;
                sequenceActive_ = false;
                break;
            default: move(e.pointerId, e.x, e.y); break;
        }
    }
    if (!postponed.empty()) deferred_ = std::move(postponed);
    if (mapping_ == Touchpad) updateTouchpad(io, nowMs);
    else updateGestures(io, nowMs);
}

}  // namespace plat
