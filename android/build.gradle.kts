// The Android build of Scacelith. Deliberately dependency-free apart from the Android Gradle
// plugin: the app is a plain Activity plus a SurfaceView (no AndroidX, no Kotlin), so a build only
// needs the SDK, the NDK and the plugin itself.
plugins {
    id("com.android.application") version "8.7.3" apply false
}
