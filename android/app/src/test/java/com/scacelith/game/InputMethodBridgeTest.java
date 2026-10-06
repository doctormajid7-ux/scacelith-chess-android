package com.scacelith.game;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import android.view.KeyEvent;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

import org.junit.Test;

/**
 * The input-method bridge (InputMethodBridge, game/src/main/.../InputMethodBridge.java).
 *
 * An input method commits words, asks for text around the caret to be deleted, and has an action
 * key of its own; the game's fields insert from Input::text and answer to Backspace, Delete and
 * Enter (src/ui/ui_widgets.cpp). The bridge is plain Java with a sink, so what an input method's
 * calls turn into can be read here instead of guessed at on a device.
 */
public class InputMethodBridgeTest {

    /** Everything the bridge asks the native layer to do, in order. */
    private static final class Recorder implements InputMethodBridge.Sink {
        final List<String> calls = new ArrayList<>();

        @Override
        public void key(int platKey, boolean down) {
            calls.add("key " + platKey + (down ? " down" : " up"));
        }

        @Override
        public void text(String text) {
            calls.add("text " + text);
        }
    }

    private final Recorder recorder = new Recorder();
    private final InputMethodBridge bridge = new InputMethodBridge(recorder);

    private static String backspace() {
        return "key " + Native.KEY_BACKSPACE;
    }

    private static String delete() {
        return "key " + Native.KEY_DELETE;
    }

    @Test
    public void a_committed_word_is_typed_at_once() {
        assertTrue(bridge.commitText("Bonjour"));
        assertEquals(Arrays.asList("text Bonjour"), recorder.calls);
    }

    @Test
    public void an_empty_commit_types_nothing() {
        assertTrue(bridge.commitText(""));
        assertTrue(bridge.commitText(null));
        assertEquals(new ArrayList<String>(), recorder.calls);
    }

    @Test
    public void composing_is_not_typed() {
        // An input method composes first and commits later; typing the composing text would put
        // letters in the field that the user has not accepted yet.
        assertTrue(bridge.setComposingText("\u3042"));
        assertEquals(new ArrayList<String>(), recorder.calls);
    }

    /** A full press and release of a key, as the game's fields listen for both edges. */
    private static List<String> taps(String key, int times) {
        List<String> out = new ArrayList<>();
        for (int i = 0; i < times; ++i) {
            out.add(key + " down");
            out.add(key + " up");
        }
        return out;
    }

    @Test
    public void deleting_before_the_caret_is_one_backspace_per_character() {
        bridge.deleteSurroundingText(3, 0);
        assertEquals(taps(backspace(), 3), recorder.calls);
    }

    @Test
    public void deleting_after_the_caret_is_a_delete_key() {
        // The game's fields have a caret and answer to Delete as well as Backspace (ui_widgets.cpp):
        // an input method that corrects a selection asks for both sides.
        bridge.deleteSurroundingText(0, 2);
        assertEquals(taps(delete(), 2), recorder.calls);
    }

    @Test
    public void both_sides_are_deleted_before_then_after() {
        bridge.deleteSurroundingText(1, 1);
        List<String> expected = new ArrayList<>();
        expected.addAll(taps(backspace(), 1));
        expected.addAll(taps(delete(), 1));
        assertEquals(expected, recorder.calls);
    }

    @Test
    public void a_nonsensical_length_deletes_nothing() {
        // Never anything but a guarded loop: a negative length must not run away.
        bridge.deleteSurroundingText(-1, -1);
        bridge.deleteSurroundingText(0, 0);
        assertEquals(new ArrayList<String>(), recorder.calls);
    }

    @Test
    public void a_key_event_becomes_the_games_key() {
        assertTrue(bridge.sendKey(KeyEvent.KEYCODE_DEL, true));
        assertTrue(bridge.sendKey(KeyEvent.KEYCODE_DEL, false));
        assertEquals(taps(backspace(), 1), recorder.calls);

        // A letter is the letter key, as everywhere else in the layer.
        recorder.calls.clear();
        assertTrue(bridge.sendKey(KeyEvent.KEYCODE_C, true));
        assertEquals(Arrays.asList("key " + (int) 'C' + " down"), recorder.calls);
    }

    @Test
    public void a_key_event_sends_no_text() {
        // An input method that sends key events for letters would otherwise type everything twice
        // (the text of a committed word arrives through commitText).
        bridge.sendKey(KeyEvent.KEYCODE_A, true);
        for (String call : recorder.calls) assertFalse(call, call.startsWith("text "));
    }

    @Test
    public void a_key_the_game_does_not_know_is_refused() {
        // Saying so is what lets the input method handle it itself (a volume key, a system gesture).
        assertFalse(bridge.sendKey(KeyEvent.KEYCODE_VOLUME_UP, true));
        assertFalse(bridge.sendKey(KeyEvent.KEYCODE_MENU, true));
        assertEquals(new ArrayList<String>(), recorder.calls);
    }

    @Test
    public void the_action_key_is_enter() {
        // Done, Go, Send...: the game's fields take any of them as a confirmation.
        bridge.performEditorAction();
        assertEquals(Arrays.asList("key " + Native.KEY_ENTER + " down",
                                   "key " + Native.KEY_ENTER + " up"),
                     recorder.calls);
    }
}
