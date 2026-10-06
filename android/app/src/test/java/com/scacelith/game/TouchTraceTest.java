package com.scacelith.game;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import android.view.MotionEvent;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Random;
import java.util.Set;

import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

/**
 * The two halves of the Android touch input, joined by a file.
 *
 * A touch crosses two languages that no test of either one can hold alone: Java turns the
 * framework's MotionEvents into (action, pointer id, position, how many fingers are down) and pushes
 * them into the native layer (GameView, platform_android.cpp), and the C++ gesture machine turns
 * them into the mouse and keyboard state the game was written against (src/platform/touch_input.cpp).
 * GameViewTouchTest and touch_input_tests.cpp each check their own half against what the other side
 * is *supposed* to say; neither can check that the two actually fit, because neither can run the
 * other's toolchain -- the first needs Robolectric, the second needs the game's C++.
 *
 * So this writes the seam down. It drives random gestures through the real View (Robolectric, no
 * device) and records, frame by frame, exactly what the View sent the native layer -- taken from the
 * recorded calls, not from what the generator meant to send. tests/touch_bridge_tests.cpp then
 * replays that file through plat::TouchInput and checks what the game would have received: that a
 * finger drags the cursor by the distance Java reported, that a pinch leans by the size the View
 * reported, that a tap is a click even when it arrives whole inside one frame, that a two-finger tap
 * is the clock and a three-finger one the menu, and that no gesture of any shape leaves a button
 * held or a piece grabbed.
 *
 * The trace goes where the desktop harness looks for it (see tracePath()). It is a build artefact --
 * a fresh trace is written by every run of this test, and the desktop test skips when there is no
 * file at all.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 35, shadows = {ShadowNative.class})
public class TouchTraceTest {

    /** The size the View is told it has: landscape, as the manifest asks for. */
    private static final int WIDTH = 1920, HEIGHT = 1080;
    /** Fixed, so a run writes the same trace; the desktop side replays the file, not the seed. */
    private static final long SEED = 0x5CA0E11L;
    /** Each kind of gesture is written this many times, with fresh coordinates and directions. */
    private static final int REPEATS = 3;
    /** The kinds, in the order they are written: the desktop test knows these names too. */
    private static final List<String> KINDS = Arrays.asList(
            "tap", "tap_in_one_frame", "drag", "look", "look_lift_one", "pinch_out", "pinch_in",
            "two_finger_tap", "three_finger_tap", "cancel", "grab_then_look", "button", "wheel");

    private GameView view;
    private LinearLayout overlay;

    @Before
    public void setUp() {
        ScacelithActivity activity = Robolectric.buildActivity(ScacelithActivity.class).setup().get();
        ViewGroup root = (ViewGroup)
                ((FrameLayout) activity.findViewById(android.R.id.content)).getChildAt(0);
        view = (GameView) root.getChildAt(0);
        overlay = (LinearLayout) root.getChildAt(1);
    }

    /** Where the trace goes: the file tests/touch_bridge_tests.cpp reads (via repo_files.h). */
    private static Path tracePath() {
        String override = System.getProperty("scacelith.touch.trace");
        if (override != null && !override.isEmpty()) return Paths.get(override);
        // Gradle runs the unit tests from the module directory (android/app), so the repository root
        // is two levels up, and build/ is where the desktop harness looks (it is not committed).
        return Paths.get("..", "..", "build", "android-touch-trace.txt").toAbsolutePath().normalize();
    }

    @Test
    public void writes_the_trace_the_desktop_harness_replays() throws IOException {
        // The size the View reports to the native layer, taken from the call it makes rather than
        // assumed: the trace carries it on its viewport line, the desktop side sets the gesture
        // machine's viewport from it, and the pinch's pixels-per-notch comes from that number. A View
        // that stopped reporting its size, or reported it transposed, shows up in the replay.
        ShadowNative.clear();
        view.onSizeChanged(WIDTH, HEIGHT, 0, 0);
        List<ShadowNative.Call> reported = ShadowNative.of("nativeSurfaceChanged");
        assertEquals(1, reported.size());
        assertEquals(WIDTH, reported.get(0).intArg(0));
        assertEquals(HEIGHT, reported.get(0).intArg(1));
        ShadowNative.clear();

        Session session = new Session(new Random(SEED), WIDTH, HEIGHT);
        for (int repeat = 0; repeat < REPEATS; ++repeat) {
            session.tap();
            session.tapInOneFrame();
            session.drag();
            session.look();
            session.lookLiftingOneFinger();
            session.pinch(true);
            session.pinch(false);
            session.twoFingerTap();
            session.threeFingerTap();
            session.cancel();
            session.grabThenLook();
            session.spaceButton();
            session.mouseWheel();
        }

        // The file is written first, whatever the checks below say: it is the record of what the
        // Java half just did, and the desktop half replays exactly that. A broken View writes a
        // broken trace, and the replay says so in the language of the game (a look that grabs a
        // piece, a two-finger tap that presses nothing) rather than in the language of arguments.
        final String trace = session.trace.toString();
        Path path = tracePath();
        Files.createDirectories(path.getParent());
        Files.write(path, trace.getBytes(StandardCharsets.UTF_8));
        System.out.println("touch trace: " + path + " (" + trace.length() + " bytes)");

        // And it has to be worth replaying: every action GameView forwards, every kind of gesture,
        // a frame marker per step (the desktop side only runs the gesture machine at those), and not
        // one line the desktop parser would not recognise.
        Set<Integer> actions = new LinkedHashSet<>();
        int scenarios = 0, frames = 0, touches = 0;
        for (String line : trace.split("\n")) {
            if (line.startsWith("scenario ")) ++scenarios;
            else if (line.equals("frame")) ++frames;
            else if (line.startsWith("touch ")) {
                ++touches;
                actions.add(Integer.parseInt(line.split(" ")[1]));
            } else if (!line.isEmpty() && !line.startsWith("#") && !line.startsWith("viewport ")
                       && !line.startsWith("key ") && !line.startsWith("wheel ")) {
                throw new AssertionError("a line the desktop parser does not know: " + line);
            }
        }
        assertEquals(KINDS.size() * REPEATS, scenarios);
        assertTrue("not enough frames", frames > scenarios * 2);
        assertTrue("not enough touches", touches > scenarios * 3);
        // DOWN, UP, MOVE, CANCEL, POINTER_DOWN, POINTER_UP: the whole vocabulary GameView forwards.
        assertEquals(Set.of(0, 1, 2, 3, 5, 6), actions);
    }

    /** One finger of the generator: the trace carries its id and its position. */
    private static final class Finger {
        final int id;
        float x, y;

        Finger(int id, float x, float y) {
            this.id = id;
            this.x = x;
            this.y = y;
        }
    }

    /**
     * A run of the generator: real MotionEvents through the View, and what the View sent the native
     * layer written out line by line.
     *
     * A frame() marker is this side's model of one iteration of the game's loop: everything
     * dispatched between two markers arrived as one batch, which is exactly what a quick tap is
     * (down and up before the game thread looked at the queue).
     */
    private final class Session {
        final StringBuilder trace = new StringBuilder();
        private final Random rnd;
        private final int width, height;
        private final List<Finger> fingers = new ArrayList<>();
        private int written = 0;

        Session(Random rnd, int width, int height) {
            this.rnd = rnd;
            this.width = width;
            this.height = height;
            trace.append("# Scacelith Android touch trace: what GameView sent the native layer, frame by\n");
            trace.append("# frame, for tests/touch_bridge_tests.cpp to replay through plat::TouchInput.\n");
            trace.append("# Written by android/app/src/test/java/com/scacelith/game/TouchTraceTest.java.\n");
            trace.append("viewport ").append(width).append(' ').append(height).append('\n');
        }

        // ---------------------------------------------------------------------------------------
        // The gestures
        // ---------------------------------------------------------------------------------------
        void tap() {
            scenario("tap");
            put(1, x(), y());
            frame();
            liftLast();
            frame();
            frame();
        }

        void tapInOneFrame() {
            // A whole tap inside one frame, as a quick finger gives it. The native layer has to hold
            // the release back for the next frame, or the game never sees the click at all.
            scenario("tap_in_one_frame");
            put(1, x(), y());
            liftLast();
            frame();
            frame();
            frame();
        }

        void drag() {
            scenario("drag");
            put(1, x(), y());
            frame();
            for (int i = 0; i < 3; ++i) {
                moveAll(stepX(), stepY());
                frame();
            }
            liftLast();
            frame();
            frame();
        }

        void look() {
            scenario("look");
            final float x = x(), y = y();
            put(1, x, y);
            put(2, x + 300, y + 40);   // both in one frame: two fingers arriving together never grab
            frame();
            for (int i = 0; i < 3; ++i) {
                moveAll(stepX(), stepY());   // both fingers, the same vector: the view turns
                frame();
            }
            liftAll();   // and both leave together, without the grip changing: not a tap
            frame();
            frame();
        }

        void lookLiftingOneFinger() {
            // What a hand does when it stops looking: one finger goes first. The finger left behind is
            // a drag as far as the native layer is concerned, which is the game's behaviour today --
            // the replay walks it and says so.
            scenario("look_lift_one");
            final float x = x(), y = y();
            put(1, x, y);
            put(2, x + 300, y + 40);
            frame();
            for (int i = 0; i < 3; ++i) {
                moveAll(stepX(), stepY());
                frame();
            }
            liftLast();
            frame();
            liftLast();
            frame();
            frame();
        }

        void pinch(boolean apart) {
            scenario(apart ? "pinch_out" : "pinch_in");
            final float cx = x(), cy = y();
            float half = 200;
            put(1, cx - half, cy);
            put(2, cx + half, cy);
            frame();
            for (int i = 0; i < 3; ++i) {
                half += apart ? 60 : -45;
                moveTo(2, cx + half, cy);   // one finger opens or closes the grip: the wheel
                frame();
            }
            liftAll();
            frame();
            frame();
        }

        void twoFingerTap() {
            scenario("two_finger_tap");
            final float x = x(), y = y();
            put(1, x, y);
            put(2, x + 240, y + 60);
            frame();
            liftAll();   // both up in one frame, without having moved: the clock
            frame();
            frame();
        }

        void threeFingerTap() {
            scenario("three_finger_tap");
            final float x = x(), y = y();
            put(1, x, y);
            put(2, x + 200, y + 40);
            put(3, x + 400, y + 80);
            frame();
            liftAll();
            frame();
            frame();
        }

        void cancel() {
            scenario("cancel");
            put(1, x(), y());
            frame();
            moveAll(stepX(), stepY());
            frame();
            dispatch(MotionEvent.ACTION_CANCEL);   // the system took the gesture over
            fingers.clear();
            frame();
            frame();
        }

        void grabThenLook() {
            scenario("grab_then_look");
            put(1, x(), y());
            frame();   // the piece is picked up
            moveAll(stepX(), stepY());
            frame();   // carried
            put(2, fingers.get(0).x + 300, fingers.get(0).y);
            frame();   // the second finger hands it back and the look starts
            for (int i = 0; i < 2; ++i) {
                moveAll(stepX(), stepY());
                frame();
            }
            liftAll();
            frame();
            frame();
        }

        void spaceButton() {
            // The overlay's clock button, pressed the way a finger presses it: the same path Tab, C,
            // S, Enter and Backspace take. Both edges of the key land in one frame, which is what the
            // game's widgets need to see a click.
            scenario("button");
            for (int i = 0; i < OverlayButtons.ALL.length; i++)
                if (OverlayButtons.ALL[i].key == Native.KEY_SPACE)
                    ((Button) overlay.getChildAt(i)).performClick();
            flush();
            frame();
            frame();
        }

        void mouseWheel() {
            // A phone in a dock: the wheel is a lean towards the board, and it arrives through
            // onGenericMotionEvent rather than through a touch.
            scenario("wheel");
            view.onGenericMotionEvent(Touches.mouseScroll(1.5f, 500f, 400f));
            flush();
            frame();
            frame();
        }

        // ---------------------------------------------------------------------------------------
        // Dispatching, and writing down what came of it
        // ---------------------------------------------------------------------------------------
        private void scenario(String kind) {
            trace.append("scenario ").append(kind).append('\n');
        }

        /** One iteration of the game's loop: the events pushed since the last one are one batch. */
        private void frame() {
            trace.append("frame\n");
        }

        /** A new finger down: ACTION_DOWN for the first, ACTION_POINTER_DOWN for the others. */
        private void put(int id, float x, float y) {
            fingers.add(new Finger(id, x, y));
            dispatch(fingers.size() == 1
                             ? MotionEvent.ACTION_DOWN
                             : Touches.aboutPointer(MotionEvent.ACTION_POINTER_DOWN, fingers.size() - 1));
        }

        private void lift(int id) {
            final int index = indexOf(id);
            dispatch(fingers.size() == 1
                             ? MotionEvent.ACTION_UP
                             : Touches.aboutPointer(MotionEvent.ACTION_POINTER_UP, index));
            fingers.remove(index);
        }

        private void liftLast() {
            lift(fingers.get(fingers.size() - 1).id);
        }

        /** Every finger up inside one frame: what a hand taking two or three fingers away gives. */
        private void liftAll() {
            while (!fingers.isEmpty()) liftLast();
        }

        private void moveTo(int id, float x, float y) {
            for (Finger f : fingers)
                if (f.id == id) {
                    f.x = x;
                    f.y = y;
                }
            dispatch(MotionEvent.ACTION_MOVE);
        }

        private void moveAll(float dx, float dy) {
            for (Finger f : fingers) {
                f.x += dx;
                f.y += dy;
            }
            dispatch(MotionEvent.ACTION_MOVE);
        }

        private int indexOf(int id) {
            for (int i = 0; i < fingers.size(); i++)
                if (fingers.get(i).id == id) return i;
            throw new AssertionError("no finger " + id);
        }

        /** One MotionEvent carrying every finger that is down, then what it made the View send. */
        private void dispatch(int action) {
            final int n = fingers.size();
            int[] ids = new int[n];
            float[] xs = new float[n], ys = new float[n];
            for (int i = 0; i < n; i++) {
                ids[i] = fingers.get(i).id;
                xs[i] = fingers.get(i).x;
                ys[i] = fingers.get(i).y;
            }
            view.onTouchEvent(Touches.event(action, ids, xs, ys));
            flush();
        }

        /** What the View sent since this last ran, as the trace's lines. */
        private void flush() {
            List<ShadowNative.Call> calls = ShadowNative.calls();
            for (; written < calls.size(); ++written) {
                ShadowNative.Call c = calls.get(written);
                if (c.name().equals("nativeTouch")) {
                    trace.append("touch ").append(c.intArg(0)).append(' ').append(c.intArg(1)).append(' ')
                         .append(number(c.floatArg(2))).append(' ').append(number(c.floatArg(3))).append(' ')
                         .append(c.intArg(4)).append('\n');
                } else if (c.name().equals("nativeKey")) {
                    trace.append("key ").append(c.intArg(0)).append(c.boolArg(1) ? " down" : " up").append('\n');
                } else if (c.name().equals("nativeWheel")) {
                    trace.append("wheel ").append(number(c.floatArg(0))).append('\n');
                } else {
                    // A call the trace cannot carry would be a silent hole in the replay.
                    throw new AssertionError("the View sent something the trace cannot carry: " + c);
                }
            }
        }

        /** A float the desktop parser reads back exactly (Float.toString round-trips). */
        private String number(float v) {
            return Float.toString(v);
        }

        // ---------------------------------------------------------------------------------------
        // Where the gestures go
        // ---------------------------------------------------------------------------------------
        private float x() {
            return 40 + rnd.nextInt(width - 80) + rnd.nextFloat();
        }

        private float y() {
            return 40 + rnd.nextInt(height - 80) + rnd.nextFloat();
        }

        private float stepX() {
            return (rnd.nextBoolean() ? 1 : -1) * (8 + rnd.nextInt(18));
        }

        private float stepY() {
            return (rnd.nextBoolean() ? 1 : -1) * (4 + rnd.nextInt(12));
        }
    }
}
