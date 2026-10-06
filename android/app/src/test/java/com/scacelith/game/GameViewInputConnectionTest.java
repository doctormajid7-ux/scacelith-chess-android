package com.scacelith.game;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import android.text.InputType;
import android.view.KeyEvent;
import android.view.ViewGroup;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
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
 * The input connection GameView hands to an input method: InputMethodBridgeTest drives the
 * translation, this drives the adapter in GameView.onCreateInputConnection that carries it to the
 * native layer, with the calls an input method really makes.
 *
 * It is four lines of wiring, but they are the ones a soft keyboard on the phone goes through: the
 * IME never calls Native, and nothing else in the Java layer talks to the game's text fields. A
 * broken adapter is a keyboard that opens over a name field and types nothing into it.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 35, shadows = {ShadowNative.class})
public class GameViewInputConnectionTest {

    private GameView view;

    @Before
    public void setUp() {
        ScacelithActivity activity = Robolectric.buildActivity(ScacelithActivity.class).setup().get();
        ViewGroup root = (ViewGroup)
                ((FrameLayout) activity.findViewById(android.R.id.content)).getChildAt(0);
        view = (GameView) root.getChildAt(0);
        ShadowNative.clear();
    }

    /** The connection an input method would be given, attached the way the framework attaches it. */
    private InputConnection connection() {
        EditorInfo out = new EditorInfo();
        InputConnection ic = view.onCreateInputConnection(out);
        assertNotNull("no input connection: no keyboard can type into the game", ic);
        return ic;
    }

    // -------------------------------------------------------------------------------------------
    // The connection itself
    // -------------------------------------------------------------------------------------------
    @Test
    public void the_view_asks_for_a_text_keyboard_with_a_confirmation_key() {
        // Without onCheckIsTextEditor there is no connection at all, and the fields of the game --
        // a player's name, a server address -- could not be filled on a phone.
        assertTrue(view.onCheckIsTextEditor());

        EditorInfo out = new EditorInfo();
        assertNotNull(view.onCreateInputConnection(out));
        assertEquals(InputType.TYPE_CLASS_TEXT, out.inputType);
        // The action key is what confirms a field, and the game is full-screen: the keyboard must
        // not go full-screen over it.
        assertEquals(EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_FULLSCREEN, out.imeOptions);
    }

    // -------------------------------------------------------------------------------------------
    // Text
    // -------------------------------------------------------------------------------------------
    @Test
    public void a_committed_word_is_typed_into_the_game() {
        InputConnection ic = connection();
        assertTrue(ic.commitText("Bonjour", 1));
        assertEquals(Arrays.asList(NativeCalls.text("Bonjour")), NativeCalls.sent());
    }

    @Test
    public void two_commits_arrive_in_one_piece_each_and_in_order() {
        // The game's fields insert from Input::text, which carries several characters at once: a
        // sentence typed word by word must not be split or reordered.
        InputConnection ic = connection();
        ic.commitText("Bon", 1);
        ic.commitText("jour", 1);
        assertEquals(Arrays.asList(NativeCalls.text("Bon"), NativeCalls.text("jour")), NativeCalls.sent());
    }

    @Test
    public void an_empty_commit_types_nothing() {
        InputConnection ic = connection();
        assertTrue(ic.commitText("", 1));
        assertEquals(0, ShadowNative.count());
    }

    @Test
    public void a_composing_word_is_not_typed() {
        // An input method composes first and commits later; typing the composing text would put
        // letters in the field the player has not accepted yet (InputMethodBridge drops it).
        InputConnection ic = connection();
        assertTrue(ic.setComposingText("\u3053", 1));
        assertEquals(0, ShadowNative.count());
    }

    @Test
    public void the_typed_text_is_characters_not_a_key() {
        // The fields insert from Input::text and the game's shortcuts read keys: committing text
        // must not press anything.
        InputConnection ic = connection();
        ic.commitText("e4", 1);
        assertEquals(Arrays.asList(NativeCalls.text("e4")), NativeCalls.sent());
    }

