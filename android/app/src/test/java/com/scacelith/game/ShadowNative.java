package com.scacelith.game;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

import org.robolectric.annotation.Implementation;
import org.robolectric.annotation.Implements;

/**
 * A recording stand-in for the native bridge (Native.java, libscacelith.so).
 *
 * The library is built for arm64 and is not loadable in a JVM, so every method of Native is declared
 * native and every View that talks to the game goes through it. Robolectric would call a native
 * method and fail; this shadow replaces all twelve of them with a recording no-op, which is what
 * makes the Views testable off a device: the tests assert on what the Views <em>sent</em>, and the
 * game itself is out of the picture.
 *
 * Enabled with @Config(shadows = ShadowNative.class). The tests write what they expect against the
 * game's own constants (Native.KEY_SPACE, MotionEvent.ACTION_POINTER_DOWN), never against a number
 * typed here.
 */
@Implements(Native.class)
public class ShadowNative {

    /** One call a View made: the name of the native method, and its arguments, boxed. */
    public static final class Call {
        private final String name;
        private final Object[] args;

        Call(String name, Object[] args) {
            this.name = name;
            this.args = args;
        }

        public String name() {
            return name;
        }

        public int intArg(int i) {
            return (Integer) args[i];
        }

        public float floatArg(int i) {
            return (Float) args[i];
        }

        public boolean boolArg(int i) {
            return (Boolean) args[i];
        }

        public Object objectArg(int i) {
            return args[i];
        }

        @Override
        public String toString() {
            return name + Arrays.toString(args);
        }
    }

    private static final List<Call> calls = new ArrayList<>();

    private static void record(String name, Object... args) {
        calls.add(new Call(name, args));
    }

    // ---- the window ----
    @Implementation
    public static void nativeInit(Object activity, String filesDir) {
        record("nativeInit", activity, filesDir);
    }

    @Implementation
    public static void nativeStart() {
        record("nativeStart");
    }

    @Implementation
    public static void nativeSurfaceCreated(Object surface) {
        record("nativeSurfaceCreated", surface);
    }

    @Implementation
    public static void nativeSurfaceChanged(int width, int height) {
        record("nativeSurfaceChanged", width, height);
    }

    @Implementation
    public static void nativeSurfaceDestroyed() {
        record("nativeSurfaceDestroyed");
    }

    @Implementation
    public static void nativePause() {
        record("nativePause");
    }

    @Implementation
    public static void nativeResume() {
        record("nativeResume");
    }

    @Implementation
    public static void nativeShutdown() {
        record("nativeShutdown");
    }

    // ---- the input ----
    @Implementation
    public static void nativeTouch(int action, int pointerId, float x, float y, int pointerCount) {
        record("nativeTouch", action, pointerId, x, y, pointerCount);
    }

    @Implementation
    public static void nativeKey(int key, boolean down) {
        record("nativeKey", key, down);
    }

    @Implementation
    public static void nativeWheel(float notches) {
        record("nativeWheel", notches);
    }

    @Implementation
    public static void nativeText(String text) {
        record("nativeText", text);
    }

    // -------------------------------------------------------------------------------------------
    // What the tests read back
    // -------------------------------------------------------------------------------------------
    /** Every call since the last clear(), in order, as a copy. */
    public static List<Call> calls() {
        return Collections.unmodifiableList(new ArrayList<>(calls));
    }

    /** The calls of one method, in order. */
    public static List<Call> of(String name) {
        List<Call> out = new ArrayList<>();
        for (Call c : calls) if (c.name.equals(name)) out.add(c);
        return out;
    }

    /** The name of every call, in order: what a test compares to a whole expected sequence. */
    public static List<String> names() {
        List<String> out = new ArrayList<>();
        for (Call c : calls) out.add(c.name);
        return out;
    }

    /** The last call, or null when the Views sent nothing. */
    public static Call last() {
        return calls.isEmpty() ? null : calls.get(calls.size() - 1);
    }

    public static int count() {
        return calls.size();
    }

    public static void clear() {
        calls.clear();
    }
}
