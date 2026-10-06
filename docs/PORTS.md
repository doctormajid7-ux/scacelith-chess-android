# Later ports: AppImage, Linux aarch64, macOS

**Status: research only, nothing shipped** — except Android, which is built and documented
separately in [ANDROID.md](ANDROID.md): the arm64 notes below (x86-only `SF_ISA_*` flags, x86-only
TTS kernels, `DenormalGuard` a no-op) are the ones that port turned out to need. Version 1.0.0-beta.2 releases the Linux x86-64 client
as a `.tar.gz` archive (see the README). This document keeps what was found while preparing the
other targets, so that the work can start from it.

## AppImage (x86-64, later aarch64)

**Layout of the AppDir**
- `AppRun`: a symlink to `usr/bin/scacelith`.
- `scacelith.desktop` (the same file as `res/linux/scacelith.desktop`) and `scacelith.png` (the
  256 px icon) at the root; appimagetool makes `.DirIcon` a symlink to the root icon.
- `usr/share/applications/scacelith.desktop`, `usr/share/icons/hicolor/<N>x<N>/apps/scacelith.png`
  (the sizes of `res/icons/png/`), `usr/share/doc/scacelith/` (LICENSE and the licences folder of
  the archive).
- An AppStream metainfo file is optional (appimagetool only warns).

**Building it**
- `ARCH=x86_64 appimagetool --appimage-extract-and-run --no-appstream --runtime-file runtime AppDir
  Scacelith-<version>-linux-x86_64.AppImage`. With `--runtime-file` it works offline and the output
  is byte-identical from one run to the next (tested: 94.6 MB for the 1.0.0-beta.1 binary).
- Without `--runtime-file`, appimagetool downloads the "continuous" runtime at build time: pin it.
- Pins checked on 2026-10-04 (to be updated by hand, Dependabot does not see them):

| File | URL | SHA-256 |
|---|---|---|
| appimagetool 1.9.1 x86_64 | `https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage` | `ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0` |
| appimagetool 1.9.1 aarch64 | `…/1.9.1/appimagetool-aarch64.AppImage` | `f0837e7448a0c1e4e650a93bb3e85802546e60654ef287576f46c71c126a9158` |
| type2-runtime 20251108 x86_64 | `https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64` | `2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d` |
| type2-runtime 20251108 aarch64 | `…/20251108/runtime-aarch64` | `00cbdfcf917cc6c0ff6d3347d59e0ca1f7f45a6df1a428a0d6d8a78664d87444` |

**Behaviour to keep in mind**
- The static type2 runtime needs no libfuse2, only `fusermount`/`fusermount3`. Without them it
  prints "No suitable fusermount binary found" and still runs; `--appimage-extract-and-run` or
  `APPIMAGE_EXTRACT_AND_RUN=1` work too. The runtime sets `APPIMAGE` and `APPDIR`.
- Never bundle libssl (a bundled copy looks for its CA store at the build distribution's path and
  breaks TLS on Fedora, Arch, openSUSE...), libasound (dlopen'd: the host's library, configuration
  and PulseAudio/PipeWire plugins must be used), libGL or libX11.
- The game keeps its settings, log and session token in `$XDG_CONFIG_HOME/scacelith/` (by default
  `~/.config/scacelith/`) on Linux unless a `Scacelith.ini` stands next to the executable (portable
  mode): in extract-and-run mode the executable lives in `$TMPDIR/appimage_extracted_<hash>/usr/bin`, so the portable mode must never
  apply to an AppImage.

## Linux aarch64

**Build and test environment**
- GitHub runners `ubuntu-22.04-arm` (the release toolchain: GCC 11, binutils 2.38, OpenSSL 3.0.2)
  and `ubuntu-24.04-arm`, free for public repositories and known to actionlint 1.7.12.
- Locally, cross-compile with `cmake/aarch64-linux-gnu.cmake` (Debian/Ubuntu multiarch: `dpkg
  --add-architecture arm64`, the arm64 sources from ports.ubuntu.com, `g++-aarch64-linux-gnu`,
  `libssl-dev:arm64 libx11-dev:arm64 libgl-dev:arm64`) and run the tests under `qemu-aarch64 -L
  /usr/aarch64-linux-gnu` (`qemu-user`). The toolchain file sets `CMAKE_CROSSCOMPILING_EMULATOR`.