    // -------------------------------------------------------------------------------------------
    // Deleting around the caret
    // -------------------------------------------------------------------------------------------
    @Test
    public void deleting_before_the_caret_is_a_backspace() {
        InputConnection ic = connection();
        assertTrue(ic.deleteSurroundingText(1, 0));
        assertEquals(NativeCalls.taps(Native.KEY_BACKSPACE, 1), NativeCalls.sent());
    }

    @Test
    public void deleting_after_the_caret_is_a_delete() {
        // The adapter must pass *both* lengths on: an input method that replaces a selection asks
        // for what stands after the caret too, and the fields answer to Delete as well as Backspace.
        InputConnection ic = connection();
        assertTrue(ic.deleteSurroundingText(0, 3));
        assertEquals(NativeCalls.taps(Native.KEY_DELETE, 3), NativeCalls.sent());
    }

    @Test
    public void both_sides_are_deleted_before_then_after() {
        InputConnection ic = connection();
        ic.deleteSurroundingText(2, 1);
        List<String> expected = new ArrayList<>();
        expected.addAll(NativeCalls.taps(Native.KEY_BACKSPACE, 2));
        expected.addAll(NativeCalls.taps(Native.KEY_DELETE, 1));
        assertEquals(expected, NativeCalls.sent());
    }

    // -------------------------------------------------------------------------------------------
    // The action key and the keys the input method sends itself
    // -------------------------------------------------------------------------------------------
    @Test
    public void the_action_key_confirms_the_field() {
        InputConnection ic = connection();
        assertTrue(ic.performEditorAction(EditorInfo.IME_ACTION_DONE));
        assertEquals(NativeCalls.taps(Native.KEY_ENTER, 1), NativeCalls.sent());
    }

    @Test
    public void a_key_the_input_method_sends_goes_to_the_game_with_both_edges() {
        InputConnection ic = connection();
        assertTrue(ic.sendKeyEvent(new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_ENTER)));
        assertTrue(ic.sendKeyEvent(new KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_ENTER)));
        assertEquals(NativeCalls.taps(Native.KEY_ENTER, 1), NativeCalls.sent());

        // Backspace is the other key an input method sends as a key event (its delete key), and a
        // letter is the letter key, as everywhere else in the layer.
        ShadowNative.clear();
        assertTrue(ic.sendKeyEvent(new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DEL)));
        assertEquals(Arrays.asList(NativeCalls.press(Native.KEY_BACKSPACE)), NativeCalls.sent());
    }

    @Test
    public void a_key_event_types_no_text_of_its_own() {
        // The letters of a word arrive through commitText; an input method that also sent them as
        // key events would type everything twice.
        InputConnection ic = connection();
        ic.sendKeyEvent(new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_A));
        assertEquals(Arrays.asList(NativeCalls.press('A')), NativeCalls.sent());
    }

    @Test
    public void a_key_the_game_does_not_know_is_refused() {
        // Returning false is how the input method learns the key is its own to handle.
        InputConnection ic = connection();
        assertFalse(ic.sendKeyEvent(new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_VOLUME_UP)));
        assertEquals(0, ShadowNative.count());
    }

    // -------------------------------------------------------------------------------------------
    // A whole session
    // -------------------------------------------------------------------------------------------
    @Test
    public void a_typing_session_arrives_in_order() {
        // What a keyboard does to a name field: compose (nothing yet), commit, correct a letter,
        // then press Done.
        InputConnection ic = connection();
        ic.setComposingText("Kasparo", 1);
        ic.commitText("Kasparov", 1);
        ic.deleteSurroundingText(1, 0);
        ic.performEditorAction(EditorInfo.IME_ACTION_DONE);

        List<String> expected = new ArrayList<>();
        expected.add(NativeCalls.text("Kasparov"));
        expected.addAll(NativeCalls.taps(Native.KEY_BACKSPACE, 1));
        expected.addAll(NativeCalls.taps(Native.KEY_ENTER, 1));
        assertEquals(expected, NativeCalls.sent());
    }
}
