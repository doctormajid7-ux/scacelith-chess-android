package com.scacelith.game;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import android.view.KeyEvent;
import android.view.ViewGroup;
import android.widget.FrameLayout;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

/**
 * GameView and a physical keyboard: the other half of the key path, the one an input method does not
 * go through. KeyMapTest checks the table; this checks the two overrides that use it, and the fact
 * that a keystroke carries <em>two</em> things to the game.
 *
 * A letter is both a character and a key, and the game wants both: the text goes to Input::text,
 * which is the only thing its fields insert from, and the key to the shortcuts, which read
 * <code>keyDown['C']</code> (look at the board) and <code>keyDown['S']</code> (the scoresheet). A
 * named key -- Escape, Tab, Backspace, an arrow, a modifier -- carries a key and nothing else, and a
 * key the game has no use for is left to the system.
 *
 * The characters come from the framework's own key map (event.getUnicodeChar()), so what is checked
 * here is that the layer keeps what the layout produces -- lowercase unshifted, uppercase shifted --
 * while the *key* stays the game's uppercase letter whatever the layout.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 35, shadows = {ShadowNative.class})
public class GameViewKeyboardTest {

    private GameView view;

    @Before
    public void setUp() {
        ScacelithActivity activity = Robolectric.buildActivity(ScacelithActivity.class).setup().get();
        ViewGroup root = (ViewGroup)
                ((FrameLayout) activity.findViewById(android.R.id.content)).getChildAt(0);
        view = (GameView) root.getChildAt(0);
        // The activity's own startup has been recorded and is not what this file is about.
        ShadowNative.clear();
    }

    private static KeyEvent down(int keyCode) {
        return new KeyEvent(KeyEvent.ACTION_DOWN, keyCode);
    }

    private static KeyEvent up(int keyCode) {
        return new KeyEvent(KeyEvent.ACTION_UP, keyCode);
    }

    /** A press with a modifier down, the way a keyboard reports Shift+c. */
    private static KeyEvent down(int keyCode, int metaState) {
        return new KeyEvent(0L, 0L, KeyEvent.ACTION_DOWN, keyCode, 1, metaState);
    }

    /** The code of each key that has a name in the game, and the game's key for it. */
    private static int[][] namedKeys() {
        return new int[][] {
            {KeyEvent.KEYCODE_ESCAPE, Native.KEY_ESCAPE},
            {KeyEvent.KEYCODE_BACK, Native.KEY_ESCAPE},            // the back button opens the menu
            {KeyEvent.KEYCODE_TAB, Native.KEY_TAB},
            {KeyEvent.KEYCODE_ENTER, Native.KEY_ENTER},
            {KeyEvent.KEYCODE_NUMPAD_ENTER, Native.KEY_ENTER},
            {KeyEvent.KEYCODE_DEL, Native.KEY_BACKSPACE},
            {KeyEvent.KEYCODE_FORWARD_DEL, Native.KEY_DELETE},
            {KeyEvent.KEYCODE_DPAD_LEFT, Native.KEY_LEFT},
            {KeyEvent.KEYCODE_DPAD_RIGHT, Native.KEY_RIGHT},
            {KeyEvent.KEYCODE_DPAD_UP, Native.KEY_UP},
            {KeyEvent.KEYCODE_DPAD_DOWN, Native.KEY_DOWN},
            {KeyEvent.KEYCODE_MOVE_HOME, Native.KEY_HOME},
            {KeyEvent.KEYCODE_MOVE_END, Native.KEY_END},
            {KeyEvent.KEYCODE_PAGE_UP, Native.KEY_PAGEUP},
            {KeyEvent.KEYCODE_PAGE_DOWN, Native.KEY_PAGEDOWN},
            {KeyEvent.KEYCODE_F1, Native.KEY_F1},
            {KeyEvent.KEYCODE_F12, Native.KEY_F12},
        };
    }

    // -------------------------------------------------------------------------------------------
    // A letter is a character and a key
    // -------------------------------------------------------------------------------------------
    @Test
    public void a_letter_types_its_character_and_presses_the_letter_key() {
        assertTrue(view.onKeyDown(KeyEvent.KEYCODE_C, down(KeyEvent.KEYCODE_C)));
        // 'c' for the field (Input::text), 'C' for the shortcut -- the two halves of one keystroke.
        assertEquals(Arrays.asList(NativeCalls.text("c"), NativeCalls.press('C')), NativeCalls.sent());
    }

    @Test
    public void the_typed_character_keeps_the_case_the_layout_gives_it() {
        assertTrue(view.onKeyDown(KeyEvent.KEYCODE_C, down(KeyEvent.KEYCODE_C, KeyEvent.META_SHIFT_ON)));
        // Shift+c types 'C' -- a player naming a file gets the capital -- while the key is the same
        // 'C' as without Shift, which is what the shortcuts read.
        assertEquals(Arrays.asList(NativeCalls.text("C"), NativeCalls.press('C')), NativeCalls.sent());
    }

    @Test
    public void a_digit_types_it_and_presses_it() {
        assertTrue(view.onKeyDown(KeyEvent.KEYCODE_3, down(KeyEvent.KEYCODE_3)));
        assertEquals(Arrays.asList(NativeCalls.text("3"), NativeCalls.press('3')), NativeCalls.sent());
    }

    @Test
    public void space_types_a_space_and_presses_the_space_key() {
        // The clock is the Space key, and a space is also a character a name can contain.
        assertTrue(view.onKeyDown(KeyEvent.KEYCODE_SPACE, down(KeyEvent.KEYCODE_SPACE)));
        assertEquals(Arrays.asList(NativeCalls.text(" "), NativeCalls.press(Native.KEY_SPACE)),
                     NativeCalls.sent());
    }

    @Test
    public void the_release_sends_the_key_and_types_nothing() {
        view.onKeyDown(KeyEvent.KEYCODE_C, down(KeyEvent.KEYCODE_C));
        ShadowNative.clear();
        assertTrue(view.onKeyUp(KeyEvent.KEYCODE_C, up(KeyEvent.KEYCODE_C)));
        assertEquals(Arrays.asList(NativeCalls.release('C')), NativeCalls.sent());
    }

    @Test
    public void a_held_letter_repeats_the_way_it_does_on_a_desktop() {
        // Android reports auto-repeat as another ACTION_DOWN, and the desktop layers type the
        // character again on every repeat too (WM_CHAR, XLookupString): a field must fill up when a
        // key is held, not stand still.
        view.onKeyDown(KeyEvent.KEYCODE_C, down(KeyEvent.KEYCODE_C));
        view.onKeyDown(KeyEvent.KEYCODE_C, down(KeyEvent.KEYCODE_C));
        assertEquals(Arrays.asList(NativeCalls.text("c"), NativeCalls.press('C'),
                                   NativeCalls.text("c"), NativeCalls.press('C')),
                     NativeCalls.sent());
    }

    // -------------------------------------------------------------------------------------------
    // A named key carries a key and nothing else
    // -------------------------------------------------------------------------------------------
    @Test
    public void a_key_with_a_name_presses_the_games_key_and_types_nothing() {
        for (int[] pair : namedKeys()) {
            ShadowNative.clear();
            assertTrue("press " + pair[0], view.onKeyDown(pair[0], down(pair[0])));
            // Exactly one call: the key. An arrow or an Escape that typed something would put a
            // control character in whatever field is open.
            assertEquals("key " + pair[0], Arrays.asList(NativeCalls.press(pair[1])), NativeCalls.sent());

            ShadowNative.clear();
            assertTrue("release " + pair[0], view.onKeyUp(pair[0], up(pair[0])));
            assertEquals("key " + pair[0], Arrays.asList(NativeCalls.release(pair[1])), NativeCalls.sent());
        }
    }

    @Test
    public void a_modifier_presses_the_games_key_and_types_nothing() {
        int[][] modifiers = {
            {KeyEvent.KEYCODE_SHIFT_LEFT, Native.KEY_LSHIFT},
            {KeyEvent.KEYCODE_SHIFT_RIGHT, Native.KEY_RSHIFT},
            {KeyEvent.KEYCODE_CTRL_LEFT, Native.KEY_LCTRL},
            {KeyEvent.KEYCODE_CTRL_RIGHT, Native.KEY_RCTRL},
            {KeyEvent.KEYCODE_ALT_LEFT, Native.KEY_LALT},
            {KeyEvent.KEYCODE_ALT_RIGHT, Native.KEY_RALT},
        };
        for (int[] pair : modifiers) {
            ShadowNative.clear();
            assertTrue(view.onKeyDown(pair[0], down(pair[0])));
            // A modifier held down must not insert anything (its character is 0 or a control code,
            // and KeyMap.typed drops everything below a space).
            assertEquals(Arrays.asList(NativeCalls.press(pair[1])), NativeCalls.sent());
        }
    }

    // -------------------------------------------------------------------------------------------
    // The keys that are the system's
    // -------------------------------------------------------------------------------------------
    @Test
    public void a_key_the_game_has_no_use_for_is_left_to_the_system() {
        // The volume keys act on the phone wherever they are pressed (the activity says so too), and
        // the game must not hear them at all.
        for (int keyCode : new int[] {KeyEvent.KEYCODE_VOLUME_UP, KeyEvent.KEYCODE_VOLUME_DOWN}) {
            ShadowNative.clear();
            assertFalse(view.onKeyDown(keyCode, down(keyCode)));
            assertFalse(view.onKeyUp(keyCode, up(keyCode)));
            assertEquals(0, ShadowNative.count());
        }
    }

    // -------------------------------------------------------------------------------------------
    // A whole word
    // -------------------------------------------------------------------------------------------
    @Test
    public void typing_a_name_and_confirming_it() {
        // A keyboard plugged into the phone, a name field open: three letters, a correction, Enter.
        int[][] strokes = {{KeyEvent.KEYCODE_K, 'K'}, {KeyEvent.KEYCODE_A, 'A'}, {KeyEvent.KEYCODE_S, 'S'}};
        for (int[] stroke : strokes) {
            view.onKeyDown(stroke[0], down(stroke[0]));
            view.onKeyUp(stroke[0], up(stroke[0]));
        }
        view.onKeyDown(KeyEvent.KEYCODE_DEL, down(KeyEvent.KEYCODE_DEL));          // take the S back
        view.onKeyUp(KeyEvent.KEYCODE_DEL, up(KeyEvent.KEYCODE_DEL));
        view.onKeyDown(KeyEvent.KEYCODE_ENTER, down(KeyEvent.KEYCODE_ENTER));      // confirm it
        view.onKeyUp(KeyEvent.KEYCODE_ENTER, up(KeyEvent.KEYCODE_ENTER));

        List<String> expected = new ArrayList<>();
        expected.addAll(Arrays.asList(NativeCalls.text("k"), NativeCalls.press('K'), NativeCalls.release('K')));
        expected.addAll(Arrays.asList(NativeCalls.text("a"), NativeCalls.press('A'), NativeCalls.release('A')));
        expected.addAll(Arrays.asList(NativeCalls.text("s"), NativeCalls.press('S'), NativeCalls.release('S')));
        expected.addAll(NativeCalls.taps(Native.KEY_BACKSPACE, 1));
        expected.addAll(NativeCalls.taps(Native.KEY_ENTER, 1));
        assertEquals(expected, NativeCalls.sent());
    }
}
