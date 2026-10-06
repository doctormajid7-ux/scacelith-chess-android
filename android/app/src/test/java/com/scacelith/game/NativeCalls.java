package com.scacelith.game;

import java.util.ArrayList;
import java.util.List;

/**
 * What the Views told the native layer (ShadowNative), as plain strings, so that a test can compare
 * a whole expected sequence to what happened -- the shortest way to pin an order as well as a
 * content, which is what every one of these seams is about.
 *
 *   * a committed word or a typed character: "text Kasparov";
 *   * a key of the game: "key 259 down" (plat::Key 259 is Backspace).
 *
 * The names are ShadowNative's (nativeText, nativeKey), so a call this helper does not know shows
 * up as its own name rather than being silently dropped.
 */
final class NativeCalls {

    private NativeCalls() {}

    /** Every call since the last clear(), in order. */
    static List<String> sent() {
        List<String> out = new ArrayList<>();
        for (ShadowNative.Call c : ShadowNative.calls()) {
            if (c.name().equals("nativeText")) out.add(text((String) c.objectArg(0)));
            else if (c.name().equals("nativeKey")) out.add((c.boolArg(1) ? press(c.intArg(0)) : release(c.intArg(0))));
            else out.add(c.name());
        }
        return out;
    }

    /** One keystroke's press and release, repeated: what a tap of a button or of an IME key is. */
    static List<String> taps(int key, int times) {
        List<String> out = new ArrayList<>();
        for (int i = 0; i < times; ++i) {
            out.add(press(key));
            out.add(release(key));
        }
        return out;
    }

    static String text(String typed) {
        return "text " + typed;
    }

    static String press(int key) {
        return "key " + key + " down";
    }

    static String release(int key) {
        return "key " + key + " up";
    }
}
