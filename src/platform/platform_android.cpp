// Android platform layer: one ANativeWindow with an EGL / OpenGL ES 3.2 context, touch input, the
// activity lifecycle, timing and paths. The Android counterpart of platform_win32.cpp (the
// shipping target) and platform_x11.cpp (the Linux development one).
//
// Shape of an Android app: Java owns the window (a SurfaceView), the input events and the
// lifecycle; this file owns the GL context and the game. The two sides meet on three small
// bridges:
//   * the window: Java hands the Surface over (nativeSurfaceCreated / Changed / Destroyed). The
//     game thread adopts it, because only the thread the context is current on may touch EGL.
//   * the events: Java pushes touch, key and text events into a queue this file drains in
//     pumpEvents(); the gesture logic below turns them into the mouse and keyboard state the game
//     was written against (see "Touch" further down).
//   * the services: a Toast, an ACTION_VIEW intent, the clipboard and the user's language, called
//     from here and from src/net/net_sys.cpp (android_plat, platform_android.h).
//
// The game itself runs on a thread of its own (started by nativeStart), so the Java thread is
// never blocked by the renderer: the UI stays responsive while a frame is drawn.
#if defined(__ANDROID__)

#include "platform.h"
#include "platform_android.h"
#include "touch_input.h"
#include "../gl/gl46.h"
#include "../gl/gl46_gles.h"
#include "../gl/gl_context.h"
#include "../core/log.h"
#include "../net/net_sys.h"

#include <jni.h>
#include <android/native_window_jni.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#include <sys/system_properties.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// ---------------------------------------------------------------------------------------------
// Java handles (JNIEnv of the game thread, the Activity, the methods called across the bridge).
// ---------------------------------------------------------------------------------------------
JavaVM* g_vm = nullptr;
jobject g_activity = nullptr;      // global ref, kept for the life of the process
jmethodID g_mToast = nullptr, g_mClipboard = nullptr, g_mLanguage = nullptr, g_mOpenUrl = nullptr, g_mQuit = nullptr;
std::string g_filesDir;
JNIEnv* g_env = nullptr;           // the game thread's environment (attached at start)

// ---------------------------------------------------------------------------------------------
// Window, EGL and lifecycle state. g_mutex guards everything the Java thread writes and the game
// thread reads (the pending window, the event queue, the pause and quit flags).
// ---------------------------------------------------------------------------------------------
std::mutex g_mutex;
std::condition_variable g_cv;
ANativeWindow* g_window = nullptr;          // adopted by the game thread
ANativeWindow* g_pendingWindow = nullptr;   // handed over by Java, not adopted yet
bool g_pendingDestroy = false;              // the current window went away
bool g_quit = false;
bool g_started = false;
double g_pausedSeconds = 0.0;
std::chrono::steady_clock::time_point g_pauseStarted;
bool g_waitingForWindow = false;

EGLDisplay g_display = EGL_NO_DISPLAY;
EGLConfig g_config = nullptr;
EGLContext g_context = EGL_NO_CONTEXT;
EGLSurface g_surface = EGL_NO_SURFACE;
int g_width = 0, g_height = 0;
ANativeWindow* g_surfaceWindow = nullptr;   // the window 'g_surface' was created for
std::chrono::steady_clock::time_point g_t0;

// The touch input pipeline (the Java-thread event queue and the gesture state machine) lives in
// src/platform/touch_input.cpp, which is platform-neutral and unit-tested on the desktop
// (tests/touch_input_tests.cpp). The Java thread pushes into it (the JNI entry points below) and
// the game thread drains it (pumpEvents). See that file for the touch-to-mouse mapping.

// A monotonic millisecond clock, for the touch layer's tap timing.
double gestureNowMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------------------------
// Input state handed to the game (plat::input()).
// ---------------------------------------------------------------------------------------------
plat::Input g_input;
bool g_focus = true;
// The gesture state machine and its event queue (src/platform/touch_input.cpp).
plat::TouchInput g_touch;

