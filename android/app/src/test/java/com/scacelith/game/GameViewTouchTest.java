package com.scacelith.game;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import android.view.MotionEvent;

import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

/**
 * GameView's touch translation: Android's MotionEvent in, the game's (action, pointer id, position,
 * how many pointers are down) out (src/platform/platform_android.h, touch_input.cpp).
 *
 * The gesture machine itself is the native layer's and is tested there (tests/touch_input_tests.cpp);
 * what this checks is the half that stays in Java, and in particular which pointer of the event each
 * action is about: DOWN, POINTER_DOWN, UP and POINTER_UP carry one pointer -- event.getActionIndex()
 * -- and MOVE carries them all.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 35, shadows = {ShadowNative.class})
public class GameViewTouchTest {

    private GameView view;

    @Before
    public void setUp() {
        ScacelithActivity activity = Robolectric.buildActivity(ScacelithActivity.class).setup().get();
        android.view.ViewGroup root = (android.view.ViewGroup)
                ((android.widget.FrameLayout) activity.findViewById(android.R.id.content)).getChildAt(0);
        view = (GameView) root.getChildAt(0);
        // The activity's own startup (nativeInit, nativeStart, nativeResume) and the first layout of
        // the view are not what this file is about, and the native layer has already heard them:
        // from here the log holds a touch, and nothing else.
        ShadowNative.clear();
    }

    // -------------------------------------------------------------------------------------------
    // What the assertions compare
    // -------------------------------------------------------------------------------------------
    /** The (action, id, x, y, count) the game was told, as a string: what the assertions compare. */
    private static String touchCall(int index) {
        ShadowNative.Call c = ShadowNative.of("nativeTouch").get(index);
        return c.intArg(0) + "," + c.intArg(1) + "," + c.floatArg(2) + "," + c.floatArg(3) + ","
                + c.intArg(4);
    }

    private static String sent(int action, int id, float x, float y, int count) {
        return action + "," + id + "," + x + "," + y + "," + count;
    }

    // -------------------------------------------------------------------------------------------
    // One pointer per action
    // -------------------------------------------------------------------------------------------
    @Test
    public void a_finger_down_is_the_left_button() {
        assertTrue(view.onTouchEvent(Touches.event(MotionEvent.ACTION_DOWN, new int[]{7}, new float[]{10f}, new float[]{20f})));
        assertEquals(1, ShadowNative.count());
        assertEquals(sent(MotionEvent.ACTION_DOWN, 7, 10f, 20f, 1), touchCall(0));
    }

    @Test
    public void a_finger_up_is_the_same_pointer() {
        view.onTouchEvent(Touches.event(MotionEvent.ACTION_DOWN, new int[]{7}, new float[]{10f}, new float[]{20f}));
        ShadowNative.clear();
        assertTrue(view.onTouchEvent(Touches.event(MotionEvent.ACTION_UP, new int[]{7}, new float[]{12f}, new float[]{22f})));
        assertEquals(sent(MotionEvent.ACTION_UP, 7, 12f, 22f, 1), touchCall(0));
    }

    @Test
    public void a_second_finger_down_is_the_last_pointer_with_its_own_id() {
        view.onTouchEvent(Touches.event(MotionEvent.ACTION_DOWN, new int[]{3}, new float[]{10f}, new float[]{20f}));
        ShadowNative.clear();
        // The two pointers are at different places, so an implementation that reported the first
        // one's position (or got the id wrong) would be caught here.
        assertTrue(view.onTouchEvent(Touches.event(Touches.aboutPointer(MotionEvent.ACTION_POINTER_DOWN, 1),
                                           new int[]{3, 9}, new float[]{10f, 44f}, new float[]{20f, 55f})));
        assertEquals(1, ShadowNative.count());
        assertEquals(sent(MotionEvent.ACTION_POINTER_DOWN, 9, 44f, 55f, 2), touchCall(0));
    }

    @Test
    public void the_action_index_is_what_names_the_pointer_not_the_last_one() {
        // Three fingers down, and the event is about the middle one. In a real stream the pointer
        // that goes down or up is the last of the array, so this event is not one Android builds --
        // which is exactly why it is worth pinning: the code must read getActionIndex(), not
        // getPointerCount() - 1.
        ShadowNative.clear();
        assertTrue(view.onTouchEvent(Touches.event(Touches.aboutPointer(MotionEvent.ACTION_POINTER_DOWN, 1),
                                           new int[]{1, 2, 3}, new float[]{10f, 20f, 30f},
                                           new float[]{11f, 22f, 33f})));
        assertEquals(1, ShadowNative.count());
        assertEquals("the second finger, not the third",
                     sent(MotionEvent.ACTION_POINTER_DOWN, 2, 20f, 22f, 3), touchCall(0));
    }

    @Test
    public void the_pointer_that_went_up_is_the_action_index() {
        ShadowNative.clear();
        assertTrue(view.onTouchEvent(Touches.event(Touches.aboutPointer(MotionEvent.ACTION_POINTER_UP, 1),
                                           new int[]{4, 5, 6}, new float[]{1f, 2f, 3f},
                                           new float[]{4f, 5f, 6f})));
        assertEquals(sent(MotionEvent.ACTION_POINTER_UP, 5, 2f, 5f, 3), touchCall(0));
    }

    @Test
    public void a_move_reports_every_pointer_it_carries() {
        ShadowNative.clear();
        assertTrue(view.onTouchEvent(Touches.event(MotionEvent.ACTION_MOVE, new int[]{5, 6, 7},
                                           new float[]{1f, 2f, 3f}, new float[]{4f, 5f, 6f})));
        assertEquals(3, ShadowNative.count());
        assertEquals(sent(MotionEvent.ACTION_MOVE, 5, 1f, 4f, 3), touchCall(0));
        assertEquals(sent(MotionEvent.ACTION_MOVE, 6, 2f, 5f, 3), touchCall(1));
        assertEquals(sent(MotionEvent.ACTION_MOVE, 7, 3f, 6f, 3), touchCall(2));
    }

    @Test
    public void a_move_reports_the_pointers_in_the_order_of_the_event() {
        // The native layer tracks the pointers by their ids, so the order only has to be the event's
        // -- but it has to be *every* pointer, and only once each.
        view.onTouchEvent(Touches.event(MotionEvent.ACTION_MOVE, new int[]{20, 10}, new float[]{1f, 2f},
                                new float[]{3f, 4f}));
        assertEquals(2, ShadowNative.count());
        assertEquals(20, ShadowNative.of("nativeTouch").get(0).intArg(1));
        assertEquals(10, ShadowNative.of("nativeTouch").get(1).intArg(1));
    }

    // -------------------------------------------------------------------------------------------
    // The whole stream
    // -------------------------------------------------------------------------------------------
    @Test
    public void a_two_finger_drag_is_sent_as_it_happens() {
        // down, second finger down, both drag, second lifts, first lifts: five events, six calls
        // (the drag carries both pointers at once), each with how many were down at the time.
        view.onTouchEvent(Touches.event(MotionEvent.ACTION_DOWN, new int[]{1}, new float[]{100f}, new float[]{100f}));
        view.onTouchEvent(Touches.event(Touches.aboutPointer(MotionEvent.ACTION_POINTER_DOWN, 1), new int[]{1, 2},
                                new float[]{100f, 200f}, new float[]{100f, 200f}));
        view.onTouchEvent(Touches.event(MotionEvent.ACTION_MOVE, new int[]{1, 2}, new float[]{101f, 201f},
                                new float[]{101f, 201f}));
        view.onTouchEvent(Touches.event(Touches.aboutPointer(MotionEvent.ACTION_POINTER_UP, 1), new int[]{1, 2},
                                new float[]{102f, 202f}, new float[]{102f, 202f}));
        view.onTouchEvent(Touches.event(MotionEvent.ACTION_UP, new int[]{1}, new float[]{103f}, new float[]{103f}));

        assertEquals(6, ShadowNative.of("nativeTouch").size());
        for (ShadowNative.Call c : ShadowNative.calls()) assertEquals("nativeTouch", c.name());
        assertEquals(sent(MotionEvent.ACTION_DOWN, 1, 100f, 100f, 1), touchCall(0));
        assertEquals(sent(MotionEvent.ACTION_POINTER_DOWN, 2, 200f, 200f, 2), touchCall(1));
        assertEquals(sent(MotionEvent.ACTION_MOVE, 1, 101f, 101f, 2), touchCall(2));
        assertEquals(sent(MotionEvent.ACTION_MOVE, 2, 201f, 201f, 2), touchCall(3));
        assertEquals(sent(MotionEvent.ACTION_POINTER_UP, 2, 202f, 202f, 2), touchCall(4));
        assertEquals(sent(MotionEvent.ACTION_UP, 1, 103f, 103f, 1), touchCall(5));
    }

    @Test
    public void a_cancelled_stream_forgets_the_pointers() {
        // A gesture the system took back (a call arriving, the shade pulled down): the native layer
        // drops every pointer, so the event carries none of them -- a zeroed id, position and count.
        assertTrue(view.onTouchEvent(Touches.event(MotionEvent.ACTION_CANCEL, new int[]{7, 8},
                                           new float[]{10f, 20f}, new float[]{30f, 40f})));
        assertEquals(1, ShadowNative.count());
        assertEquals(sent(MotionEvent.ACTION_CANCEL, 0, 0f, 0f, 0), touchCall(0));
    }

    @Test
    public void an_action_the_game_has_no_use_for_is_not_sent() {
        assertTrue("the view keeps the stream", view.onTouchEvent(Touches.event(MotionEvent.ACTION_OUTSIDE,
                                                                       new int[]{7}, new float[]{1f}, new float[]{2f})));
        assertEquals(0, ShadowNative.count());
    }

    // -------------------------------------------------------------------------------------------
    // A mouse
    // -------------------------------------------------------------------------------------------
    @Test
    public void a_mouse_wheel_leans_towards_the_board() {
        MotionEvent wheel = Touches.mouseScroll(1.5f, 500f, 400f);
        try {
            assertTrue(view.onGenericMotionEvent(wheel));
            assertEquals(1, ShadowNative.count());
            assertEquals("nativeWheel", ShadowNative.last().name());
            assertEquals(1.5f, ShadowNative.last().floatArg(0), 0f);
        } finally {
            wheel.recycle();
        }
    }

    @Test
    public void a_finger_is_not_a_wheel() {
        // The same motion from a touchscreen: it is not a lean, it is a drag, and it goes to
        // onTouchEvent -- onGenericMotionEvent must leave it alone.
        view.onGenericMotionEvent(Touches.event(MotionEvent.ACTION_MOVE, new int[]{1}, new float[]{1f}, new float[]{2f}));
        assertEquals(0, ShadowNative.count());
    }
}
