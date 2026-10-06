# Android (arm64-v8a): the game builds, and plays with touch

**Status: runs on devices.** Tested on a Snapdragon 8 Elite Gen 5 (Adreno 840) phone: the whole
game plays with touch, Stockfish 19 on arm64, the coach's voice downloaded over HTTPS and
synthesised on the phone, 60 to 100+ FPS at full resolution with the simple renderer (README.md).
Live online games and direct matches are not available yet. The notes below were written while the
port was prepared without a device; some of their "not run yet" remarks are history now.

This is a port of the shipping Windows/Linux game, not a rewrite: the same `src/` tree, the same
renderer, the same scene, chess rules, UI and coach. The differences are all "this platform has no
X":

| The desktop has | Android gets |
|---|---|
| OpenGL 4.6 core, direct state access | OpenGL ES 3.2 + `src/gl/gl46_gles.cpp` (below) |
| GLSL 4.60, tessellation | GLSL ES 3.20, tessellation off (`SCACELITH_GLES`) |
| WASAPI / ALSA | AAudio (`src/audio/backend_aaudio.cpp`) |
| WinHTTP / OpenSSL for TLS | nothing: the transport reports "unavailable" |
| BCrypt / OpenSSL for direct match | portable SHA/HMAC/HKDF; AES-GCM and ECDH unavailable |
| x86 SSE2/AVX TTS kernels | none: the coach speaks through its subtitles |
| Stockfish 19 compiled per x86 ISA | Stockfish 19 compiled twice (armv8 + armv8-dotprod), NNUE embedded |

## Requirements

- Android SDK with platform 35 and build-tools 35.0.1.
- **NDK 27.2.12479018** (`ndkVersion` in `android/app/build.gradle.kts`), `ANDROID_PLATFORM`
  android-26.
- **JDK 17.** AGP 8.7.3 refuses a JDK 21: "Toolchain installation ... does not provide the required
  capabilities: [JAVA_COMPILER]". `JAVA_HOME=/usr/lib/jvm/java-17-openjdk`.
- Gradle 8.13. The repository has no `gradlew` script: use the cached distribution
  (`~/.gradle/wrapper/dists/gradle-8.13-bin/*/gradle-8.13/bin/gradle`) or a `gradle` 8.13 on PATH.
- ~3 GB of free space: the unstripped `libscacelith.so` is 229 MB, the APK is 128 MB (Release).
- The NNUE network, `third_party/stockfish/src/nn-1a298aa575a0.nnue` (98,511,183 bytes, sha256
  `1a298aa575a085434d29027978dc36867fe9c5bcea9376654b7a8eba1e52dfc2`). The repository tracks it, but
  a sparse-checkout clone that filters `*.nnue` leaves it out of the worktree: materialize it from
  the object store (`git cat-file blob <blob> > third_party/stockfish/src/nn-1a298aa575a0.nnue`) or
  download it from Stockfish's networks repository. Without it the native build stops with a clear
  message.

## Building

The APK:

```sh
cd android
ANDROID_HOME=$HOME/android-sdk JAVA_HOME=/usr/lib/jvm/java-17-openjdk \
    ~/.gradle/wrapper/dists/gradle-8.13-bin/*/gradle-8.13/bin/gradle --no-daemon assembleRelease
    # or assembleDebug, which signs with the debug key
```

Output: `android/app/build/outputs/apk/release/app-release-unsigned.apk` (128 MB, unsigned) and,
for the debug variant, `android/app/build/outputs/apk/debug/app-debug.apk` (137 MB, signed with the
Android debug key and therefore installable as is). Both carry the engine; the sizes are dominated
by the embedded network.

The native library alone, without Gradle (much faster while iterating on C++):

```sh
cmake -B build-arm64 -G Ninja -S android/app/src/main/cpp \
      -DCMAKE_TOOLCHAIN_FILE=$HOME/android-sdk/ndk/27.2.12479018/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 \
      -DSCACELITH_ROOT=$(pwd) -DCMAKE_BUILD_TYPE=Release
ninja -C build-arm64
```

### The embedded engine

The engine is part of the build by default (`SCACELITH_STOCKFISH_ANDROID`, ON in
`android/app/src/main/cpp/CMakeLists.txt`). Stockfish 19's upstream sources compile **twice** with
the NDK's clang, once per variant: `armv8` (upstream's `ARCH=armv8` flags: `-O3 -funroll-loops
-fno-exceptions`, `-DUSE_NEON=8 -DUSE_POPCNT`, `-DIS_64BIT`) and `armv8-dotprod` (adding
`-march=armv8.2-a+dotprod -DUSE_NEON_DOTPROD`), with the two clang adaptations the desktop makes
for GCC reversed (`-fconstexpr-steps=500000000` for clang instead of `-fconstexpr-ops-limit`, and
no `-fno-ipa-cp-clone`, which clang does not know).

