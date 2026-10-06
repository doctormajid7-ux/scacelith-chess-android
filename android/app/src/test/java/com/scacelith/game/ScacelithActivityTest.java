package com.scacelith.game;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertSame;
import static org.junit.Assert.assertTrue;

import android.content.Context;
import android.os.Looper;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.inputmethod.InputMethodManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;

import java.util.Arrays;
import java.util.List;

import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.Shadows;
import org.robolectric.android.controller.ActivityController;
import org.robolectric.annotation.Config;

/**
 * ScacelithActivity: the window of the game -- the overlay of buttons over the SurfaceView, and the
 * lifecycle calls that start the game and keep it alive across a pause.
 *
 * OverlayButtonsTest checks the table the buttons are built from; this checks the Views themselves:
 * that each button shows its entry, that pressing one sends that entry's key (a press and a release,
 * nothing else), that the keyboard button shows the input method and sends no key, and that the
 * activity itself tells the game when to run and when to park. Robolectric runs all of it in a JVM,
 * so a button can be laid out and clicked the way a finger clicks it, and libscacelith.so is
 * replaced by ShadowNative, which records what it was told.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 35, shadows = {ShadowNative.class})
public class ScacelithActivityTest {

    private ScacelithActivity activity;
    private LinearLayout overlay;

    @Before
    public void setUp() {
        // setup() is create + start + resume + the decor view attached to a window, which is what the
        // buttons need to be laid out and clicked. onCreate also calls nativeInit and nativeStart.
        activity = Robolectric.buildActivity(ScacelithActivity.class).setup().get();
        FrameLayout content = (FrameLayout) activity.findViewById(android.R.id.content);
        FrameLayout root = (FrameLayout) content.getChildAt(0);
        assertEquals("the game view and the overlay are the two children of the root",
                     2, root.getChildCount());
        assertTrue("the game view is under the overlay", root.getChildAt(0) instanceof GameView);
        overlay = (LinearLayout) root.getChildAt(1);
        // Start every test with an empty log: the startup of the activity is the game's business, and
        // the lifecycle test below builds its own activity to look at it.
        ShadowNative.clear();
    }

    /** The button of one entry: its position in the bar is its position in the table. */
    private Button button(OverlayButtons.Entry entry) {
        for (int i = 0; i < OverlayButtons.ALL.length; i++)
            if (OverlayButtons.ALL[i] == entry) return (Button) overlay.getChildAt(i);
        throw new AssertionError("the table has no entry " + entry.label);
    }

    private static OverlayButtons.Entry entryWithKey(int key) {
        for (OverlayButtons.Entry e : OverlayButtons.ALL) if (e.key == key) return e;
        throw new AssertionError("the table has no button for key " + key);
    }

    // -------------------------------------------------------------------------------------------
    // The overlay
    // -------------------------------------------------------------------------------------------
    @Test
    public void every_entry_of_the_table_is_a_button_of_the_bar() {
        assertEquals(OverlayButtons.ALL.length, overlay.getChildCount());
        for (int i = 0; i < OverlayButtons.ALL.length; i++) {
            OverlayButtons.Entry entry = OverlayButtons.ALL[i];
            Button b = (Button) overlay.getChildAt(i);
            assertEquals(entry.label, b.getText().toString());                      // what it shows
            assertEquals(entry.description, b.getContentDescription().toString());   // what it says
            assertTrue("the buttons are laid out", b.getLayoutParams().width > 0);
            assertNotNull("no click listener on " + entry.label,
                          Shadows.shadowOf((View) b).getOnClickListener());
        }
    }

    @Test
    public void pressing_a_button_sends_its_key_once() {
        for (OverlayButtons.Entry entry : OverlayButtons.ALL) {
            if (entry.key == OverlayButtons.KEYBOARD) continue;
            ShadowNative.clear();
            assertTrue("press " + entry.label, button(entry).performClick());
            assertEquals("press then release of " + entry.description, 2, ShadowNative.count());
            ShadowNative.Call down = ShadowNative.calls().get(0);
            ShadowNative.Call up = ShadowNative.calls().get(1);
            assertEquals("nativeKey", down.name());
            assertEquals(entry.key, down.intArg(0));
            assertTrue("the press comes first", down.boolArg(1));
            assertEquals("nativeKey", up.name());
            assertEquals(entry.key, up.intArg(0));
            assertFalse("and the release after it", up.boolArg(1));
        }
    }

    @Test
    public void the_keyboard_button_shows_the_input_method_and_sends_no_key() {
        InputMethodManager imm = (InputMethodManager) activity.getSystemService(Context.INPUT_METHOD_SERVICE);
        assertNotNull(imm);
        // Start from a known state: the shadow keeps its visibility in a static field, and this is
        // the only test that looks at it.
        imm.hideSoftInputFromWindow(activity.getWindow().getDecorView().getWindowToken(), 0);
        assertFalse(Shadows.shadowOf(imm).isSoftInputVisible());

        ShadowNative.clear();
        assertTrue(button(entryWithKey(OverlayButtons.KEYBOARD)).performClick());

        // The keys with a gesture are the native layer's; the keyboard is the only button that asks
        // the system for something, and it must not look like a key of the game (KEYBOARD is 0,
        // plat::KEY_UNKNOWN, which the native side drops).
        assertEquals(0, ShadowNative.count());
        assertTrue("the keyboard was asked for", Shadows.shadowOf(imm).isSoftInputVisible());
    }

    @Test
    public void a_touch_on_a_button_reaches_the_game() {
        // Not just a listener that exists: a finger down and up on the button itself, the way the
        // framework delivers it. View.onTouchEvent posts the click, so the looper is drained after.
        Button b = button(entryWithKey(Native.KEY_ENTER));
        b.layout(0, 0, b.getLayoutParams().width, b.getLayoutParams().height);

        ShadowNative.clear();
        long downTime = 1000L;
        b.dispatchTouchEvent(MotionEvent.obtain(downTime, downTime, MotionEvent.ACTION_DOWN, 10f, 10f, 0));
        b.dispatchTouchEvent(MotionEvent.obtain(downTime, downTime + 50, MotionEvent.ACTION_UP, 10f, 10f, 0));
        Shadows.shadowOf(Looper.getMainLooper()).idle();

        assertEquals(2, ShadowNative.count());
        assertEquals("nativeKey", ShadowNative.calls().get(0).name());
        assertEquals(Native.KEY_ENTER, ShadowNative.calls().get(0).intArg(0));
        assertTrue(ShadowNative.calls().get(0).boolArg(1));
        assertFalse(ShadowNative.calls().get(1).boolArg(1));
    }

    @Test
    public void a_touch_that_leaves_the_button_presses_nothing() {
        // A finger that slides off the button before it lifts is not a click (the framework turns
        // that into a cancel): the game must hear nothing, or sliding across the strip would fire
        // keys.
        Button b = button(entryWithKey('C'));
        b.layout(0, 0, b.getLayoutParams().width, b.getLayoutParams().height);

        ShadowNative.clear();
        b.dispatchTouchEvent(MotionEvent.obtain(2000L, 2000L, MotionEvent.ACTION_DOWN, 10f, 10f, 0));
        b.dispatchTouchEvent(MotionEvent.obtain(2000L, 2100L, MotionEvent.ACTION_CANCEL, 400f, 40f, 0));
        Shadows.shadowOf(Looper.getMainLooper()).idle();
        assertEquals(0, ShadowNative.count());
    }

    // -------------------------------------------------------------------------------------------
    // The game's lifecycle
    // -------------------------------------------------------------------------------------------
    @Test
    public void the_activity_starts_the_game_and_ends_it_on_destroy() {
        ShadowNative.clear();
        ActivityController<ScacelithActivity> controller = Robolectric.buildActivity(ScacelithActivity.class);
        ScacelithActivity started = controller.setup().get();

        // onCreate introduces the activity to the game and starts its loop, then onResume unparks it
        // (the loop is only ever started once: a second call would be a second game).
        List<String> names = ShadowNative.names();
        assertEquals(Arrays.asList("nativeInit", "nativeStart", "nativeResume"), names.subList(0, 3));
        assertEquals(1, ShadowNative.of("nativeStart").size());
        // nativeInit is handed the activity (the game calls back into it for the clipboard, the toast
        // and the locale) and the private folder it keeps its settings and its log in.
        assertSame(started, ShadowNative.calls().get(0).objectArg(0));
        assertEquals(started.getFilesDir().getAbsolutePath(), ShadowNative.calls().get(0).objectArg(1));

        // A call or a home press parks the loop instead of ending it: a game in progress survives.
        ShadowNative.clear();
        controller.pause();
        assertEquals(Arrays.asList("nativePause"), ShadowNative.names());
        ShadowNative.clear();
        controller.resume();
        assertEquals(Arrays.asList("nativeResume"), ShadowNative.names());

        // Being destroyed is the one that ends it, and it happens before the activity is torn down.
        ShadowNative.clear();
        controller.destroy();
        assertEquals(Arrays.asList("nativeShutdown"), ShadowNative.names());
    }

    // -------------------------------------------------------------------------------------------
    // The keys the activity does not take
    // -------------------------------------------------------------------------------------------
    @Test
    public void the_activity_leaves_the_keys_the_game_has_no_use_for_to_the_system() {
        // The activity has no onKeyDown at all, on purpose: it answers the superclass's false, so a
        // key the game has no binding for (the volume rocker) keeps the meaning it has in every
        // other app, and nothing of it reaches the game. An override here that swallowed the key,
        // or forwarded it, would fail this.
        ShadowNative.clear();
        assertFalse(activity.onKeyDown(KeyEvent.KEYCODE_VOLUME_UP,
                                       new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_VOLUME_UP)));
        assertFalse(activity.onKeyDown(KeyEvent.KEYCODE_VOLUME_DOWN,
                                       new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_VOLUME_DOWN)));
        assertEquals(0, ShadowNative.count());
    }
}
