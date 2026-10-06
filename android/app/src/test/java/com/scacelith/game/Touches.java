package com.scacelith.game;

import android.view.InputDevice;
import android.view.MotionEvent;

/**
 * Real MotionEvents to hand GameView, built the way the framework builds them.
 *
 * The multi-pointer form of MotionEvent.obtain is the one that carries a pointer array, and it is
 * the only one a two- or three-finger gesture can be tested with; the action of a pointer event
 * carries the index of the pointer it is about, which is what a test of GameView's forwarding is
 * about in the first place.
 */
final class Touches {

    private Touches() {}

    /** One event with the given pointers, a finger each: ids[i] at xs[i], ys[i]. */
    static MotionEvent event(int action, int[] ids, float[] xs, float[] ys) {
        return event(action, ids, xs, ys, InputDevice.SOURCE_TOUCHSCREEN);
    }

    /** The same, from a given source (a mouse in a dock reports its buttons as touch events). */
    static MotionEvent event(int action, int[] ids, float[] xs, float[] ys, int source) {
        final int n = ids.length;
        MotionEvent.PointerProperties[] props = new MotionEvent.PointerProperties[n];
        MotionEvent.PointerCoords[] coords = new MotionEvent.PointerCoords[n];
        for (int i = 0; i < n; i++) {
            props[i] = new MotionEvent.PointerProperties();
            props[i].id = ids[i];
            props[i].toolType = MotionEvent.TOOL_TYPE_FINGER;
            coords[i] = new MotionEvent.PointerCoords();
            coords[i].x = xs[i];
            coords[i].y = ys[i];
            coords[i].pressure = 1f;
            coords[i].size = 1f;
        }
        return MotionEvent.obtain(1000L, 1000L, action, n, props, coords, 0, 0, 1f, 1f, 0, 0,
                                  source, 0);
    }

    /** A mouse wheel: an ACTION_SCROLL event a real mouse sends, its vertical axis turned. */
    static MotionEvent mouseScroll(float notches, float x, float y) {
        MotionEvent.PointerProperties[] props = {new MotionEvent.PointerProperties()};
        props[0].id = 0;
        props[0].toolType = MotionEvent.TOOL_TYPE_MOUSE;
        MotionEvent.PointerCoords[] coords = {new MotionEvent.PointerCoords()};
        coords[0].x = x;
        coords[0].y = y;
        coords[0].setAxisValue(MotionEvent.AXIS_VSCROLL, notches);
        return MotionEvent.obtain(1000L, 1000L, MotionEvent.ACTION_SCROLL, 1, props, coords, 0, 0,
                                  1f, 1f, 0, 0, InputDevice.SOURCE_MOUSE, 0);
    }

    /** ACTION_POINTER_DOWN / _UP about the pointer at index, as Android encodes it. */
    static int aboutPointer(int action, int index) {
        return action | (index << MotionEvent.ACTION_POINTER_INDEX_SHIFT);
    }
}