// ---------------------------------------------------------------------------------------------
// EGL.
// ---------------------------------------------------------------------------------------------
void* getProc(const char* name) {
    void* p = (void*)eglGetProcAddress(name);
    if (!p) p = dlsym(RTLD_DEFAULT, name);
    return p;
}

bool createEglSurface(ANativeWindow* window) {
    EGLint attribs[] = {EGL_NONE};
    g_surface = eglCreateWindowSurface(g_display, g_config, window, attribs);
    if (g_surface == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface failed (0x%x)", eglGetError());
        return false;
    }
    if (!eglMakeCurrent(g_display, g_surface, g_surface, g_context)) {
        LOGE("eglMakeCurrent failed (0x%x)", eglGetError());
        eglDestroySurface(g_display, g_surface);
        g_surface = EGL_NO_SURFACE;
        return false;
    }
    g_surfaceWindow = window;
    // Without a request Android keeps a game window at 60 Hz, whatever the panel can do (90, 120,
    // 144 Hz on recent phones): ask for 120 Hz. ANativeWindow_setFrameRate is API 30, looked up so
    // the app still starts on older systems (minSdk 26), where the request just does not exist.
    {
        using SetFrameRate = int32_t (*)(ANativeWindow*, float, int8_t);
        static const auto setFrameRate = [] {
            void* lib = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
            return lib ? (SetFrameRate)dlsym(lib, "ANativeWindow_setFrameRate") : SetFrameRate(nullptr);
        }();
        if (!setFrameRate) LOGI("display: no ANativeWindow_setFrameRate (Android < 11), default rate");
        if (setFrameRate) {
            const int32_t rc = setFrameRate(window, 120.0f, 0 /* ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT */);
            LOGI("display: 120 Hz requested (%s)", rc == 0 ? "ok" : "refused");
        }
    }
    g_width = ANativeWindow_getWidth(window);
    g_height = ANativeWindow_getHeight(window);
    g_touch.setViewport(g_width, g_height);
    LOGI("EGL surface %dx%d", g_width, g_height);
    return true;
}

void destroyEglSurface() {
    if (g_surface == EGL_NO_SURFACE) return;
    eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(g_display, g_surface);
    g_surface = EGL_NO_SURFACE;
    g_surfaceWindow = nullptr;
}

// Adopts the window Java handed over, or waits for one when the app is in the background. Called
// from the game thread only.
void serviceWindow() {
    ANativeWindow* fresh = nullptr;
    bool drop = false;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        fresh = g_pendingWindow;
        g_pendingWindow = nullptr;
        drop = g_pendingDestroy;
        g_pendingDestroy = false;
    }
    if (drop) {
        destroyEglSurface();
        if (g_window) {
            ANativeWindow_release(g_window);
            g_window = nullptr;
        }
    }
    if (fresh && fresh != g_window) {
        if (g_window) ANativeWindow_release(g_window);
        g_window = fresh;
    }
    if (g_window && (!g_surfaceWindow || g_surfaceWindow != g_window)) {
        destroyEglSurface();
        createEglSurface(g_window);
    }
    if (!g_window && !g_waitingForWindow) {
        // Nothing to draw on (the activity is in the background): park the game loop here, and
        // count the time away so the chess clock, the animations and the AI do not pay for it.
        g_waitingForWindow = true;
        g_pauseStarted = std::chrono::steady_clock::now();
        g_focus = false;
        LOGI("no window: the game loop waits");
    }
    if (g_waitingForWindow && g_window) {
        const double away = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_pauseStarted).count();
        g_pausedSeconds += away;
        g_waitingForWindow = false;
        g_focus = true;
        LOGI("window back: the game loop resumes (%.1f s away)", away);
    }
}

// ---------------------------------------------------------------------------------------------
// The game thread: main.cpp's loop, exactly as on the desktop.
// ---------------------------------------------------------------------------------------------
extern "C" int scacelith_run_app(int argc, const char** argv);