- OpenGL 4.6 on aarch64 means NVIDIA (Jetson Orin, discrete GPUs), AMD discrete GPUs or Asahi Linux
  on Apple M1/M2. A Raspberry Pi 4/5 (V3D, desktop GL 3.1) cannot run the game.

**What does not build or run today**
- Stockfish: `SF_ALL_VARIANTS` (`third_party/stockfish/CMakeLists.txt`) and the `SF_ISA_*` flags are
  x86-64 only (`-msse2`, `-mavx2`...: the cross build stops on all five variants). The Android port
  has closed this gap for its toolchain and is the recipe to port here: two variants (`armv8`,
  `armv8-dotprod` with `-march=armv8.2-a+dotprod`), isolated by
  `third_party/stockfish/cmake/isolate_llvm.cmake` (written for the NDK's LLVM tools — lld, no
  `--force-group-allocation` — and verified to work with them), dispatched on
  `getauxval(AT_HWCAP) & HWCAP_ASIMDDP` by `scacelith/cpu_arm64.cpp` (the counterpart of
  `scacelith/cpu.cpp`), with the arm64 audit `cmake/isa_check_arm64.cmake` in the place of the x86
  `tools/isa_audit.py` and the NNUE network embedded. The cross build needs the same block adapted
  to a GNU aarch64 toolchain (the desktop `isolate.cmake` already handles ELF with binutils; pick
  the flags and the dispatcher from the Android CMakeLists).
- TTS: the kernels are SSE2/AVX2/AVX-VNNI/AVX-512 (`src/tts/kernels_*.cpp`), the dispatch reads
  cpuid (`src/tts/cpu.cpp`) and `src/tts/threads.cpp` uses `_mm_getcsr` unconditionally; a NEON or
  portable kernel set and an aarch64 dispatch are needed, and the TTS instruction-set audit in
  `CMakeLists.txt` is x86 only.
- TTS: the kernels are SSE2/AVX2/AVX-VNNI/AVX-512 (`src/tts/kernels_*.cpp`), the dispatch reads
  cpuid (`src/tts/cpu.cpp`) and `src/tts/threads.cpp` uses `_mm_getcsr` unconditionally; a NEON or
  portable kernel set and an aarch64 dispatch are needed, and the TTS instruction-set audit in
  `CMakeLists.txt` is x86 only.
- Audio: `DenormalGuard` (`src/audio/dsp.h`) does nothing on aarch64; set FPCR.FZ (bit 24) with
  `mrs`/`msr fpcr` there (the audio tests assert that no denormal reaches the output).
- GCC contracts `a*b+c` into `fmadd` by default on aarch64: results are not bitwise identical to
  x86-64; tests with tolerances are fine, bit-exact ones may need `-ffp-contract=off`.
- `char` is unsigned on aarch64 Linux: check any code that assumes it is signed.

## macOS (Apple Silicon)

Not possible as a build job alone; it is a port:
- macOS caps OpenGL at 4.1 core, while the renderer needs `#version 460 core`, compute shaders
  (post-processing, probes), direct state access, SSBOs, image load/store, `glClipControl`, texture
  views and multi-draw indirect: a Metal renderer (or a GL-on-Metal translation layer) is needed.
- No Cocoa platform layer (window, GL context, input, high DPI) and no CoreAudio backend.
- The Stockfish variant isolation handles ELF and PE with GNU binutils, and ELF with LLVM's tools
  (`cmake/isolate_llvm.cmake`, written for the Android NDK); macOS uses Mach-O and Apple's linker,
  for which there is no equivalent. OpenSSL is not part of macOS.
- Distribution needs Developer ID signing and notarization (a paid Apple account and repository
  secrets).
- The icon for it: `res/icons/png/scacelith-1024.png` and the smaller PNGs make the `.icns` with
  `iconutil -c icns` (iconset: `icon_16x16` = 16, `@2x` = 32, `icon_32x32` = 32, `@2x` = 64,
  `icon_128x128` = 128, `@2x` = 256, `icon_256x256` = 256, `@2x` = 512, `icon_512x512` = 512,
  `@2x` = 1024).

## Android (arm64-v8a)

Done: see [ANDROID.md](ANDROID.md). The AI opponent is embedded (one armv8/NEON variant of
Stockfish 19, the NNUE network embedded, verified at build level — never run on a device). The
gaps left there are named and scoped — the network's TLS, the direct match's cipher operations and
the TTS kernels — and the aarch64 section above holds the groundwork for the last one.
