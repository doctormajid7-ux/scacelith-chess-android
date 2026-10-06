// The handful of services the core library (src/net, src/ui) needs from the Android app: the
// private data folder the Java side hands over at start-up, the system browser, the clipboard and
// the interface language. Implemented by the platform layer (platform_android.cpp) over JNI; the
// desktop builds never compile either side of it.
#pragma once
#include <string>

struct _JNIEnv;   // jni.h's JNIEnv (C++)

namespace android_plat {

// The app's private files directory (/data/user/0/<package>/files/), with a trailing separator,
// echoed to the SCACELITH_FILES_DIR environment variable for net::sys (which does not know Java).
// Called once, before the settings, the log and the saved games are opened; "" before.
void setFilesDir(const std::string& dir);
std::string filesDir();

// Opens an http(s) URL in the default browser (Intent.ACTION_VIEW). False when it could not be
// handed over (no activity, no browser).
bool openUrl(const std::string& url);
// A short message shown over the game (a Toast): the startup failures of main.cpp.
void toast(const char* title, const char* text);
// Text from the system clipboard ("" when there is none or it is empty).
std::string clipboardText();
// The user's interface language as a locale tag ("fr-FR", "zh-TW"), "" when unknown.
std::string systemLanguage();
// The touch mapping (Options > Game): direct, the finger is the pointer (a tap clicks where it
// lands, a drag carries); or the touchpad, the finger moves an arrow (the default). Game thread.
void setTouchDirect(bool direct);

// JNI for the transport (src/net/transport_android.cpp), which runs on worker threads: the
// calling thread's environment (attached on first use, detached when the thread ends), and the
// app's com.scacelith.game.Http class (a global reference cached at start-up: a thread created in
// native code only sees the system class loader, which cannot find the app's classes).
// nullptr before nativeInit.
::_JNIEnv* threadEnv();
void* httpClass();   // jclass

}  // namespace android_plat