void gameThread() {
    if (g_vm) g_vm->AttachCurrentThread(&g_env, nullptr);
    {
        // Nothing can be drawn before Java hands the Surface over; plat::init waits too, and
        // gives up with a message the player can act on when no window ever arrives.
        std::unique_lock<std::mutex> lk(g_mutex);
        g_cv.wait_for(lk, std::chrono::milliseconds(200));
    }
    const char* args[] = {};
    int rc = scacelith_run_app(0, args);
    LOGI("the game loop ended (rc=%d)", rc);
    bool javaAsked = false;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        javaAsked = g_quit;   // the activity is being destroyed: it is leaving already
        g_quit = true;
    }
    // Quit from the game's menu: the activity knows nothing of it and would stay on screen,
    // frozen on the last frame, until the system kills it. Tell it to close.
    if (!javaAsked && g_env && g_activity && g_mQuit) {
        g_env->CallVoidMethod(g_activity, g_mQuit);
        if (g_env->ExceptionCheck()) g_env->ExceptionClear();
    }
    if (g_vm) g_vm->DetachCurrentThread();
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// plat:: -- the interface the game uses (platform.h).
// ---------------------------------------------------------------------------------------------
namespace plat {

bool init(const WindowDesc& desc) {
    g_t0 = std::chrono::steady_clock::now();
    // An app has no command line and no environment of the player's: the diagnostics the desktop
    // reads from variables (SCACELITH_GPU_PROFILE, ...) come from a system property instead,
    // `adb shell setprop debug.scacelith.env "NAME=value NAME2=value2"`, read at each start.
    {
        char value[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.scacelith.env", value) > 0) {
            std::string all(value);
            size_t p = 0;
            while (p < all.size()) {
                size_t q = all.find(' ', p);
                if (q == std::string::npos) q = all.size();
                std::string kv = all.substr(p, q - p);
                size_t eq = kv.find('=');
                if (eq != std::string::npos && eq > 0) {
                    setenv(kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str(), 1);
                    LOGI("env from debug.scacelith.env: %s", kv.c_str());
                }
                p = q + 1;
            }
        }
    }
    {
        std::unique_lock<std::mutex> lk(g_mutex);
        // The SurfaceView has not necessarily handed its surface over yet (nativeStart runs from
        // onCreate; the surface arrives a frame later). Wait for it, but not forever: a failure
        // here is a message box away from the player.
        if (!g_window && !g_pendingWindow) {
            LOGI("waiting for the window");
            g_cv.wait_for(lk, std::chrono::seconds(10),
                          [] { return g_window != nullptr || g_pendingWindow != nullptr || g_quit; });
        }
        if (!g_window && g_pendingWindow) {
            g_window = g_pendingWindow;
            g_pendingWindow = nullptr;
        }
        if (!g_window) {
            LOGE("no window: the game cannot start");
            return false;
        }
    }
    g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_display == EGL_NO_DISPLAY || !eglInitialize(g_display, nullptr, nullptr)) {
        LOGE("cannot initialise EGL (0x%x)", eglGetError());
        return false;
    }
    const EGLint configAttribs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                                    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
                                    EGL_NONE};
    EGLint count = 0;
    if (!eglChooseConfig(g_display, configAttribs, &g_config, 1, &count) || count < 1) {
        LOGE("no EGL config for an ES 3 window (0x%x)", eglGetError());
        return false;
    }
    // Ask for ES 3.2 exactly: the emulation layer (gl46_gles.cpp) needs features GL ES 3.2 added
    // (glCopyImageSubData, samplers as core, KHR_debug) and the renderer needs SSBOs, compute and
    // image load/store from 3.1. A driver that cannot give 3.2 gets the 3.0 context as a last
    // resort, and the log says what is missing.
    const EGLint ctx32[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE};
    g_context = eglCreateContext(g_display, g_config, EGL_NO_CONTEXT, ctx32);
    if (g_context == EGL_NO_CONTEXT) {
        LOGW("no OpenGL ES 3.2 context, falling back to 3.0 (some effects will be missing)");
        const EGLint ctx30[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        g_context = eglCreateContext(g_display, g_config, EGL_NO_CONTEXT, ctx30);
    }
    if (g_context == EGL_NO_CONTEXT) {
        LOGE("cannot create an OpenGL ES 3 context (0x%x)", eglGetError());
        return false;
    }
    if (!createEglSurface(g_window)) return false;
    int missing = gl46::load(getProc, nullptr);
    if (missing) LOGW("%d GL entry points missing (expected on ES: the emulation fills them)", missing);
    gl46::installGlesFallbacks();
    gl46::afterContextCreated(desc.debugContext);
    setVsync(desc.vsync);
    g_focus = true;
    return true;
}

