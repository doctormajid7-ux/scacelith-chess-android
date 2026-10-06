package com.scacelith.game;

import android.view.KeyEvent;

/**
 * The plat::Key of an Android key code, and the character a keystroke types (src/platform/platform.h).
 *
 * Kept apart from GameView, and free of every Android class except the key codes themselves -- which
 * are compile-time constants, so javac inlines them and this file is plain arithmetic that runs on a
 * JVM (app/src/test/java, no device and no emulator: see docs/ANDROID.md).
 *
 * Two things every other platform layer does, and this one had to be taught:
 *
 *  * A letter is the letter <em>key</em>, in uppercase, whatever the layout types. platform_x11.cpp
 *    maps XK_a..XK_z and XK_A..XK_Z alike to 'A' + offset, and platform_win32.cpp sends the same for
 *    'A'..'Z': the game's shortcuts read keyDown['C'] (look at the board) and keyDown['S'] (the
 *    scoresheet), so sending the layout's 'c' would lose both.
 *  * A keystroke also <em>types</em> a character. The desktop layers fill Input::text from WM_CHAR
 *    and from XLookupString, and the game's text fields read that and nothing else (ui_widgets.cpp
 *    inserts from Input::text): a key code alone types nothing into them, so a keyboard plugged into
 *    a phone would leave every name field empty.
 */
final class KeyMap {
    private KeyMap() {}

    /** The game's key for an Android key code, 0 when the game has no use for it. */
    static int key(int keyCode) {
        if (keyCode >= KeyEvent.KEYCODE_A && keyCode <= KeyEvent.KEYCODE_Z)
            return 'A' + (keyCode - KeyEvent.KEYCODE_A);   // the key, not the character
        if (keyCode >= KeyEvent.KEYCODE_0 && keyCode <= KeyEvent.KEYCODE_9)
            return '0' + (keyCode - KeyEvent.KEYCODE_0);
        if (keyCode >= KeyEvent.KEYCODE_F1 && keyCode <= KeyEvent.KEYCODE_F12)
            return Native.KEY_F1 + (keyCode - KeyEvent.KEYCODE_F1);
        switch (keyCode) {
            case KeyEvent.KEYCODE_SPACE: return Native.KEY_SPACE;
            case KeyEvent.KEYCODE_ESCAPE:
            case KeyEvent.KEYCODE_BACK: return Native.KEY_ESCAPE;   // the back button opens the menu
            case KeyEvent.KEYCODE_ENTER:
            case KeyEvent.KEYCODE_NUMPAD_ENTER: return Native.KEY_ENTER;
            case KeyEvent.KEYCODE_TAB: return Native.KEY_TAB;
            case KeyEvent.KEYCODE_DEL: return Native.KEY_BACKSPACE;
            case KeyEvent.KEYCODE_FORWARD_DEL: return Native.KEY_DELETE;
            case KeyEvent.KEYCODE_DPAD_LEFT: return Native.KEY_LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return Native.KEY_RIGHT;
            case KeyEvent.KEYCODE_DPAD_UP: return Native.KEY_UP;
            case KeyEvent.KEYCODE_DPAD_DOWN: return Native.KEY_DOWN;
            case KeyEvent.KEYCODE_MOVE_HOME: return Native.KEY_HOME;
            case KeyEvent.KEYCODE_MOVE_END: return Native.KEY_END;
            case KeyEvent.KEYCODE_PAGE_UP: return Native.KEY_PAGEUP;
            case KeyEvent.KEYCODE_PAGE_DOWN: return Native.KEY_PAGEDOWN;
            case KeyEvent.KEYCODE_SHIFT_LEFT: return Native.KEY_LSHIFT;
            case KeyEvent.KEYCODE_SHIFT_RIGHT: return Native.KEY_RSHIFT;
            case KeyEvent.KEYCODE_CTRL_LEFT: return Native.KEY_LCTRL;
            case KeyEvent.KEYCODE_CTRL_RIGHT: return Native.KEY_RCTRL;
            case KeyEvent.KEYCODE_ALT_LEFT: return Native.KEY_LALT;
            case KeyEvent.KEYCODE_ALT_RIGHT: return Native.KEY_RALT;
            default: return 0;
        }
    }

    /**
     * The character a keystroke types, 0 when it types nothing: what the desktop layers take from
     * WM_CHAR and XLookupString, filtered the same way (the game's fields insert from Input::text
     * and drop anything below a space, ui_widgets.cpp).
     *
     * A modifier key, an arrow, Escape or Tab types nothing (their unicode character is 0 or a
     * control code), Space types a space, exactly as it does on a desktop.
     */
    static int typed(int unicodeChar) {
        return unicodeChar >= 32 && unicodeChar != 127 ? unicodeChar : 0;
    }
}
