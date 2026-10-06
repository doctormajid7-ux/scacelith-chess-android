package com.scacelith.game;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotEquals;

import android.view.KeyEvent;

import org.junit.Test;

/**
 * The key translation of the Android layer (KeyMap, game/src/main/.../KeyMap.java).
 *
 * These are plain JVM tests: the Android key codes are compile-time constants, so nothing here
 * loads a device class. What they pin down is the contract the native side has, which the other
 * platform layers already meet (src/platform/platform_x11.cpp, platform_win32.cpp):
 *
 *   * a letter is the letter KEY, in uppercase -- the game's shortcuts read keyDown['C'] and
 *     keyDown['S'], and the layout's lowercase character is not a key it knows;
 *   * a keystroke also TYPES a character (Input::text), because that is the only thing the game's
 *     text fields insert from.
 */
public class KeyMapTest {

    @Test
    public void a_letter_is_the_uppercase_letter_key() {
        assertEquals('C', KeyMap.key(KeyEvent.KEYCODE_C));
        assertEquals('S', KeyMap.key(KeyEvent.KEYCODE_S));
        // Not the character the layout types: a lowercase 'c' is a different array slot in the game
        // (plat::Key has no lowercase letters), and a shortcut bound to 'C' would never fire.
        assertNotEquals('c', KeyMap.key(KeyEvent.KEYCODE_C));
        for (int code = KeyEvent.KEYCODE_A; code <= KeyEvent.KEYCODE_Z; ++code)
            assertEquals('A' + (code - KeyEvent.KEYCODE_A), KeyMap.key(code));
    }

    @Test
    public void digits_and_function_keys_keep_their_order() {
        for (int code = KeyEvent.KEYCODE_0; code <= KeyEvent.KEYCODE_9; ++code)
            assertEquals('0' + (code - KeyEvent.KEYCODE_0), KeyMap.key(code));
        for (int code = KeyEvent.KEYCODE_F1; code <= KeyEvent.KEYCODE_F12; ++code)
            assertEquals(Native.KEY_F1 + (code - KeyEvent.KEYCODE_F1), KeyMap.key(code));
    }

    @Test
    public void the_named_keys_are_the_games_keys() {
        assertEquals(Native.KEY_SPACE, KeyMap.key(KeyEvent.KEYCODE_SPACE));
        assertEquals(Native.KEY_ESCAPE, KeyMap.key(KeyEvent.KEYCODE_ESCAPE));
        assertEquals(Native.KEY_ESCAPE, KeyMap.key(KeyEvent.KEYCODE_BACK));   // back opens the menu
        assertEquals(Native.KEY_ENTER, KeyMap.key(KeyEvent.KEYCODE_ENTER));
        assertEquals(Native.KEY_ENTER, KeyMap.key(KeyEvent.KEYCODE_NUMPAD_ENTER));
        assertEquals(Native.KEY_TAB, KeyMap.key(KeyEvent.KEYCODE_TAB));
        assertEquals(Native.KEY_BACKSPACE, KeyMap.key(KeyEvent.KEYCODE_DEL));
        assertEquals(Native.KEY_DELETE, KeyMap.key(KeyEvent.KEYCODE_FORWARD_DEL));
        assertEquals(Native.KEY_LEFT, KeyMap.key(KeyEvent.KEYCODE_DPAD_LEFT));
        assertEquals(Native.KEY_RIGHT, KeyMap.key(KeyEvent.KEYCODE_DPAD_RIGHT));
        assertEquals(Native.KEY_UP, KeyMap.key(KeyEvent.KEYCODE_DPAD_UP));
        assertEquals(Native.KEY_DOWN, KeyMap.key(KeyEvent.KEYCODE_DPAD_DOWN));
        assertEquals(Native.KEY_HOME, KeyMap.key(KeyEvent.KEYCODE_MOVE_HOME));
        assertEquals(Native.KEY_END, KeyMap.key(KeyEvent.KEYCODE_MOVE_END));
        assertEquals(Native.KEY_PAGEUP, KeyMap.key(KeyEvent.KEYCODE_PAGE_UP));
        assertEquals(Native.KEY_PAGEDOWN, KeyMap.key(KeyEvent.KEYCODE_PAGE_DOWN));
        assertEquals(Native.KEY_LSHIFT, KeyMap.key(KeyEvent.KEYCODE_SHIFT_LEFT));
        assertEquals(Native.KEY_RSHIFT, KeyMap.key(KeyEvent.KEYCODE_SHIFT_RIGHT));
        assertEquals(Native.KEY_LCTRL, KeyMap.key(KeyEvent.KEYCODE_CTRL_LEFT));
        assertEquals(Native.KEY_RCTRL, KeyMap.key(KeyEvent.KEYCODE_CTRL_RIGHT));
        assertEquals(Native.KEY_LALT, KeyMap.key(KeyEvent.KEYCODE_ALT_LEFT));
        assertEquals(Native.KEY_RALT, KeyMap.key(KeyEvent.KEYCODE_ALT_RIGHT));
    }