void shutdown() {
    destroyEglSurface();
    if (g_display != EGL_NO_DISPLAY) {
        if (g_context != EGL_NO_CONTEXT) eglDestroyContext(g_display, g_context);
        eglTerminate(g_display);
    }
    g_display = EGL_NO_DISPLAY;
    g_context = EGL_NO_CONTEXT;
}

bool pumpEvents() {
    g_touch.update(g_input, gestureNowMs());
    serviceWindow();
    if (g_waitingForWindow && !g_quit) {
        // Parked with no surface to draw on: wait for the next one (or for shutdown), then let the
        // frame that follows work out its (small) delta time.
        std::unique_lock<std::mutex> lk(g_mutex);
        g_cv.wait_for(lk, std::chrono::milliseconds(200), [] { return g_window != nullptr || g_quit; });
        serviceWindow();
        if (g_window) return true;
    }
    return !g_quit;
}

void swapBuffers() {
    if (g_surface == EGL_NO_SURFACE) return;
    eglSwapBuffers(g_display, g_surface);
    // The window's size follows the device (rotation, a fold, a resized freeform window) without
    // Java having to tell us: the renderer resizes when it changes (main.cpp).
    if (g_window) {
        g_width = ANativeWindow_getWidth(g_window);
        g_height = ANativeWindow_getHeight(g_window);
        g_touch.setViewport(g_width, g_height);
    }
}

void setVsync(bool on) {
    if (g_display != EGL_NO_DISPLAY) eglSwapInterval(g_display, on ? 1 : 0);
}
void setDisplayMode(DisplayMode, int, int) {
    // The window belongs to the system: fullscreen is the only mode there is, and its size is the
    // surface's. Options > Screen keeps its settings, they just have nothing to apply to.
}
int width() { return g_width; }
int height() { return g_height; }
bool hasFocus() { return g_focus && !g_waitingForWindow; }

double time() {
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count();
    return t - g_pausedSeconds;
}
void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

const Input& input() { return g_input; }
void setCursorVisible(bool) {}   // the game draws its own pointer over the table
void setMouseCaptured(bool) {}   // no cursor to capture: the look is the two-finger drag

std::string exeDirectory() { return net::sys::exeDirectory(); }
std::string userDataDirectory() { return net::sys::userDataDirectory(); }
std::string appDataDirectory() { return net::sys::appDataDirectory(); }
void messageBox(const char* title, const char* text, bool) {
    LOGE("%s: %s", title, text);
    android_plat::toast(title, text);
}
uint64_t randomSeed() {
    uint64_t v = 0;
    if (FILE* f = std::fopen("/dev/urandom", "rb")) {
        if (std::fread(&v, sizeof(v), 1, f) != 1) v = 0;
        std::fclose(f);
    }
    if (!v) v = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
    return v;
}
std::string clipboardText() { return android_plat::clipboardText(); }
std::string systemLanguage() { return android_plat::systemLanguage(); }
std::vector<std::string> commandLine() { return {}; }   // no command line on Android
bool openInFileManager(const std::string&) { return false; }   // the saved-games page hides the button

}  // namespace plat