The desktop's variant machinery carries over. NEON is part of the arm64 baseline, but FEAT_DOTPROD
(SDOT/UDOT) is not — a Snapdragon 6xx/7xx-class core lacks it — and the linker merges the copies
of every inlined function and standard-library template that both variants compile: without
isolation a non-dotprod CPU could be handed dotprod code and crash. So each variant is isolated
into one object (like the desktop's `cmake/isolate.cmake`, but with the NDK's LLVM tools:
`third_party/stockfish/cmake/isolate_llvm.cmake` — `ld.lld` has no `--force-group-allocation`,
`llvm-objcopy` takes the symbols to keep global from a file, and the initialiser-table bounds are
defined in the linker script because `--add-symbol` cannot attach a symbol to a grouped section),
and `cmake/isa_check_arm64.cmake` checks the result where the desktop audits with
`tools/isa_audit.py` (no `sdot`/`udot` instruction in the baseline variant, at least one in the
dotprod one). `scacelith/cpu_arm64.cpp` is the dispatcher: it mirrors the desktop's `cpu.cpp` and
chooses the variant with the same run-time check upstream's own universal build makes,
`getauxval(AT_HWCAP) & HWCAP_ASIMDDP`; the variants' static initialisers are no longer run at
library load but by the dispatcher for the chosen variant, exactly as on the desktop. The NNUE
network is embedded once with Stockfish's own `INCBIN`, resolved by clang's integrated assembler
(`-Wa,-I<stockfish>/src`: the NDK ships no binutils), exactly as GNU `as` resolves it on the
desktop.

To build the APK without the engine, configure with `-DSCACELITH_STOCKFISH_ANDROID=OFF`. After
**toggling** the option, delete `android/app/.cxx` first: CMake remembers the old value in its
cache, and the next Gradle build would silently keep it (turning it on makes the APK 98.5 MB
larger — that is how the switch is noticed).

The network inside the built library can be checked without a device, which is what "embedded"
below rests on:

```sh
# gEmbeddedNNUEData/Size are in the unstripped .so's symbol table; read the 98,511,183 bytes
# they describe and hash them (in the stripped APK, find the network by its first bytes instead).
llvm-readelf -s build-arm64/libscacelith.so | grep gEmbeddedNNUE
```

### Installing

`app-debug.apk` is signed with the Android debug key (`apksigner verify` reports
`CN=Android Debug`) and installs directly — at 137.6 MB, because the Debug variant keeps the native
symbols for a native debugger (`isJniDebuggable`) and they are most of the library. The Release APK is **not** signed
(`apksigner verify` says `DOES NOT VERIFY`, no `META-INF/MANIFEST.MF`): Android refuses it. Either
use the debug APK, or add a `signingConfigs` block to `android/app/build.gradle.kts` pointing at a
keystore of your own and rebuild Release. Then:

```sh
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.scacelith.game/.ScacelithActivity
```

## What is in the APK

- `lib/arm64-v8a/libscacelith.so`, 128.1 MB in Release: the game, the renderer, the scene, the
  rules, the UI, the coach, the embedded shaders and assets (`cmake/embed.cmake`), the Android
  platform layer — and a large majority of it, Stockfish 19 built twice for arm64 (armv8 and
  armv8-dotprod) with its 98.5 MB
  neural network embedded (`nnue_incbin.cpp`). There is no `assets/` folder in the APK: everything
  is compiled into the library. That is also why the Release APK is 128.3 MB and not 28: the
  network alone is 77% of it.
- `classes.dex` (~15 KB): four small Java files, no libraries. The whole Java side is the window,
  the input bridge and the activity lifecycle; anything else is C++.
- The launcher icon, from `res/icons/png/`.

`AndroidManifest.xml`: `sensorLandscape`, `configChanges` for everything (rotation and keyboard
events must not restart the activity: that would drop the GL context), no permissions at all, and
`android:glEsVersion="0x00030002"` so the store and the installer filter out devices that could not
run it.

## The layers

**`src/platform/platform_android.cpp`** — one `ANativeWindow`, one EGL/GLES 3.2 context, and the
game. Java owns the window (a `SurfaceView`), the input events and the lifecycle; this file owns
the context and runs the game loop on its own thread, so the Java thread never blocks on a frame.
The two sides meet on three bridges: the window (`nativeSurfaceCreated/Changed/Destroyed`, and the
game thread adopts it because only the thread the context is current on may touch EGL), the events
(Java pushes touch/key/text into a queue this file drains in `pumpEvents()`), and the services (a
Toast, an `ACTION_VIEW` intent, the clipboard, the user's language — `android_plat::` in
`platform_android.h`, also used by `src/net/net_sys.cpp` for the settings and log paths).

**`src/gl/gl46_gles.cpp`** — the compatibility layer, and the largest single piece of this port.
The renderer is written 100 % against direct state access and never calls `glBindTexture` or
`glBindBuffer` even once. GLES 3.2 does not have the DSA entry points (33 of the 104 GL functions
the engine uses are platform-absent), so this file emulates them over the classic bind-and-restore
API: it `dlopen`s `libGLESv2.so`/`libGLESv3.so` and `dlsym`s the classic entry points itself (to
avoid colliding with the gl46 globals in `src/gl/gl46.cpp`), keeps the binding slots and a
texture→target map, and wraps the calls in `Scratch` / `FboGuard` / `VaoGuard` scopes that put the
previous state back before returning. Because the game never binds anything itself, restoring the
previous binding is unobservable. `glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)` — what the
renderer asks for its reverse-Z — is ES's own convention, so it is a no-op (it logs if a different
pair is ever requested). `glDeleteTextures` is always wrapped so recycled names are forgotten.

