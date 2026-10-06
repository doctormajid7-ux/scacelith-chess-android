package com.scacelith.game;

/**
 * The strip of buttons over the game: the keys of the desktop game that a touchscreen has no gesture
 * for.
 *
 * A table rather than eight builder calls, so what the overlay sends can be read in one place -- and
 * asserted by a JVM test, which is the only half of the overlay that can be tested without a device
 * (the buttons themselves are Views, and would need Robolectric).
 *
 * The gestures cover the rest: one finger plays, two look around and lean in (the pinch), a
 * two-finger tap presses the chess clock and three fingers open the menu (src/platform/touch_input.cpp).
 */
final class OverlayButtons {

    /** One button: what it shows, the plat::Key it sends, and what a screen reader reads out. */
    static final class Entry {
        final String label;
        final int key;
        final String description;

        Entry(String label, int key, String description) {
            this.label = label;
            this.key = key;
            this.description = description;
        }
    }

    /** The keyboard button's key: it shows the input method and sends no key at all. */
    static final int KEYBOARD = 0;

    /** In the order they appear, from the menu to the keyboard. */
    static final Entry[] ALL = {
        new Entry("\u2261", Native.KEY_ESCAPE, "Menu (Esc)"),
        new Entry("\u23F1", Native.KEY_SPACE, "Press the chess clock (Space)"),
        new Entry("\u2630", Native.KEY_TAB, "Move list (Tab)"),
        new Entry("\u2316", 'C', "Look at the board (C)"),
        new Entry("\u270E", 'S', "Scoresheet (S)"),
        new Entry("\u232B", Native.KEY_BACKSPACE, "Take back / delete (Backspace)"),
        new Entry("\u23CE", Native.KEY_ENTER, "Confirm (Enter)"),
        new Entry("\u2328", KEYBOARD, "Keyboard"),
    };

    private OverlayButtons() {}
}
