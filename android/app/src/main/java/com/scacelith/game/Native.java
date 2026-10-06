package com.scacelith.game;

/**
 * The native side of the app (libscacelith.so, src/platform/platform_android.cpp).
 *
 * Everything here is a thin bridge: the game runs its own loop on its own thread, and this class
 * only hands it the window, the events and the few services that need Java.
 */
final class Native {
    private Native() {}

    static {
        // Loads libscacelith.so from the APK (app/build.gradle.kts names it "scacelith"). Class
        // initialisation runs before the first native call (ScacelithActivity.onCreate's
        // Native.nativeInit), which is the contract every declaration below relies on.
        System.loadLibrary("scacelith");
    }

    // ---- keys, mirrored from plat::Key (src/platform/platform.h). Letters and digits use their
    // ASCII code; the codes below start at 256 and follow the enum's order. ----
    static final int KEY_A = 'A', KEY_Z = 'Z', KEY_0 = '0', KEY_9 = '9', KEY_SPACE = ' ';
    static final int KEY_ESCAPE = 256, KEY_ENTER = 257, KEY_TAB = 258, KEY_BACKSPACE = 259, KEY_DELETE = 260;
    static final int KEY_LEFT = 261, KEY_RIGHT = 262, KEY_UP = 263, KEY_DOWN = 264;
    static final int KEY_HOME = 265, KEY_END = 266, KEY_PAGEUP = 267, KEY_PAGEDOWN = 268;
    static final int KEY_LSHIFT = 269, KEY_RSHIFT = 270, KEY_LCTRL = 271, KEY_RCTRL = 272;
    static final int KEY_LALT = 273, KEY_RALT = 274;
    static final int KEY_F1 = 275, KEY_F2 = 276, KEY_F3 = 277, KEY_F4 = 278, KEY_F5 = 279, KEY_F6 = 280;
    static final int KEY_F7 = 281, KEY_F8 = 282, KEY_F9 = 283, KEY_F10 = 284, KEY_F11 = 285, KEY_F12 = 286;

    // ---- touch actions, mirrored from android.view.MotionEvent ----
    static final int ACTION_DOWN = 0, ACTION_UP = 1, ACTION_MOVE = 2, ACTION_CANCEL = 3;
    static final int ACTION_POINTER_DOWN = 5, ACTION_POINTER_UP = 6;

    /** Creates the platform layer, remembering the Activity and the app's private folder. */
    static native void nativeInit(Object activity, String filesDir);
    /** Starts the game loop on its own thread (called once, from onCreate). */
    static native void nativeStart();
    /** The SurfaceView has a surface: the game thread adopts it and creates its EGL surface. */
    static native void nativeSurfaceCreated(Object surface);
    static native void nativeSurfaceChanged(int width, int height);
    /** The surface went away (the activity is going to the background): the loop parks itself. */
    static native void nativeSurfaceDestroyed();
    static native void nativePause();
    static native void nativeResume();
    /** The activity is being destroyed: the game loop saves its settings and returns. */
    static native void nativeShutdown();

    /** One touch pointer event: action, its pointer id, its position and how many are down. */
    static native void nativeTouch(int action, int pointerId, float x, float y, int pointerCount);
    /** A key from the software or a physical keyboard. */
    static native void nativeKey(int key, boolean down);
    /** A mouse wheel notch (+ = lean towards the board). */
    static native void nativeWheel(float notches);
    /** Text typed by an input method (several characters at once when it commits a word). */
    static native void nativeText(String text);

    // ---- the HTTPS transport (Http.java, src/net/transport_android.cpp) ----
    /** The head of a response: status and header name/value pairs (names lower-case). False stops. */
    static native boolean nativeHttpHead(long ctx, int status, String[] headers);
    /** A piece of the body (the first n bytes of buf). False aborts the exchange. */
    static native boolean nativeHttpBody(long ctx, byte[] buf, int n);
}