// ---------------------------------------------------------------------------------------------
// android_plat:: -- what the core library asks of the app (platform_android.h).
// ---------------------------------------------------------------------------------------------
namespace android_plat {

void setFilesDir(const std::string& dir) {
    g_filesDir = dir;
    if (!g_filesDir.empty() && g_filesDir.back() != '/') g_filesDir += '/';
    setenv("SCACELITH_FILES_DIR", g_filesDir.c_str(), 1);   // net::sys reads the folder from here
}
std::string filesDir() { return g_filesDir; }

namespace {
// Calls a String-returning method of the activity; "" when there is none, null or it failed.
std::string callString(jmethodID id) {
    if (!g_env || !g_activity || !id) return std::string();
    jstring r = (jstring)g_env->CallObjectMethod(g_activity, id);
    if (!r) return std::string();
    const char* c = g_env->GetStringUTFChars(r, nullptr);
    std::string out = c ? c : "";
    if (c) g_env->ReleaseStringUTFChars(r, c);
    g_env->DeleteLocalRef(r);
    return out;
}
void callVoidString(jmethodID id, const std::string& arg) {
    if (!g_env || !g_activity || !id) return;
    jstring s = g_env->NewStringUTF(arg.c_str());
    g_env->CallVoidMethod(g_activity, id, s);
    g_env->DeleteLocalRef(s);
}
}  // namespace

bool openUrl(const std::string& url) {
    if (!g_env || !g_activity || !g_mOpenUrl) return false;
    callVoidString(g_mOpenUrl, url);
    return true;
}
void toast(const char* title, const char* text) {
    if (!g_env || !g_activity || !g_mToast) return;
    callVoidString(g_mToast, std::string(title ? title : "") + ": " + std::string(text ? text : ""));
}
void setTouchDirect(bool direct) {
    const plat::TouchInput::Mapping m = direct ? plat::TouchInput::Touch : plat::TouchInput::Touchpad;
    if (g_touch.mapping() != m) {
        g_touch.setMapping(m);
        LOGI("touch: %s mapping", direct ? "direct" : "touchpad");
    }
}
std::string clipboardText() { return callString(g_mClipboard); }
std::string systemLanguage() { return callString(g_mLanguage); }

namespace {
jclass g_httpClass = nullptr;
// Detaches a thread this file attached, when the thread ends (a JVM refuses to let an attached
// native thread exit).
struct ThreadAttachment {
    JNIEnv* env = nullptr;
    bool attachedHere = false;
    ~ThreadAttachment() {
        if (attachedHere && g_vm) g_vm->DetachCurrentThread();
    }
};
}  // namespace

::_JNIEnv* threadEnv() {
    if (!g_vm) return nullptr;
    thread_local ThreadAttachment t;
    if (t.env) return t.env;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&t.env), JNI_VERSION_1_6) == JNI_OK) return t.env;
    if (g_vm->AttachCurrentThread(&t.env, nullptr) != JNI_OK) {
        t.env = nullptr;
        return nullptr;
    }
    t.attachedHere = true;
    return t.env;
}
void* httpClass() { return g_httpClass; }
void cacheHttpClass(JNIEnv* env, jclass cls) { g_httpClass = static_cast<jclass>(env->NewGlobalRef(cls)); }

}  // namespace android_plat

// ---------------------------------------------------------------------------------------------
// JNI: com.scacelith.game.Native. Every entry point except nativeStart is called from the Java UI
// thread and only touches the event queue or the state under g_mutex; the window is adopted, and
// EGL created, on the game thread.
// ---------------------------------------------------------------------------------------------
extern "C" {

JNIEXPORT void JNICALL Java_com_scacelith_game_Native_nativeInit(JNIEnv* env, jobject, jobject activity, jstring filesDir) {
    env->GetJavaVM(&g_vm);
    g_activity = env->NewGlobalRef(activity);
    if (filesDir) {
        const char* c = env->GetStringUTFChars(filesDir, nullptr);
        android_plat::setFilesDir(c ? c : "");
        env->ReleaseStringUTFChars(filesDir, c);
    }
    g_env = env;
    if (g_activity) {
        jclass cls = env->GetObjectClass(g_activity);
        g_mToast = env->GetMethodID(cls, "showToast", "(Ljava/lang/String;)V");
        g_mClipboard = env->GetMethodID(cls, "clipboardText", "()Ljava/lang/String;");
        g_mLanguage = env->GetMethodID(cls, "localeTag", "()Ljava/lang/String;");
        g_mOpenUrl = env->GetMethodID(cls, "openUrl", "(Ljava/lang/String;)V");
        g_mQuit = env->GetMethodID(cls, "quitApp", "()V");
        env->DeleteLocalRef(cls);
    }
    // The transport's class, while this thread still has the app's class loader.
    if (jclass http = env->FindClass("com/scacelith/game/Http")) {
        android_plat::cacheHttpClass(env, http);
        env->DeleteLocalRef(http);
    } else {
        env->ExceptionClear();
        LOGW("com.scacelith.game.Http not found: no HTTPS transport");
    }
    LOGI("Android platform layer ready (files: %s)", g_filesDir.c_str());
}

JNIEXPORT void JNICALL Java_com_scacelith_game_Native_nativeStart(JNIEnv*, jobject) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_started) return;
    g_started = true;
    std::thread(gameThread).detach();
}

