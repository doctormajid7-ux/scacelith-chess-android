package com.scacelith.game;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertSame;
import static org.junit.Assert.assertTrue;

import android.graphics.Canvas;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.SurfaceTexture;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

import java.util.Arrays;

import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.Shadows;
import org.robolectric.annotation.Config;
import org.robolectric.shadows.ShadowSurfaceView;

/**
 * GameView as a SurfaceView: the surface goes to the native layer when it appears, its size when it
 * changes, and nothing when it goes away (SurfaceHolder.Callback, platform_android.cpp).
 *
 * The game thread parks itself when the surface is destroyed (the activity going to the background)
 * and adopts the new one when it comes back, so the order and the arguments of those four calls are
 * what this file is about. EGL is not created here: that is inside the library, which a JVM test
 * cannot load.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 35, shadows = {ShadowNative.class})
public class GameViewSurfaceTest {

    private GameView view;
    private SurfaceHolder holder;

    @Before
    public void setUp() {
        ScacelithActivity activity = Robolectric.buildActivity(ScacelithActivity.class).setup().get();
        android.view.ViewGroup root = (android.view.ViewGroup)
                ((android.widget.FrameLayout) activity.findViewById(android.R.id.content)).getChildAt(0);
        view = (GameView) root.getChildAt(0);
        holder = new SurfaceHolderStub();
        // Whatever the activity and the first layout said is the game's business and was said
        // before these tests look: the log starts empty. (In a JVM test the holder is never told
        // that a surface appeared -- ShadowSurfaceView keeps no surface -- so the lifecycle below is
        // the one this file drives by hand, which is what a device would drive.)
        ShadowNative.clear();
    }

    @After
    public void tearDown() {
        // The surface is a real Surface and Android holds it to account (CloseGuard): released when
        // the test is done with it, or the run logs a leak per test.
        ((SurfaceHolderStub) holder).release();
    }

    /** A holder with a surface of its own: the framework's has none in a JVM test (it is null). */
    @SuppressWarnings("deprecation")   // SurfaceTexture(int): the only public way to build one here
    private static final class SurfaceHolderStub implements SurfaceHolder {
        private final Surface surface = new Surface(new SurfaceTexture(0));

        @Override
        public Surface getSurface() {
            return surface;
        }

        @Override
        public void addCallback(Callback callback) {
        }

        @Override
        public void removeCallback(Callback callback) {
        }

        @Override
        public boolean isCreating() {
            return false;
        }

        @Override
        public void setType(int type) {
        }

        @Override
        public void setFixedSize(int width, int height) {
        }

        @Override
        public void setSizeFromLayout() {
        }

        @Override
        public void setFormat(int format) {
        }

        @Override
        public void setKeepScreenOn(boolean screenOn) {
        }

        @Override
        public Canvas lockCanvas() {
            return null;
        }

        @Override
        public Canvas lockCanvas(Rect dirty) {
            return null;
        }

        @Override
        public void unlockCanvasAndPost(Canvas canvas) {
        }

        @Override
        public Rect getSurfaceFrame() {
            return null;
        }

        void release() {
            surface.release();
        }
    }

    @Test
    public void the_view_listens_to_its_own_holder() {
        // The constructor's getHolder().addCallback(this): without it the game would never be given
        // a surface at all, and nothing below would ever be called on a device.
        ShadowSurfaceView shadow = Shadows.shadowOf((SurfaceView) view);
        assertTrue("the view did not register itself with its holder",
                   shadow.getFakeSurfaceHolder().getCallbacks().contains(view));
        assertEquals(1, shadow.getFakeSurfaceHolder().getCallbacks().size());
    }

    @Test
    public void a_created_surface_is_handed_to_the_native_layer() {
        view.surfaceCreated(holder);
        assertEquals(1, ShadowNative.count());
        assertEquals("nativeSurfaceCreated", ShadowNative.last().name());
        assertSame("the game is given the surface the holder has", holder.getSurface(),
                   ShadowNative.last().objectArg(0));
    }

    @Test
    public void a_changed_surface_reports_its_size() {
        view.surfaceChanged(holder, PixelFormat.RGBA_8888, 1920, 1080);
        assertEquals(1, ShadowNative.count());
        assertEquals("nativeSurfaceChanged", ShadowNative.last().name());
        assertEquals(1920, ShadowNative.last().intArg(0));
        assertEquals(1080, ShadowNative.last().intArg(1));
    }

    @Test
    public void a_resize_is_reported_with_the_new_size() {
        // Android calls onSizeChanged with (new, old), and onSizeChanged forwards it to the same
        // native call: the arguments must be the new size, or the viewport of the game would be the
        // one it had before the rotation.
        view.onSizeChanged(2340, 1080, 1080, 2340);
        assertEquals("nativeSurfaceChanged", ShadowNative.last().name());
        assertEquals(2340, ShadowNative.last().intArg(0));
        assertEquals(1080, ShadowNative.last().intArg(1));
    }

    @Test
    public void a_destroyed_surface_tells_the_native_layer_to_park() {
        view.surfaceDestroyed(holder);
        assertEquals(1, ShadowNative.count());
        assertEquals("nativeSurfaceDestroyed", ShadowNative.last().name());
    }

    @Test
    public void a_real_lifecycle_arrives_in_order() {
        // What a device does: the surface is created, then sized (both surfaceChanged and
        // onSizeChanged fire for the same resize -- the native side resizes the EGL surface twice,
        // which is harmless and is what the two calls below are), then the activity goes to the
        // background and comes back, which creates the surface again.
        view.surfaceCreated(holder);
        view.surfaceChanged(holder, PixelFormat.RGBA_8888, 1920, 1080);
        view.onSizeChanged(1920, 1080, 0, 0);
        view.surfaceDestroyed(holder);
        view.surfaceCreated(holder);

        assertEquals(Arrays.asList("nativeSurfaceCreated", "nativeSurfaceChanged", "nativeSurfaceChanged",
                                   "nativeSurfaceDestroyed", "nativeSurfaceCreated"),
                     ShadowNative.names());
        // Both size reports carry the size the surface now has, and the same surface goes back.
        assertEquals("nativeSurfaceChanged", ShadowNative.calls().get(1).name());
        assertEquals("nativeSurfaceChanged", ShadowNative.calls().get(2).name());
        assertEquals(1920, ShadowNative.calls().get(1).intArg(0));
        assertEquals(1080, ShadowNative.calls().get(1).intArg(1));
        assertEquals(1920, ShadowNative.calls().get(2).intArg(0));
        assertEquals(1080, ShadowNative.calls().get(2).intArg(1));
        assertSame(holder.getSurface(), ShadowNative.calls().get(4).objectArg(0));
    }
}