    @Test
    public void a_key_the_game_has_no_use_for_is_zero() {
        assertEquals(0, KeyMap.key(KeyEvent.KEYCODE_MENU));
        assertEquals(0, KeyMap.key(KeyEvent.KEYCODE_VOLUME_UP));
        assertEquals(0, KeyMap.key(KeyEvent.KEYCODE_CAMERA));
        assertEquals(0, KeyMap.key(0));
    }

    @Test
    public void every_key_the_game_knows_is_reachable() {
        // The whole point of the mapping: the letters the game's shortcuts read, and the named keys
        // its widgets and menus listen to. Written out rather than compared to KeyMap's own switch,
        // so dropping one here is a failure and not a tautology.
        final int[] expected = {
            256, 257, 258, 259, 260, 261, 262, 263, 264, 265, 266, 267, 268, 269, 270, 271, 272, 273, 274,
        };
        final int[] codes = {
            KeyEvent.KEYCODE_ESCAPE, KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_TAB, KeyEvent.KEYCODE_DEL,
            KeyEvent.KEYCODE_FORWARD_DEL, KeyEvent.KEYCODE_DPAD_LEFT, KeyEvent.KEYCODE_DPAD_RIGHT,
            KeyEvent.KEYCODE_DPAD_UP, KeyEvent.KEYCODE_DPAD_DOWN, KeyEvent.KEYCODE_MOVE_HOME,
            KeyEvent.KEYCODE_MOVE_END, KeyEvent.KEYCODE_PAGE_UP, KeyEvent.KEYCODE_PAGE_DOWN,
            KeyEvent.KEYCODE_SHIFT_LEFT, KeyEvent.KEYCODE_SHIFT_RIGHT, KeyEvent.KEYCODE_CTRL_LEFT,
            KeyEvent.KEYCODE_CTRL_RIGHT, KeyEvent.KEYCODE_ALT_LEFT, KeyEvent.KEYCODE_ALT_RIGHT,
        };
        assertEquals(expected.length, codes.length);
        for (int i = 0; i < codes.length; ++i) assertEquals(expected[i], KeyMap.key(codes[i]));
    }

    // -----------------------------------------------------------------------------------------
    // What a keystroke types
    // -----------------------------------------------------------------------------------------

    @Test
    public void a_printable_character_is_typed() {
        assertEquals('a', KeyMap.typed('a'));
        assertEquals('C', KeyMap.typed('C'));
        assertEquals('4', KeyMap.typed('4'));
        assertEquals(' ', KeyMap.typed(' '));   // a space is a character, as it is on a desktop
        assertEquals(0xE9, KeyMap.typed(0xE9));   // é, from a French layout
        assertEquals(0x1F44D, KeyMap.typed(0x1F44D));   // and an emoji, should an IME send one
    }

    @Test
    public void a_key_that_types_nothing_types_nothing() {
        assertEquals(0, KeyMap.typed(0));      // a modifier, an arrow, Escape: getUnicodeChar is 0
        assertEquals(0, KeyMap.typed('\n'));   // Enter
        assertEquals(0, KeyMap.typed('\t'));   // Tab
        assertEquals(0, KeyMap.typed(27));     // Escape
        assertEquals(0, KeyMap.typed(8));      // Backspace
        assertEquals(0, KeyMap.typed(127));    // Delete: a control character, not a character
    }

    @Test
    public void the_key_and_the_character_are_two_different_things() {
        // The key is the same whatever the layout types (so the shortcut is layout-proof), while
        // what is typed follows the layout.
        assertEquals('C', KeyMap.key(KeyEvent.KEYCODE_C));
        assertEquals('c', KeyMap.typed('c'));
        assertEquals('a', KeyMap.typed('a'));
    }
}
