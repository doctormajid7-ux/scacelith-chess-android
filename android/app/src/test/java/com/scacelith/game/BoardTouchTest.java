package com.scacelith.game;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import android.os.Looper;
import android.view.MotionEvent;
import android.view.View;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;

import java.util.List;

import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.Shadows;
import org.robolectric.annotation.Config;

/**
 * Where a finger actually lands, once the window has been laid out.
 *
 * ScacelithActivity stacks two Views in a FrameLayout: the game's SurfaceView, full screen, and the
 * strip of buttons over its bottom-right corner. The tests in ScacelithActivityTest send their
 * events straight to a View; this one goes through the FrameLayout, the way the framework delivers a
 * real finger, so that <em>hit testing</em> is what decides who gets the touch. That is the part a
 * device would be needed for otherwise, and the part that can go wrong silently: a strip that grows
 * over the board, or that swallows touches it has no button for, would take gestures away from the
 * game without anything failing.
 *
 * The layout matters, so every test here starts with a real measure/layout pass at a landscape size
 * (before it, every View is 0 by 0 and a hit test would be meaningless), and the coordinates are
 * read back from the laid-out Views rather than written down.
 *
 * The negative half is what keeps the positive half honest: a touch that does land on a button must
 * reach the button and <em>not</em> also play the board. Without that, a test that found the game
 * behind the strip would pass even if the strip had stopped working altogether.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 35, shadows = {ShadowNative.class})
public class BoardTouchTest {

    /** A landscape window: the orientation the activity asks for. */
    private static final int SCREEN_W = 1920;
    private static final int SCREEN_H = 1080;

    private FrameLayout root;
    private GameView board;
    private LinearLayout strip;

    @Before
    public void setUp() {
        ScacelithActivity activity = Robolectric.buildActivity(ScacelithActivity.class).setup().get();
        FrameLayout content = (FrameLayout) activity.findViewById(android.R.id.content);
        root = (FrameLayout) content.getChildAt(0);
        assertEquals("the game view and the overlay are the two children of the root", 2, root.getChildCount());

        // The pass the framework does before anything can be touched. Without it the root is 0 by 0,
        // the strip is 0 by 0 too, and every hit test below would be a coin toss.
        root.measure(View.MeasureSpec.makeMeasureSpec(SCREEN_W, View.MeasureSpec.EXACTLY),
                     View.MeasureSpec.makeMeasureSpec(SCREEN_H, View.MeasureSpec.EXACTLY));
        root.layout(0, 0, SCREEN_W, SCREEN_H);

        board = (GameView) root.getChildAt(0);
        strip = (LinearLayout) root.getChildAt(1);
        ShadowNative.clear();
    }

    // -------------------------------------------------------------------------------------------
    // Sending a finger through the hierarchy
    // -------------------------------------------------------------------------------------------
    /** One event at (x, y), dispatched to the root: the children hit-test it, as on a device. */
    private void send(long downTime, long eventTime, int action, float x, float y) {
        root.dispatchTouchEvent(MotionEvent.obtain(downTime, eventTime, action, x, y, 0));
    }

    /** A finger down and up at the same place; a View's click is what the looper has to run. */
    private void tap(float x, float y) {
        send(1000L, 1000L, MotionEvent.ACTION_DOWN, x, y);
        send(1000L, 1050L, MotionEvent.ACTION_UP, x, y);
        Shadows.shadowOf(Looper.getMainLooper()).idle();
    }

    /** The middle of a button, in the root's coordinates. */
    private float[] centreOf(Button b) {
        return new float[]{strip.getLeft() + b.getLeft() + b.getWidth() / 2f,
                           strip.getTop() + b.getTop() + b.getHeight() / 2f};
    }

    private Button buttonWithKey(int key) {
        for (int i = 0; i < OverlayButtons.ALL.length; i++)
            if (OverlayButtons.ALL[i].key == key) return (Button) strip.getChildAt(i);
        throw new AssertionError("the table has no button for key " + key);
    }

    private static List<ShadowNative.Call> touches() {
        return ShadowNative.of("nativeTouch");
    }

    // -------------------------------------------------------------------------------------------
    // The layout the rest of the file leans on
    // -------------------------------------------------------------------------------------------
    @Test
    public void the_board_fills_the_window_and_the_strip_only_covers_its_corner() {
        assertEquals("the board is full screen", SCREEN_W, board.getWidth());
        assertEquals(SCREEN_H, board.getHeight());

        assertTrue("the strip is laid out", strip.getWidth() > 0 && strip.getHeight() > 0);
        assertTrue("the strip is at the bottom right",
                   strip.getLeft() >= 0 && strip.getTop() > 0
                           && strip.getRight() <= SCREEN_W && strip.getBottom() <= SCREEN_H);
        // The claim the whole file rests on: the strip is a corner, not the window. A strip laid out
        // match_parent (or a full-height bar) would take every touch below and no other test here
        // would be testing the board at all.
        assertTrue("the strip does not span the window",
                   strip.getWidth() < SCREEN_W && strip.getHeight() < SCREEN_H);
        assertTrue("the board is under the strip, not beside it", strip.getTop() > 0);
    }

    // -------------------------------------------------------------------------------------------
    // The board gets its touches
    // -------------------------------------------------------------------------------------------
    @Test
    public void a_touch_in_the_middle_of_the_board_reaches_the_game() {
        tap(SCREEN_W / 2f, SCREEN_H / 2f);

        List<ShadowNative.Call> sent = touches();
        assertEquals("a press and a release", 2, sent.size());
        assertEquals("nativeTouch", sent.get(0).name());
        assertEquals(Native.ACTION_DOWN, sent.get(0).intArg(0));
        assertEquals(0, sent.get(0).intArg(1));
        assertEquals(960f, sent.get(0).floatArg(2), 0.001f);
        assertEquals(540f, sent.get(0).floatArg(3), 0.001f);
        assertEquals("one finger", 1, sent.get(0).intArg(4));
        assertEquals(Native.ACTION_UP, sent.get(1).intArg(0));
        assertEquals("the strip pressed nothing", 0, ShadowNative.of("nativeKey").size());
    }

    @Test
    public void the_strip_does_not_take_a_touch_just_above_it() {
        // The row the strip is on, one pixel above its top edge: the board's, not the strip's. A
        // strip that had grown a background or a fixed height over the board would take it.
        float x = strip.getLeft() + strip.getWidth() / 2f;
        float y = strip.getTop() - 1f;

        tap(x, y);

        List<ShadowNative.Call> sent = touches();
        assertEquals(2, sent.size());
        assertEquals(y, sent.get(0).floatArg(3), 0.001f);
        assertEquals(0, ShadowNative.of("nativeKey").size());
    }

    @Test
    public void a_touch_between_two_buttons_is_the_boards() {
        // Inside the strip's own rectangle, in the margin between two buttons. No button claims it,
        // so it must fall through to the board underneath: a strip that answered for its whole
        // rectangle (a click listener on the bar, a background that is clickable) would eat it.
        Button left = (Button) strip.getChildAt(0);
        Button right = (Button) strip.getChildAt(1);
        assertTrue("the buttons are separated by a margin", left.getRight() < right.getLeft());

        float x = strip.getLeft() + (left.getRight() + right.getLeft()) / 2f;
        float y = strip.getTop() + left.getTop() + left.getHeight() / 2f;
        assertTrue("the point is inside the strip", x > strip.getLeft() && x < strip.getRight());

        tap(x, y);

        List<ShadowNative.Call> sent = touches();
        assertEquals(2, sent.size());
        assertEquals(Native.ACTION_DOWN, sent.get(0).intArg(0));
        assertEquals(x, sent.get(0).floatArg(2), 0.001f);
        assertEquals(0, ShadowNative.of("nativeKey").size());
    }

    @Test
    public void a_drag_that_starts_on_the_board_and_crosses_the_strip_stays_the_boards() {
        // The hazard the strip creates: a finger carrying a piece across the screen passes over the
        // buttons. The framework gives the whole gesture to whoever took the press, so the strip
        // must hear nothing -- one stray key here would be a move confirmed mid-drag.
        Button b = buttonWithKey(Native.KEY_ENTER);
        float[] target = centreOf(b);

        send(3000L, 3000L, MotionEvent.ACTION_DOWN, SCREEN_W / 2f, SCREEN_H / 2f);
        send(3000L, 3020L, MotionEvent.ACTION_MOVE, target[0], target[1]);
        send(3000L, 3040L, MotionEvent.ACTION_UP, target[0], target[1]);
        Shadows.shadowOf(Looper.getMainLooper()).idle();

        assertEquals("a drag over the strip presses no button", 0, ShadowNative.of("nativeKey").size());
        List<ShadowNative.Call> sent = touches();
        assertEquals("press, move, release", 3, sent.size());
        assertEquals(Native.ACTION_MOVE, sent.get(1).intArg(0));
        assertEquals(target[0], sent.get(1).floatArg(2), 0.001f);
        assertEquals(target[1], sent.get(1).floatArg(3), 0.001f);
        assertEquals(Native.ACTION_UP, sent.get(2).intArg(0));
        assertEquals(target[0], sent.get(2).floatArg(2), 0.001f);
        assertEquals(target[1], sent.get(2).floatArg(3), 0.001f);
    }

    // -------------------------------------------------------------------------------------------
    // And the strip still keeps its own
    // -------------------------------------------------------------------------------------------
    @Test
    public void a_touch_on_a_button_is_the_buttons_and_not_the_boards() {
        Button b = buttonWithKey(Native.KEY_SPACE);
        float[] at = centreOf(b);

        tap(at[0], at[1]);

        List<ShadowNative.Call> keys = ShadowNative.of("nativeKey");
        assertEquals("press then release", 2, keys.size());
        assertEquals(Native.KEY_SPACE, keys.get(0).intArg(0));
        assertTrue(keys.get(0).boolArg(1));
        assertEquals(Native.KEY_SPACE, keys.get(1).intArg(0));
        assertFalse(keys.get(1).boolArg(1));
        assertEquals("the button's own touch does not also play the board",
                     0, touches().size());
    }

    @Test
    public void the_button_the_touch_lands_on_is_the_one_that_answers() {
        // Two neighbours, a touch on each: the strip's own hit testing, seen from the outside.
        Button first = buttonWithKey(Native.KEY_ESCAPE);
        Button second = buttonWithKey(Native.KEY_TAB);

        float[] onFirst = centreOf(first);
        tap(onFirst[0], onFirst[1]);
        assertEquals("press then release", 2, ShadowNative.of("nativeKey").size());
        assertEquals(Native.KEY_ESCAPE, ShadowNative.of("nativeKey").get(0).intArg(0));

        ShadowNative.clear();
        float[] onSecond = centreOf(second);
        tap(onSecond[0], onSecond[1]);
        assertEquals(Native.KEY_TAB, ShadowNative.of("nativeKey").get(0).intArg(0));
        assertEquals(0, touches().size());
    }

    @Test
    public void a_touch_that_slides_off_a_button_presses_nothing() {
        // Started on a button and lifted off it: the framework cancels the click, and the board must
        // not inherit an up it never saw a down for.
        Button b = buttonWithKey('C');
        float[] at = centreOf(b);

        send(4000L, 4000L, MotionEvent.ACTION_DOWN, at[0], at[1]);
        send(4000L, 4060L, MotionEvent.ACTION_MOVE, SCREEN_W / 2f, SCREEN_H / 2f);
        send(4000L, 4080L, MotionEvent.ACTION_UP, SCREEN_W / 2f, SCREEN_H / 2f);
        Shadows.shadowOf(Looper.getMainLooper()).idle();

        assertEquals("no key from a press that slid off", 0, ShadowNative.of("nativeKey").size());
        assertEquals("and the board did not inherit it", 0, touches().size());
    }
}
