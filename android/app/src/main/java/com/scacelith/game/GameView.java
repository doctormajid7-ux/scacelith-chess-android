package com.scacelith.game;

import android.annotation.SuppressLint;
import android.content.Context;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;

/**
 * The window of the game: a SurfaceView whose surface is handed to the native side, and whose
 * input events are translated into the keyboard and pointer events the game was written against
 * (src/platform/platform_android.h, platform_android.cpp).
 *
 * Two kinds of touch, both handled by the native gesture layer (it owns the state machine):
 *   * one finger: press / drag / release the left button (touch a piece, carry it, play it);
 *   * two or three: look around, lean in (pinch), press the clock (a two-finger tap) or open the
 *     menu (three fingers).
 * The keys that have no gesture (Tab, C, S, Enter, Backspace) and the software keyboard come from
 * the buttons of ScacelithActivity.
 */
@SuppressLint("ViewConstructor")
class GameView extends SurfaceView implements SurfaceHolder.Callback {
    GameView(Context context) {
        super(context);
        getHolder().addCallback(this);
        setFocusable(true);
        setFocusableInTouchMode(true);
        requestFocus();
    }

    // ---- window ----
    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        Native.nativeSurfaceCreated(holder.getSurface());
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        Native.nativeSurfaceChanged(width, height);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        Native.nativeSurfaceDestroyed();
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        Native.nativeSurfaceChanged(w, h);
    }

    // ---- touch ----
    @Override
    public boolean onTouchEvent(MotionEvent event) {
        final int action = event.getActionMasked();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
                // The pointer that went down is the last one of the event.
                send(action, event, event.getActionIndex());
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
                send(action, event, event.getActionIndex());
                break;
            case MotionEvent.ACTION_MOVE:
                // Every pointer moved; the native layer tracks them by id.
                for (int i = 0; i < event.getPointerCount(); i++) send(action, event, i);
                break;
            case MotionEvent.ACTION_CANCEL:
                Native.nativeTouch(Native.ACTION_CANCEL, 0, 0f, 0f, 0);
                break;
            default:
                break;
        }
        return true;
    }

    private void send(int action, MotionEvent event, int index) {
        Native.nativeTouch(action, event.getPointerId(index), event.getX(index), event.getY(index),
                           event.getPointerCount());
    }

    // ---- keys (a physical keyboard, or the software one through the input connection) ----
    // KeyMap (which key, and what a keystroke types) holds the table as plain Java, so a JVM test
    // can check it on its own, codes and all; what the two overrides below send it to is
    // GameViewKeyboardTest's, which runs this View under Robolectric (app/src/test/java).
    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        final int key = KeyMap.key(keyCode);
        // A keyboard plugged into the phone types into the game's fields the way a desktop one does:
        // the character goes to Input::text, which is what the fields insert from, and the key to
        // the game's shortcuts. A key code alone types nothing there.
        final int typed = KeyMap.typed(event.getUnicodeChar());
        if (typed != 0) Native.nativeText(new String(Character.toChars(typed)));
        if (key != 0) Native.nativeKey(key, true);
        // The volume keys and the others the system acts on stay the system's.
        return key != 0 || super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        final int key = KeyMap.key(keyCode);
        if (key == 0) return super.onKeyUp(keyCode, event);
        Native.nativeKey(key, false);
        return true;
    }

    // ---- the software keyboard ----
    @Override
    public boolean onCheckIsTextEditor() {
        return true;
    }

    @Override
    public InputConnection onCreateInputConnection(EditorInfo out) {
        out.inputType = android.text.InputType.TYPE_CLASS_TEXT;
        out.imeOptions = EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_FULLSCREEN;
        // Text fields in the game read Input::text (typed characters) and the Backspace / Delete /
        // Enter keys; an input method only commits text, so the deletes and the action key are
        // turned back into the keys the game's fields listen to. The translation itself is
        // InputMethodBridge, plain Java so that a JVM test can drive it: everything below is only
        // the adapter that hands it the native layer.
        final InputMethodBridge bridge = new InputMethodBridge(new InputMethodBridge.Sink() {
            @Override
            public void key(int platKey, boolean down) { Native.nativeKey(platKey, down); }

            @Override
            public void text(String text) { Native.nativeText(text); }
        });
        return new BaseInputConnection(this, true) {
            @Override
            public boolean commitText(CharSequence text, int newCursorPosition) {
                return bridge.commitText(text);
            }

            @Override
            public boolean setComposingText(CharSequence text, int newCursorPosition) {
                return bridge.setComposingText(text);
            }

            @Override
            public boolean deleteSurroundingText(int beforeLength, int afterLength) {
                bridge.deleteSurroundingText(beforeLength, afterLength);
                return true;
            }

            @Override
            public boolean sendKeyEvent(KeyEvent event) {
                return bridge.sendKey(event.getKeyCode(), event.getAction() == KeyEvent.ACTION_DOWN);
            }

            @Override
            public boolean performEditorAction(int actionCode) {
                bridge.performEditorAction();
                return true;
            }
        };
    }

    @Override
    public boolean onGenericMotionEvent(MotionEvent event) {
        // A real mouse (a phone in a desktop dock, a tablet with one plugged in): its buttons and
        // its position arrive as touch events (Android reports them as a mouse source), and its
        // wheel is the game's lean towards the board.
        if ((event.getSource() & InputDevice.SOURCE_MOUSE) == InputDevice.SOURCE_MOUSE &&
                event.getActionMasked() == MotionEvent.ACTION_SCROLL) {
            Native.nativeWheel(event.getAxisValue(MotionEvent.AXIS_VSCROLL));
            return true;
        }
        return super.onGenericMotionEvent(event);
    }
}