JNIEXPORT void JNICALL Java_com_scacelith_game_Native_nativeSurfaceCreated(JNIEnv* env, jobject, jobject surface) {
    ANativeWindow* window = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (g_pendingWindow) ANativeWindow_release(g_pendingWindow);
        g_pendingWindow = window;   // the game thread adopts it (EGL is its to touch)
        g_pendingDestroy = false;
    }
    g_cv.notify_all();
}

JNIEXPORT void JNICALL Java_com_scacelith_game_Native_nativeSurfaceChanged(JNIEnv*, jobject, jint, jint) {
    // The window follows its Surface by itself: swapBuffers reads its size every frame.
    g_cv.notify_all();
}

JNIEXPORT void JNICALL Java_com_scacelith_game_Native_nativeSurfaceDestroyed(JNIEnv*, jobject) {
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_pendingDestroy = true;
    }
    g_cv.notify_all();
}

JNIEXPORT void JNICALL Java_com_scacelith_game_Native_nativePause(JNIEnv*, jobject) {
    LOGI("activity paused");
}

JNIEXPORT void JNICALL Java_com_scacelith_game_Native_nativeResume(JNIEnv*, jobject) {
    g_cv.notify_all();
}

JNIEXPORT void JNICALL Java_com_scacelith_game_Native_nativeShutdown(JNIEnv*, jobject) {
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_quit = true;
    }
    g_cv.notify_all();
}

JNIEXPORT void JNICALL Java_com_scacelith_game_Native_nativeTouch(JNIEnv*, jobject, jint action, jint pointerId, jfloat x,
                                                                 jfloat y, jint pointerCount) {
    plat::TouchEvent e;
    e.kind = plat::TouchEvent::Touch;
    e.action = action;
    e.pointerId = pointerId;
    e.pointerCount = pointerCount;
    e.x = x;
    e.y = y;
    g_touch.push(e);
}

JNIEXPORT void JNICALL Java_com_scacelith_game_Native_nativeWheel(JNIEnv*, jobject, jfloat notches) {
    plat::TouchEvent e;
    e.kind = plat::TouchEvent::Wheel;
    e.wheel = notches;
    g_touch.push(e);
}

JNIEXPORT void JNICALL Java_com_scacelith_game_Native_nativeKey(JNIEnv*, jobject, jint key, jboolean down) {
    plat::TouchEvent e;
    e.kind = plat::TouchEvent::Key;
    e.key = key;
    e.down = down == JNI_TRUE;
    g_touch.push(e);
}

JNIEXPORT void JNICALL Java_com_scacelith_game_Native_nativeText(JNIEnv* env, jobject, jstring text) {
    if (!text) return;
    const char* c = env->GetStringUTFChars(text, nullptr);
    plat::TouchEvent e;
    e.kind = plat::TouchEvent::Text;
    e.text = c ? c : "";
    if (c) env->ReleaseStringUTFChars(text, c);
    g_touch.push(e);
}

JNIEXPORT void JNICALL Java_com_scacelith_game_Native_nativeToast(JNIEnv*, jobject, jstring text) {
    if (!text) return;
    const char* c = g_env ? g_env->GetStringUTFChars(text, nullptr) : nullptr;
    LOGI("%s", c ? c : "");
    if (c && g_env) g_env->ReleaseStringUTFChars(text, c);
}

}  // extern "C"

#endif  // __ANDROID__
