package com.scacelith.game;

/**
 * What the game's text fields need from an input method, and what an input method actually sends.
 *
 * The game has no composing state, no selection and no cursor the way an Android text editor does:
 * its fields hold a string with a caret, insert what arrives in Input::text, and answer to the
 * Backspace, Delete, Enter and arrow keys (src/ui/ui_widgets.cpp). An input method, on the other
 * hand, commits words, asks for surrounding text to be deleted and has an action key of its own.
 * This is the translation, kept apart from GameView -- and free of every Android class, so a JVM
 * test can drive it with the calls an input method makes (see docs/ANDROID.md).
 *
 * Where the decisions go is a {@link Sink}: the native layer in the app, a recorder in the tests.
 */
final class InputMethodBridge {

    interface Sink {
        void key(int platKey, boolean down);
        void text(String text);
    }

    private final Sink sink;

    InputMethodBridge(Sink sink) {
        this.sink = sink;
    }

    /**
     * A committed word: the fields insert all of it at the caret, so one text event is enough
     * (several characters at once is exactly what Input::text carries).
     */
    boolean commitText(CharSequence text) {
        if (text != null && text.length() > 0) sink.text(text.toString());
        return true;
    }

    /** Composing state is dropped: only what the input method commits reaches the game. */
    boolean setComposingText(CharSequence text) {
        return true;
    }

    /**
     * Deleting around the caret: Backspace for what stands before it, Delete for what stands after
     * (the fields handle both). An input method asks for this when it replaces a selection or
     * finishes a correction, and a negative length never happens -- it is clamped rather than
     * looping.
     */
    void deleteSurroundingText(int beforeLength, int afterLength) {
        for (int i = 0; i < beforeLength; i++) tap(Native.KEY_BACKSPACE);
        for (int i = 0; i < afterLength; i++) tap(Native.KEY_DELETE);
    }

    /**
     * A key the input method sent itself (Backspace, Enter, arrows, and the letters of a keyboard
     * that types through key events). Returns false for a code the game has no key for, which is
     * how the input method learns it is not ours to handle.
     *
     * No text is sent from here: an input method that types text uses commitText, and one that
     * sends key events for letters would otherwise type everything twice.
     */
    boolean sendKey(int keyCode, boolean down) {
        final int key = KeyMap.key(keyCode);
        if (key == 0) return false;
        sink.key(key, down);
        return true;
    }

    /** The action key (Done, Go, Search...): the game's fields take any of them as Enter. */
    void performEditorAction() {
        tap(Native.KEY_ENTER);
    }

    private void tap(int key) {
        sink.key(key, true);
        sink.key(key, false);
    }
}
