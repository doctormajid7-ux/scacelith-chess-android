package com.scacelith.game;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.view.inputmethod.InputMethodManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.Toast;

import java.util.Locale;

/**
 * The Android entry point: a full-screen SurfaceView with the game on it, and a small strip of
 * buttons for the keys a phone has no gesture for.
 *
 * The activity owns the window and the lifecycle; the game runs on its own thread inside
 * libscacelith.so and is started once, from onCreate (Native.nativeStart). It keeps running while
 * the app is in the background (parked on a condition variable, see platform_android.cpp) so that
 * a game in progress survives a call or a home press, and it is asked to end from onDestroy.
 *
 * Keys: this class intercepts none, and that is deliberate -- there is no onKeyDown or onKeyUp
 * here. The game's own keys are translated in GameView, the view that holds the focus; a key the
 * game has no binding for (the volume rocker, the back button) is answered with a plain false
 * there and so stays the system's, as it does in any other app. An override here that only
 * forwarded to super would read like a policy when the point is that there is none.
 */
public class ScacelithActivity extends Activity {
    private FrameLayout root;
    private GameView view;
    private LinearLayout overlay;
    private boolean overlayVisible = true;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // A first-person view of a hall: landscape, and nothing of the system on top of it.
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        enterImmersive();

        root = new FrameLayout(this);
        view = new GameView(this);
        root.addView(view, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        overlay = buildOverlay();
        root.addView(overlay, overlayParams());

        setContentView(root);

        Native.nativeInit(this, getFilesDir().getAbsolutePath());
        Native.nativeStart();
    }

    // -----------------------------------------------------------------------------------------
    // The window
    // -----------------------------------------------------------------------------------------
    private void enterImmersive() {
        View decor = getWindow().getDecorView();
        decor.setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_FULLSCREEN);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) enterImmersive();   // the bars come back after a swipe: put them away again
    }

    // -----------------------------------------------------------------------------------------
    // The buttons: the keys of the desktop game that have no touch gesture
    // -----------------------------------------------------------------------------------------
    private LinearLayout buildOverlay() {
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.BOTTOM | Gravity.END);
        // The table is in OverlayButtons: what each button sends is read (and tested) in one place.
        for (OverlayButtons.Entry entry : OverlayButtons.ALL) addKeyButton(bar, entry);
        return bar;
    }

    private void addKeyButton(LinearLayout bar, final OverlayButtons.Entry entry) {
        final int key = entry.key;
        Button b = new Button(this);
        b.setText(entry.label);
        b.setTextSize(16f);
        b.setContentDescription(entry.description);
        b.setAlpha(0.35f);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(dp(44), dp(40));
        lp.setMargins(dp(3), dp(3), dp(3), dp(3));
        b.setLayoutParams(lp);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) b.setBackground(null);
        b.setOnClickListener(v -> {
            if (key == OverlayButtons.KEYBOARD) showKeyboard();
            else {
                Native.nativeKey(key, true);
                Native.nativeKey(key, false);
            }
        });
        bar.addView(b);
    }

    private FrameLayout.LayoutParams overlayParams() {
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.gravity = Gravity.BOTTOM | Gravity.END;
        lp.setMargins(0, 0, dp(8), dp(4));
        return lp;
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private void showKeyboard() {
        view.requestFocus();
        InputMethodManager imm = (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
        if (imm != null) imm.showSoftInput(view, InputMethodManager.SHOW_IMPLICIT);
    }

    // -----------------------------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------------------------
    @Override
    protected void onPause() {
        super.onPause();
        Native.nativePause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        enterImmersive();
        Native.nativeResume();
    }

    @Override
    protected void onDestroy() {
        Native.nativeShutdown();   // the game saves its settings and returns from its loop
        super.onDestroy();
    }

    // -----------------------------------------------------------------------------------------
    // The services the native side asks of Java (platform_android.h)
    // -----------------------------------------------------------------------------------------
    /** A short message over the game (startup failures, a server that cannot be reached). */
    @SuppressWarnings("unused")
    public void showToast(String text) {
        runOnUiThread(() -> Toast.makeText(this, text, Toast.LENGTH_LONG).show());
    }

    /** The system clipboard, as UTF-8 text ("": empty or unavailable). */
    @SuppressWarnings("unused")
    public String clipboardText() {
        ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        if (cm == null || !cm.hasPrimaryClip()) return "";
        ClipData clip = cm.getPrimaryClip();
        if (clip == null || clip.getItemCount() == 0) return "";
        CharSequence t = clip.getItemAt(0).coerceToText(this);
        return t == null ? "" : t.toString();
    }

    /** The interface language as a locale tag ("fr-FR", "zh-TW"), "" when unknown. */
    @SuppressWarnings("unused")
    public String localeTag() {
        Locale l = Locale.getDefault();
        if (l == null) return "";
        String tag = Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP ? l.toLanguageTag() : l.toString();
        return tag == null ? "" : tag.replace('_', '-');
    }

    /**
     * The game's own loop has ended (Quit in the menu): close the app. The native side cannot
     * start a second game loop in this process, so the process goes too, once the activity has
     * left the screen.
     */
    @SuppressWarnings("unused")
    public void quitApp() {
        runOnUiThread(() -> {
            finishAndRemoveTask();
            new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(() -> System.exit(0), 300);
        });
    }

    /** Opens an http(s) URL in the browser (the online pages' links, the licence pages). */
    @SuppressWarnings("unused")
    public void openUrl(String url) {
        try {
            Intent i = new Intent(Intent.ACTION_VIEW, Uri.parse(url));
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            startActivity(i);
        } catch (Exception e) {
            showToast("No browser: " + url);
        }
    }
}