**`src/render/shader.cpp` + `shaders/`** — the preprocessor now emits `#version 320 es` with
`#define SCACELITH_GLES 1`, the `precision` statements, and the ES sampler precisions, instead of
`#version 460 core`; nothing else in the shader corpus changed. ES has no `gl_ClipDistance`, which
the meshes use for the hall's clipping plane, so under `SCACELITH_GLES` the varyings carry a
`float clipDist` and the fragment stage discards on it. ES has no tessellation either: rather than
translating `mesh.tesc`/`mesh.tese`, the renderer's own untessellated path is used — which is
exactly what the Low and Medium quality presets already request, so it is a setting the game
already knew how to be in (`tessellationEnabled()` in `src/render/renderer.cpp`).

**`src/audio/backend_aaudio.cpp`** — AAudio, `PCM_FLOAT`, `LOW_LATENCY`, 48 kHz, stereo. AAudio
owns the audio thread and calls the mixer through its data callback, so this backend has no thread
of its own; when no stream can be opened, `createBackend()` falls back to the null backend, which
keeps the mixer running at real-time pace so the game's audio state machine never stalls (the
sounds play, they are just not heard).

**`src/net/crypto_portable.cpp`** — SHA-256 (incremental), SHA-1, HMAC-SHA256, HKDF-SHA256 and
`/dev/urandom`, written here because the NDK ships no TLS library. These are verified bit-exact
against the FIPS and RFC vectors (SHA-256 of "abc" and of the empty string, the 1e6-'a' vector and
an incremental run, SHA-1 "abc", RFC 4231 case 1, RFC 5869 A.1).

