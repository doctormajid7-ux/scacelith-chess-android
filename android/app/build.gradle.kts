plugins {
    id("com.android.application")
}

android {
    namespace = "com.scacelith.game"
    compileSdk = 35
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = "com.scacelith.game"
        // 26 (Android 8): AAudio is API 26, and OpenGL ES 3.2 is what the renderer needs.
        minSdk = 26
        targetSdk = 35
        versionCode = 10001
        versionName = "1.0.0-beta.2-android.1"
        // arm64 only: the game needs a 64-bit GL ES 3.2 driver, and the embedded engine is built
        // per architecture. 32-bit devices (armeabi-v7a) would need their own Stockfish build and
        // could not hold the renderer's buffers anyway.
        ndk {
            abiFilters += "arm64-v8a"
        }
        externalNativeBuild {
            cmake {
                // The repository root: the native build compiles the sources that live there, and
                // embeds shaders/ and assets/ from it.
                arguments += "-DSCACELITH_ROOT=${projectDir.parentFile.parentFile}"
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        debug {
            isJniDebuggable = true
            externalNativeBuild {
                cmake {
                    arguments += "-DCMAKE_BUILD_TYPE=Debug"
                }
            }
        }
        release {
            isMinifyEnabled = false
            // Diagnostics from adb on the optimized build (run-as for the log file, debuggerd for
            // the stacks). Remove for a store release, together with the manifest's profileable.
            isDebuggable = true
            // Signed with the debug key so the built APK installs out of the box (a store release
            // would swap in a real keystore; an unsigned APK cannot install at all).
            signingConfig = signingConfigs.getByName("debug")
            externalNativeBuild {
                cmake {
                    // -O2 with the warnings on, and the native libraries are stripped by AGP.
                    arguments += "-DCMAKE_BUILD_TYPE=Release"
                }
            }
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildFeatures {
        buildConfig = false
    }
    // (No assets folder: the shaders and the game's assets are compiled into the library by
    // cmake/embed.cmake, so the APK holds the native libraries, the Java classes and the launcher
    // icon, nothing else.)

    testOptions {
        // Robolectric runs the Views against the real framework classes in a JVM, which means it
        // needs the merged manifest and the resources of the app (the theme the manifest names, the
        // launcher icon, the app's name).
        unitTests.isIncludeAndroidResources = true
    }
}

// The trace TouchTraceTest writes and tests/touch_bridge_tests.cpp replays: the only way the Java
// half and the C++ half of a touch can be checked against each other, since neither toolchain can
// run the other. It lands in the repository's build/ (not committed), which is where the desktop
// tests look for it (tests/repo_files.h), and the desktop test skips when the file is not there --
// so a desktop-only build still runs green, with one more skip in the summary.
tasks.withType<Test>().configureEach {
    systemProperty("scacelith.touch.trace", "${projectDir.parentFile.parentFile}/build/android-touch-trace.txt")
}

// The JVM half of the tests (app/src/test/java). Two layers:
//
//  * the translation from Android's input to the game's (KeyMap, OverlayButtons, InputMethodBridge)
//    is plain Java on purpose -- no Android class but the key codes, which are compile-time
//    constants --, so it needs nothing but JUnit.
//  * the Views (the overlay's buttons, GameView's touch and surface callbacks) are views, and are
//    run by Robolectric: a JVM-only Android, no device and no emulator. libscacelith.so is not
//    there, so the native bridge is shadowed (ShadowNative) and what the Views send it is recorded.
dependencies {
    testImplementation("junit:junit:4.13.2")
    // 4.14.1 is the release that runs API 35 (the platform this app compiles and targets against)
    // and it needs Java 17, which is what the project already builds with. It fetches
    // org.robolectric:android-all-instrumented:15-robolectric-12650502-i7, kept in ~/.m2/repository
    // by a first run, so a later build resolves it without the network.
    testImplementation("org.robolectric:robolectric:4.14.1")
}
