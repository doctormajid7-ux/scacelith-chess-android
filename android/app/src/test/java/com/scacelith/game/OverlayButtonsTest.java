package com.scacelith.game;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

import org.junit.Test;

/**
 * The overlay's button table (OverlayButtons, game/src/main/.../OverlayButtons.java).
 *
 * What the buttons send is a table, and the table is what this checks: the eight keys the desktop
 * game has and a touchscreen has no gesture for, each sent once, each one a key the native side
 * accepts. The buttons as Views -- built from it, laid out, pressed and clicked -- belong to
 * ScacelithActivityTest, which runs them under Robolectric.
 */
public class OverlayButtonsTest {

    /** "label=key" for each entry, the order included. */
    private static List<String> table() {
        List<String> out = new ArrayList<>();
        for (OverlayButtons.Entry e : OverlayButtons.ALL) out.add(e.label + "=" + e.key);
        return out;
    }

    @Test
    public void the_table_is_the_eight_keys_with_no_gesture() {
        List<String> expected = new ArrayList<>();
        expected.add("\u2261=" + Native.KEY_ESCAPE);        // the menu
        expected.add("\u23F1=" + Native.KEY_SPACE);         // the chess clock
        expected.add("\u2630=" + Native.KEY_TAB);           // the move list
        expected.add("\u2316=" + (int) 'C');                // look at the board
        expected.add("\u270E=" + (int) 'S');                // the scoresheet
        expected.add("\u232B=" + Native.KEY_BACKSPACE);     // take back / delete
        expected.add("\u23CE=" + Native.KEY_ENTER);         // confirm
        expected.add("\u2328=" + OverlayButtons.KEYBOARD);  // the keyboard, which sends no key
        assertEquals(expected, table());
    }

    @Test
    public void exactly_one_button_opens_the_keyboard_and_it_sends_no_key() {
        int keyboard = 0;
        for (OverlayButtons.Entry e : OverlayButtons.ALL)
            if (e.key == OverlayButtons.KEYBOARD) ++keyboard;
        assertEquals(1, keyboard);
        // 0 is plat::KEY_UNKNOWN: a button that sent it would send nothing, which is what the
        // keyboard button wants -- and what any other button must not do.
        assertEquals(OverlayButtons.KEYBOARD, OverlayButtons.ALL[OverlayButtons.ALL.length - 1].key);
    }

    @Test
    public void no_two_buttons_send_the_same_key() {
        Set<Integer> seen = new HashSet<>();
        for (OverlayButtons.Entry e : OverlayButtons.ALL)
            assertTrue("duplicate key " + e.key, seen.add(e.key));
    }

    @Test
    public void every_button_describes_itself() {
        Set<String> labels = new HashSet<>();
        for (OverlayButtons.Entry e : OverlayButtons.ALL) {
            assertTrue("empty label", e.label != null && !e.label.isEmpty());
            assertTrue("two buttons share the label " + e.label, labels.add(e.label));
            assertTrue("empty description", e.description != null && !e.description.isEmpty());
            // A screen reader reads the label out as text; the description is what says what the
            // button does, so a button that sends a key must name it (the keyboard button names
            // none: it only shows the input method).
            if (e.key != OverlayButtons.KEYBOARD)
                assertTrue("no key named in: " + e.description, e.description.contains("("));
        }
    }

    @Test
    public void every_key_is_one_the_game_accepts() {
        // The native side drops a key outside 1..plat::KEY_COUNT (touch_input.cpp, setKey), and the
        // game's widgets only listen to the letters and the named keys. A 0 here would be a button
        // that silently does nothing -- except the keyboard button, which means to.
        for (OverlayButtons.Entry e : OverlayButtons.ALL) {
            if (e.key == OverlayButtons.KEYBOARD) continue;
            boolean letter = e.key >= 'A' && e.key <= 'Z';
            boolean digit = e.key >= '0' && e.key <= '9';
            boolean named = e.key == Native.KEY_SPACE ||
                            (e.key >= Native.KEY_ESCAPE && e.key <= Native.KEY_F12);
            assertTrue(e.label + " sends " + e.key, letter || digit || named);
        }
    }
}