**`third_party/stockfish`** — the desktop tree, built for arm64 with one change of shape: two
variants (`armv8`, `armv8-dotprod`) instead of the x86 instruction sets, isolated by
`third_party/stockfish/cmake/isolate_llvm.cmake`, the LLVM-toolchain counterpart of the desktop's
`cmake/isolate.cmake` (the NDK has `ld.lld` and `llvm-objcopy`, no GNU binutils; lld dedups COMDAT
inside a partial link but keeps the groups, so the scheme relies on the linker never merging a
group whose signature is local — verified end to end: two compiled copies of a shared template
symbol and the game's own stay three separate copies in the shipped `.so`).
`scacelith/cpu_arm64.cpp` replaces the x86 dispatcher (`cpu.cpp`) with the same shape: a variant
table, the CPU check (`getauxval(AT_HWCAP) & HWCAP_ASIMDDP`, as upstream's `universal/entry_arm64.cpp`),
the `std::call_once` that runs the chosen variant's initialisers from its `sfinit_<tag>` table,
and `engine.arch` accepting `auto`, `armv8` or `armv8-dotprod` (an x86 name left over in a copied
`Scacelith.ini` is logged and ignored, never fatal). The upstream sources, `local_shm.h` (the
network stays in process memory: no `/tmp` directory, no socket, no `memfd`), `nnue_incbin.cpp`
and the game side (`src/ai/uci_host.cpp`) are byte-identical to the desktop's. The variants'
static initialisers no longer run at library load (they may contain dotprod code); the dispatcher
runs those of the chosen variant, exactly as `cpu.cpp` does on the desktop.

## Touch

The game is a first-person view driven by a mouse and a keyboard, so the touch layer
(`platform_android.cpp`, "Touch") is a gesture state machine that produces exactly the mouse and
key state the game was written against:

| Gesture | Means |
|---|---|
| 1 finger press / drag / release | the left button: take a piece, carry it, put it down |
| 2 fingers, drag | the right button held + move: look around (yaw and pitch) |
| 2 fingers, pinch | the wheel: lean towards the board (about 20 notches over a full pinch) |
| 2 fingers, tap | `Space` — press the chess clock |
| 3 fingers, tap | `Escape` — the menu |

A tap that begins and ends inside one frame (a quick finger) is held for one frame rather than
reported as a press and a release the game might miss. A second finger taking the piece out of the
hand ends the drag in the game's own terms.

The `Activity` adds a row of on-screen buttons for what a touchscreen has no key for, each sending
the same key the desktop uses: `≡` Escape (menu), `⏱` Space (clock), `☰` Tab, `⌖` C, `✎` S, `⌫`
Backspace, `⏎` Enter, `⌨` the soft keyboard. That table is data, in
`android/app/src/main/java/com/scacelith/game/OverlayButtons.java`, so what each button sends is
read (and tested) in one place. `BACK` maps to Escape too. The keyboard shows through
a `BaseInputConnection` (commitText / deleteSurroundingText / sendKeyEvent / performEditorAction),
which is what the UI's text fields need — a `KeyEvent`-only bridge would type nothing into them.
A mouse wheel is forwarded as well, so a tablet with a mouse works.

## What works, what does not

Works, compile-verified: the whole single-player game — the hall, the 3D board, moving pieces,
the UI, the settings, the scoresheet, saved games and their replay, the observer mode, two players
at one device (hot-seat), the coach's analysis and its subtitles, sound, the touch and keyboard
input, the activity lifecycle (pause/resume, the surface being recreated, the process being
killed), the launcher icon — **and the AI opponent**: Stockfish 19 is built for arm64 (two
variants, `armv8` and `armv8-dotprod`, isolated and dispatched on `getauxval(AT_HWCAP)`:
`android/app/src/main/cpp/CMakeLists.txt`, with upstream's flags for `ARCH=armv8[-dotprod]`,
`USE_NEON=8` and the network embedded by `nnue_incbin.cpp`), so `SCACELITH_HAS_STOCKFISH`
is on and `src/ai/uci_host.cpp` runs the engine the way it does on the desktop. Online play and the
coach's *spoken* voice are off; see below.

Does **not** work, by design of this first pass:

- **No online play, and no download of the coach's voice model.** The NDK exposes no TLS library.
  `transport_android.cpp` reports `transportAvailable() == false` and every call returns
  `"unavailable"`, which the callers already handle (the online screen says the network is
  unavailable). Going online on Android means implementing that interface over Java's own TLS
  stack across JNI, or vendoring a TLS library for the NDK: a project of its own.
- **No direct match** (two devices playing each other without a server): it needs AES-256-GCM and
  ECDH on P-256. The join codes, hashing and HKDF are implemented (`direct_crypto_android.cpp`);
  the two cipher operations return false, so a host or a guest sees "crypto unavailable" instead of
  a handshake that never completes. Java's `javax.crypto` and `KeyAgreement("ECDH")` are the
  natural way to finish it.
- **No spoken coach.** The x86 kernels cannot be compiled for arm64 — `kernels_impl.h` uses
  `__m128i` directly, not only through the per-ISA trait. `src/tts/kernels_portable.cpp` provides a
  scalar table whose functions zero their output, and `SCACELITH_TTS_NO_KERNELS` makes the
  synthesizer refuse to load a model, so the coach falls back to its subtitles — exactly what the
  desktop game does when the model has not been downloaded, which on Android it never is.
  Finishing this means a NEON kernel unit; `tests/data/tts/ref_en.bin` is the reference to compare
  against.
- **Untested at run time.** No KVM, only x86-64 system images: the emulator cannot boot on the
  machine this was built on, and no device was attached. The APK has never been installed, so
  nothing has ever drawn a frame. The ES layer's bookkeeping and the touch gesture mapping are
  covered by the desktop harnesses below, and the Views by Robolectric; EGL, AAudio, the JNI/EGL
  glue around the touch queue, the GLSL ES 3.20 re-emission and a real driver's answers are
  compile-verified only.

## Testing off a device

Five pieces of this port would fail silently on a device and cannot be run here (no /dev/kvm,
x86-64 system images only, no attached device). Two of them -- the renderer's compatibility layer
and the gesture state machine -- were kept free of every Android header so that the desktop build
can drive them instead; two more are the Java half, which Robolectric runs in a JVM; and the fifth is
the seam between the two halves, which is checked by a trace one writes and the other replays.
Between them they are what a player would notice first: the frame appears, the fingers do what they
should, and the buttons over the game work without taking the game's own touches.

### The ES 3.2 layer

A wrong line in the compatibility layer is a black screen with no explanation:

- `tests/gles_fake.cpp` builds a **fake libGLESv2** (`libgles_fake.so`): the classic ES 3.2 entry
  points the layer resolves, over a driver-like state (bindings per texture unit and per target,
  buffers, framebuffers, renderbuffers, vertex arrays, per-object storage) plus a log of every call
  made into it. It recycles object names smallest-first, as a driver's name pool does, which is
  what makes the layer's texture-target table testable.
- `tests/gles_tests.cpp` points the layer at that library (`SCACELITH_GLES_LIBRARY`, which the
  layer honours, and which is its own hook: an explicit request that cannot be honoured fails
  loudly instead of falling back to a driver that is not there) and drives it through the `gl46`
  globals, exactly as the game does, then reads the driver state back. 18 tests, among them: every
  one of the 34 entry points is really installed, and an entry point the driver resolved itself is
  kept rather than overwritten; a texture is edited through the target its creation recorded, and
  the unit-0 binding is put back untouched while other units are not touched at all; an array
  texture attaches through a layer and a 2D one through a face; deleting a texture makes the layer
  forget both its target and its binding slots, with the name recycled under it; every framebuffer,
  vertex-array and renderbuffer emulation restores both the draw and the read side;
  `glInvalidateBufferData` and `glClipControl` really do nothing;  `glBindTextureUnit` moves the
  active unit only when it has to (one `glActiveTexture` per run of binds to a unit), and every
  (unit, target) binding slot is tracked independently — the table grows per unit rather than being
  capped, so an edit can never restore another target's binding.

The whole suite runs on the desktop build, no device needed:

```sh
./build/scacelith_tests gles_      # 18 test(s), 0 failed
```

Writing it found one real defect: `glDisableVertexArrayAttrib` called `glDisableVertexAttribArray`
through the game's global instead of the driver entry point the layer had resolved itself (the same
call every other emulation goes through). On a device that works -- the global holds the driver's
function -- but it is the one place where the layer depended on `gl46::load` having succeeded, and
it is now consistent with the rest.

What the harness cannot prove: that a real driver accepts the GLSL ES 3.20 the shaders are
re-emitted as, that a real ES 3.2 implementation behaves like the fake on every call, and that a
frame actually appears. Those need a device.

### The touch input

The gesture state machine is the other place a wrong line is silent on a device: it turns touches
into the mouse and keyboard state the game was written against, and a wrong threshold is a button
that does not work. It used to live inside `platform_android.cpp`, tangled with JNI and EGL; it now
lives in `src/platform/touch_input.{h,cpp}`, which reads `plat::TouchEvent` and writes
`plat::Input` with no Android header anywhere, and `platform_android.cpp` only pushes the JNI
events into it and drains it once per frame. `tests/touch_input_tests.cpp` drives it directly --
push what Java would push, run a frame, read what the game got -- with the clock injected, so a tap
and a 350 ms press differ by a number instead of by waiting:

```sh
./build/scacelith_tests touch_      # 21 test(s), 0 failed
```

What it pins down: one finger is the left button (press, drag, release, and no key at all); a
second finger hands the piece back and starts the look, and two fingers arriving inside one frame
never grab anything on their way; the pinch is a notch per 6% of the window's long side, follows
the viewport, does nothing on the first frame, and is off once a third finger is down; a two-finger
tap presses Space and a three-finger tap Escape, both edges in one frame; a whole tap inside one
frame has its release held back for the next one (or the game never sees the click); a held or
dragged press is not a tap, and neither is a single-finger one; the keys, the UTF-8 text an IME
sends and the wheel accumulate and reset per frame; every finger gets its own slot (ten and more,
with the slots of fingers that left reused), and a release that matches no finger leaves the count
alone; and the queue stops growing at 513 events when the game thread is wedged.

Writing it found two real defects, both in behaviour a device would have hit:

- **A cancelled gesture was a tap.** `ACTION_CANCEL` lifts every finger, and the tap check that
  ran right after it saw a quick, motionless sequence and pressed Space (two fingers) or Escape
  (three). An interruption by the system -- a call arriving, a gesture navigation -- would have
  pressed the chess clock or opened the menu. The sequence is now dropped on cancel.
- **A repeated `ACTION_DOWN` for a pointer already down counted as a second finger.** A duplicate
  down for the same id inflated the pointer count, and that count is what decides between "a finger
  is dragging a piece" and "two fingers are looking around": the piece would have been handed back
  and the view started turning. The count now only moves for a pointer that was not already down.
- **The pointer table silently dropped fingers past its eighth slot, and a dropped finger's release
  still counted.** Android reports every finger a hand puts down; a finger the table had no slot for
  was ignored on the way down but its release still decremented the pointer count, which is what
  decides between a one-finger drag and a two-finger look. The table now grows (the slots of fingers
  that left are reused), so no finger is dropped, and a release that matches no finger is ignored
  rather than counted.

What the harness cannot prove: that Java delivers the MotionEvent codes the mapping expects (the
trace bridge below covers that), that the EGL and JNI glue around the queue behaves, and how the
gestures feel on a real screen. Those last two need a device.

### The Java layer

The third place a wrong line is quiet: the Java side that feeds the native layer. Most of it is
Views (the overlay's buttons, the `SurfaceView`, the input connection), and the *translation* would
have been tangled into them -- pulling it out is what lets it run here with nothing but JUnit; the
Views themselves are Robolectric's, further down. Three plain-Java classes hold it
(`android/app/src/main/java/com/scacelith/game/`), with no Android class in sight except the key
codes, which are compile-time constants and therefore inlined:

- `KeyMap` — an Android key code to a plat::Key, and what a keystroke types (`Input::text`);
- `OverlayButtons` — the overlay's eight buttons as a table;
- `InputMethodBridge` — what an input method's calls turn into, over a sink (the native layer in
  the app, a recorder in the tests).

`GameView` and `ScacelithActivity` keep only the adapters: the View's `onKeyDown` hands `KeyMap`'s
answer to `Native`, its `BaseInputConnection` hands the input method's calls to
`InputMethodBridge`, and the activity's buttons hand the table's keys to the same `Native`. The
activity overrides no key method at all, so a key the game has no binding for stays the system's --
see the class comment, and the test that pins it below.

```sh
cd android && ANDROID_HOME=$HOME/android-sdk JAVA_HOME=/usr/lib/jvm/java-17-openjdk \
    ~/.gradle/wrapper/dists/gradle-8.13-bin/*/gradle-8.13/bin/gradle --no-daemon testDebugUnitTest \
    --tests 'com.scacelith.game.KeyMapTest' --tests 'com.scacelith.game.OverlayButtonsTest' \
    --tests 'com.scacelith.game.InputMethodBridgeTest'
# 24 tests, 0 failures  (JUnit 4.13.2 and nothing else: these three classes are plain Java)
```

Writing them found three defects, all of them things a device would have shown:

- **Letters lost their case.** `keyOf` sent the character the layout types (`event.getUnicodeChar()`),
  so 'c' became plat::Key 99 while the game's shortcuts read `keyDown['C']` (67): "look at the board"
  and "scoresheet" could never fire. Every other platform layer sends the letter *key*, uppercase
  (`platform_x11.cpp`: `XK_a..XK_z` and `XK_A..XK_Z` alike to `'A' + offset`). `KeyMap.key` now does
  the same, whatever the layout.
- **A keyboard typed nothing into the game's fields.** `onKeyDown` sent only the key; the desktop
  layers send the character too (`WM_CHAR`, `XLookupString`), and `Input::text` is the *only* thing
  the game's fields insert from (`ui_widgets.cpp`). A keyboard plugged into a phone would have left
  every name field empty. The typed character is now sent alongside the key.
- **`deleteSurroundingText`'s second half was ignored.** An input method that corrects a selection
  asks for text to be deleted before *and* after the caret; only Backspaces were sent, so anything
  after the caret stayed. The game's fields answer to Delete as well as Backspace, and the bridge
  sends both now.

### The Views

The other half of the Java layer *is* the Views, and Robolectric constructs them: a JVM-only
Android, which lays a button out, clicks it, delivers a MotionEvent and calls the surface callbacks
the way the framework would, with no device and no emulator. The one thing it cannot do is load
`libscacelith.so` (arm64 only), so every method of `Native` is declared native in the app, and
`app/src/test/java/com/scacelith/game/ShadowNative.java` replaces all twelve of them with a
recording no-op: the tests assert on what the Views *sent* the game, which is exactly the seam a
device cannot show.

```sh
cd android && ANDROID_HOME=$HOME/android-sdk JAVA_HOME=/usr/lib/jvm/java-17-openjdk \
    ~/.gradle/wrapper/dists/gradle-8.13-bin/*/gradle-8.13/bin/gradle --no-daemon testDebugUnitTest
# 82 tests, 0 failures  (24 plain Java, 58 here: org.robolectric:robolectric:4.14.1, API 35)
```

- `ScacelithActivityTest` — the window and its overlay. The eight buttons: each shows its entry's
  label and reads out its description, each sends its key as a press *and* a release and nothing
  else, the keyboard button sends no key at all and makes the soft keyboard visible, a real touch
  (a finger down, up, then the looper that `View` posts the click on) reaches the game, and a touch
  that slides off the button presses nothing. Then the game's lifecycle: `onCreate` calls
  `nativeInit` (handing it the activity and its private folder) and `nativeStart` once, `onPause`
  parks the loop, `onResume` wakes it, `onDestroy` ends it. And the keys the activity does *not*
  take: it has no `onKeyDown`, so a volume key answers the superclass's false and reaches neither
  the game nor anything of the app's.
- `BoardTouchTest` — where a finger lands, once the window has been laid out. ScacelithActivity
  stacks a full-screen SurfaceView and the strip of buttons over its bottom-right corner; the other
  tests send their events straight to a View, which never asks who *should* have received them.
  This one goes through the FrameLayout after a real measure/layout pass (before it every View is
  0 by 0 and a hit test means nothing), so hit testing decides. A touch in the middle of the board
  reaches the game with its own coordinates; a touch inside the strip's rectangle but in the margin
  between two buttons falls through to the board underneath; a finger that starts on the board and
  slides across the buttons stays the board's -- a stray key mid-drag is a move confirmed by
  accident; and the strip keeps what is its own, a touch on a button sending that button's key and
  playing nothing. The layout assertion comes first, because it is the leading indicator: the strip
  must be a corner of the window and not the window, which is what a strip grown over the board
  would look like before it even started eating touches.
- `GameViewTouchTest` — the touch translation, and in particular *which* pointer each action is
  about: DOWN, POINTER_DOWN, UP and POINTER_UP carry `event.getActionIndex()`, MOVE carries every
  pointer it holds together with how many are down, CANCEL carries none, and an action the game has
  no use for is dropped (the view keeping the stream in every case). One of the events is not one
  any device builds -- three pointers, the event about the middle one -- because
  `getPointerCount() - 1` and `getActionIndex()` name the same pointer in every event Android
  produces, and only such an event tells the two apart.
- `GameViewSurfaceTest` — the surface. The view registers itself with its own holder, which is the
  framework's fake one in a JVM test (its `getSurface()` is null), so the rest of the lifecycle is
  driven by hand through a holder the test owns: `surfaceCreated` hands *that* surface to the game,
  `surfaceChanged` and `onSizeChanged` both report the new size (twice for one resize -- the native
  side resizes the EGL surface twice, which is harmless, and it is now pinned), `surfaceDestroyed`
  parks the loop, and going to the background and coming back creates the surface again.
- `GameViewInputConnectionTest` — the connection an input method is handed, which is where a soft
  keyboard on a phone meets the game's fields: the view says it is a text editor and asks for a
  text keyboard with a Done action; a committed word reaches `nativeText` in one piece, several
  commits in order, an empty one nothing, and a composing word nothing either (an input method
  composes before it commits, and the fields would otherwise hold letters the player has not
  accepted); deleting before the caret is a Backspace per character and after it a Delete per
  character, in that order; the action key is Enter; and a key the input method sends itself
  (`sendKeyEvent`) reaches the game with both edges while typing no text of its own -- an input
  method that sent the letters of a word as key events would type everything twice -- while a key
  the game has no use for is refused, which is how the input method learns to handle it itself.
- `GameViewKeyboardTest` — a physical keyboard, the one path an input method does not take. A
  letter carries *two* things and the game wants both: the character the layout produces
  (lowercase unshifted, uppercase shifted -- a player naming a save gets the capital) to
  `nativeText`, and the game's own uppercase letter key to `nativeKey`, in that order, the release
  sending the key and typing nothing. Space types a space *and* presses the clock key, and a held
  letter repeats the way a held key does on a desktop (`WM_CHAR`, `XLookupString`). Then the keys
  that must type nothing at all: every named key (Escape, the back button, Tab, Enter, Backspace,
  Delete, the four arrows, Home/End, Page Up and Down, F1 and F12) and every modifier (Shift, Ctrl,
  Alt) presses its key and sends no text -- an arrow or an Escape that typed something would drop a
  control character into whatever field is open. A key the game has no use for (the volume keys) is
  left to the system: `GameView.onKeyDown` answers with the superclass's false, and the activity
  overrides no key method at all.

Each of these was mutation-checked, one at a time: `getPointerCount() - 1` for `getActionIndex()`, `w`
for `h` in `onSizeChanged`, the key release dropped, the MOVE loop cut to one pointer, the real
count on CANCEL, `getHolder().addCallback(this)` removed; in the four overrides of the input
connection, `commitText` not committing, the two delete lengths swapped, `sendKeyEvent` dropped and
`performEditorAction` doing nothing; and, on the keyboard, the typed character dropped, the key
taken from the layout instead of from `KeyMap`, text typed on a key's *release*, and the
filter on what may be typed removed; on the overlay, the button bar made clickable (so it answers
for its whole rectangle) and the overlay laid out to fill the window; and, on the activity, an
`onKeyDown` put back to swallow the volume keys. Each fails exactly the tests written for it
(`the_pointer_that_went_up_is_the_action_index`, `a_resize_is_reported_with_the_new_size`,
`pressing_a_button_sends_its_key_once`, `a_move_reports_every_pointer_it_carries`,
`a_cancelled_stream_forgets_the_pointers`, `the_view_listens_to_its_own_holder`,
`a_committed_word_is_typed_into_the_game`, `deleting_before_the_caret_is_a_backspace`,
`a_key_the_input_method_sends_goes_to_the_game_with_both_edges`,
`the_action_key_confirms_the_field`, `a_letter_types_its_character_and_presses_the_letter_key`,
`a_key_with_a_name_presses_the_games_key_and_types_nothing`,
`the_release_sends_the_key_and_types_nothing`,
`a_modifier_presses_the_games_key_and_types_nothing`,
`a_touch_between_two_buttons_is_the_boards`,
`the_board_fills_the_window_and_the_strip_only_covers_its_corner`,
`the_activity_leaves_the_keys_the_game_has_no_use_for_to_the_system`), and nothing else.

Robolectric brings `androidx.test:monitor` and `androidx.test.espresso:espresso-idling-resource`
into the unit-test classpath, where the Android Gradle plugin refuses to resolve them while
`android.useAndroidX` is false ("contains AndroidX dependencies, but the 'android.useAndroidX'
property is not enabled"). The property is now true, jetifier stays off, and none of it reaches the
APK: `assembleDebug` and `assembleRelease` still produce the same 137.2 MB and 127.9 MB files, and
no dex in either mentions Robolectric or AndroidX.

What these tests cannot prove: EGL, AAudio, the JNI/EGL glue around the touch queue, the GLSL ES
3.20 re-emission, and how the gestures and the buttons feel on a real screen.

### The two halves, joined by a file

One thing is still untested after all of the above, and it is the one a player would feel: the two
halves have to *agree*. GameViewTouchTest checks the View against what the native layer is supposed
to be told, and touch_input_tests.cpp checks the gesture machine against events a C++ test made up --
neither can run the other's toolchain, so a mismatch between them is invisible to both.

So the seam is written down. `TouchTraceTest` drives thirty-nine gestures of thirteen kinds through
the real View (random coordinates from a fixed seed, one frame per step, including a tap that
arrives whole inside one frame) and writes what the View sent the native layer, line by line, to
`build/android-touch-trace.txt`. `tests/touch_bridge_tests.cpp` replays that file through
`plat::TouchInput` and checks what the game would have received. The trace carries the View's own
output -- taken from the recorded calls, not from what the generator meant to send -- and its
`viewport` line is the size the View reported to the native layer, so the pinch's pixels-per-notch
(6% of it) is checked as the number crossing from one half to the other.

What the replay holds, frame by frame:

- an **oracle for the movement**: the cursor moved by exactly the centroid movement the trace's own
  coordinates describe -- and by nothing on the frames where a gesture starts, ends or loses a
  finger, which is where the machine re-takes its reference; the wheel leaned by the grip change,
  over the unit the View's size gives it;
- **the gesture's meaning**: one finger drags and is a click; a tap is a click *even when it arrived
  whole inside one frame*, the press and the release landing in different frames, which is what the
  deferred release is for; two fingers arriving together never grab a piece; a two-finger tap is the
  clock and a three-finger one the menu; a cancelled gesture is not a tap; the overlay's clock button
  reaches the game with both edges in one frame;
- **nothing leaks**: every one of the thirty-nine gestures ends with no finger down and no button
  held, in the same `plat::TouchInput` the previous gestures left behind.

The trace is the Android tests' build artefact, so this is two commands, and the replay skips --
keeping a desktop-only build green -- when the file is not there:

```sh
cd android && ANDROID_HOME=$HOME/android-sdk JAVA_HOME=/usr/lib/jvm/java-17-openjdk \
    ~/.gradle/wrapper/dists/gradle-8.13-bin/*/gradle-8.13/bin/gradle --no-daemon testDebugUnitTest
cd .. && SCACELITH_SOURCE_DIR=$(pwd) ./build/scacelith_tests touch_bridge
# 1 test(s), 0 failed   ([SKIP] when build/android-touch-trace.txt has not been written)
```

Replaying it found one thing worth a decision, and the replay pins it rather than hiding it: a
**look that ends one finger at a time** leaves the remaining finger as a one-finger drag, so the
machine presses the left button where that finger is and releases it where it lifts. In the game
that is a piece picked up and put back down under the cursor: a look can end by moving a piece by
accident. `look_lift_one` walks exactly that shape, four times per run; changing it should be
deliberate.

And it bites in both directions. On the C++ side, swapping x and y in the centroid, changing the
pinch's 6%, widening the tap window so that a look presses the clock, dropping the deferred release,
and turning `ACTION_CANCEL` into a move each fail the replay -- 171 checks over those five, in the
language of the gesture: a look that grabs a piece, a two-finger tap that presses nothing, fingers
left down. On the Java side, forgetting `ACTION_POINTER_DOWN` in the View's switch still writes a
trace, and the replay of it reports a look that grabs a piece, a pinch that grabs one, a clock that
never presses and a menu that never opens.

### Shared-core tables: bounded on purpose

The sweep that made the ES texture slots and the touch pointer table grow -- both reused or dropped
a *wrong* entry when full -- also walked the shared core the port ships. These tables are bounded on
purpose, their full case reuses the right entry or refuses, and none should be reopened:

- **Effect voices, `kMaxVoices = 32`** (`audio/mixer.h`). A play with every voice busy steals the
  *oldest* effect (`Mixer::play`), never an arbitrary one. Speech is a separate pool
  (`kMaxSpeech = 2`), so effects cannot cut the coach; pinned by `audio_voice_not_stolen_by_effects`
  and by `audio_voice_pool_full_reuses_the_oldest_effect`, which fills the pool and checks the 33rd
  play reuses the voice it panned hard right while the coach stays `Playing`.
- **Retired buffers, `retired_[512]`** (`audio/mixer.cpp`). The overflow branch frees on the audio
  thread and its comment calls it unreachable; it is: the bank is at most `kBankVariants` ×
  `Sfx::Count` (~126) buffers, and one block retires at most `incoming` (256) + voices (32) + speech
  chunks (64), under 512, while the owner drains the list every block. Do not make it a
  `std::vector`: growing it would allocate inside the audio callback.
- **Bird slots, `birds_[3]`** (`audio/ambience.cpp`). `startPhrase` with all three busy returns
  without touching a slot -- the phrase is skipped and rescheduled, never written over a sounding
  bird. Pinned by `audio_ambience_bird_pool_skips_when_full`, which fills all three slots and checks
  a fourth phrase is refused, the windows of the three are unchanged, and a slot freed by rendering
  is then reused.
- **Speech FIFO, `fifo[kSpeechChunks]` (64)** (`audio/mixer.cpp`). A full FIFO refuses the chunk,
  retires it and counts it in `chunksDone`; the ring index is written only while `count < 64`, so it
  cannot overwrite the head. Pinned by `audio_voice_volume_and_chunk_ownership`.
- **`MpmcQueue`** (`audio/queue.h`) and the **TTS model file table** (`tts/model.h`, checked
  against `kFileCount`): push *fails* when full rather than overwriting, and out-of-range entries
  are rejected before use.

The other silent drops are policies, not bugs: a full refresh queue keeps the old variant, and a
stalled command queue drops a play request.

## Notes on this environment

- The desktop build is the regression test. It does not need Stockfish
  (`-DSCACELITH_STOCKFISH=OFF` avoids the GCC+binutils requirement), which is also how it is built
  here: the SDK build needs the 98.5 MB `third_party/stockfish/src/nn-1a298aa575a0.nnue`, and a
  sparse-checkout clone that filtered it out fails the Android build with exactly that message
  (`third_party/stockfish/README.scacelith.md` says how to materialize it).
- The tests need the repository root for their fixtures:
  `SCACELITH_SOURCE_DIR=$(pwd) ./scacelith_tests`.
- Packaging an APK over an existing file leaves the previous one's data behind as dead space:
  before the engine was embedded, the content that made a 38 MB APK sat inside a 75.8 MB file
  after a few `assembleDebug` runs (and 175 MB after a few more), the excess being beyond the last
  zip entry — today's sizes make the dead space larger still. Delete the APK
  before a build that has to be shipped: `rm android/app/build/outputs/apk/debug/app-debug.apk`,
  then rebuild.
- The first `testDebugUnitTest` needs the network: Robolectric downloads
  `org.robolectric:android-all-instrumented:15-robolectric-12650502-i7` (API 35, ~200 MB) from
  Maven Central, looks in `~/.m2/repository` first, and keeps it there -- so a later run, and a run
  after `rm -rf` on Gradle's caches, resolves without it.
- The TTS instruction-set audit parses the linker's memory map, so it needs an English locale:
  `LC_ALL=C ninja`. Run in a French locale, `isa_audit.py` reports "no memory map" because the
  linker wrote "Configuration de la mémoire".
- `tests/coach_appraisal_tests.cpp` used `std::find` without `<algorithm>`: it compiles only
  because another header happens to pull it in on older libstdc++. Added.
