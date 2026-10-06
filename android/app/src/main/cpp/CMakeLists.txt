# Scacelith for Android: the same sources as the desktop builds, plus the Android platform layer,
# the OpenGL ES 3.2 backend and the AAudio backend.
#
# Differences from the top level CMakeLists.txt, all of them "this platform has no X":
#   * OpenGL ES 3.2 instead of OpenGL 4.6 core: src/gl/gl46_gles.cpp emulates the direct state
#     access the renderer is written against, and shader.cpp emits GLSL ES 3.20 (SCACELITH_GLES).
#     Tessellation does not exist in ES; the renderer's own untessellated path is used (Low and
#     Medium presets already do).
#   * Audio: AAudio (src/audio/backend_aaudio.cpp) instead of WASAPI or ALSA.
#   * TTS: the x86 SIMD kernel units are left out; src/tts/kernels_portable.cpp provides the scalar
#     table, so the coach's voice works (slower) instead of not at all.
#   * Networking: the NDK has no TLS library, so the OpenSSL transport and the BCrypt/OpenSSL
#     direct-match crypto are replaced by src/net/transport_android.cpp and
#     src/net/direct_crypto_android.cpp (portable hashes, honest "unavailable" for TLS and
#     AES-GCM/ECDH). Single player, the coach, two players at one device and the observer mode are
#     unaffected.
#   * Stockfish: two arm64 variants (armv8 and armv8-dotprod) with the desktop's variant
#     machinery adapted to the NDK's LLVM tools; see SCACELITH_STOCKFISH_ANDROID below.
#
# The build is driven by Gradle (android/app/build.gradle.kts, which passes SCACELITH_ROOT and the
# ABI); it can also be run by hand with the NDK's toolchain file:
#   cmake -B build-arm64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
#         -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DSCACELITH_ROOT=$(pwd)
cmake_minimum_required(VERSION 3.22)
project(scacelith C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(NOT SCACELITH_ROOT)
    message(FATAL_ERROR "pass -DSCACELITH_ROOT=<repository root> (Gradle does)")
endif()
set(ROOT "${SCACELITH_ROOT}")
if(NOT EXISTS ${ROOT}/src/main.cpp)
    message(FATAL_ERROR "SCACELITH_ROOT (${ROOT}) is not the Scacelith repository root")
endif()

# Stockfish (GPL-3.0), embedded as on the desktop (third_party/stockfish/README.scacelith.md) but
# for arm64: two variants, armv8 (upstream's ARCH=armv8, baseline AArch64 with NEON) and
# armv8-dotprod (upstream's ARCH=armv8-dotprod, adding FEAT_DOTPROD). NEON and POPCNT are part of
# what the arm64-v8a ABI guarantees; the dotprod extension is not, so scacelith/cpu_arm64.cpp
# dispatches on getauxval(AT_HWCAP) & HWCAP_ASIMDDP — the same runtime check as upstream's own
# universal/entry_arm64.cpp. The desktop's machinery carries over: the per-variant compile
# (SCACELITH_SF_VARIANTS), the isolation of each variant into one object that exports only its
# entry point and the bounds of its initialiser table, and the dispatcher that chooses the variant
# the CPU runs best and runs its static initialisers by hand. What changes is the toolchain: the
# NDK has LLVM's tools and no GNU binutils, so the isolation runs cmake/isolate_llvm.cmake (lld
# has no --force-group-allocation, and llvm-objcopy takes the symbols to keep global from a file),
# and cmake/isa_check_arm64.cmake checks each isolated object (no dotprod instruction in the
# baseline, at least one in the dotprod variant) where the desktop audits with tools/isa_audit.py.
# GCC-only flags give way to clang's: -fconstexpr-ops-limit -> -fconstexpr-steps, no
# -fno-ipa-cp-clone; the NDK toolchain is clang.
option(SCACELITH_STOCKFISH_ANDROID "Embed Stockfish 19 (armv8 + armv8-dotprod variants, the NNUE network embedded)" ON)

# The full version string (src/... everywhere) and the embedded data table.
include(${ROOT}/cmake/version.cmake)
configure_file(${ROOT}/cmake/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/scacelith_version.h @ONLY)
include(${ROOT}/cmake/embed.cmake)
set(EMBED_CPP ${CMAKE_CURRENT_BINARY_DIR}/embedded_files.cpp)
scacelith_embed_files(${EMBED_CPP} ${ROOT} shaders assets)

set(SCACELITH_WARNINGS -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

# ---------------------------------------------------------------------------------------------
# The engine-free modules (same split as the desktop CMakeLists.txt)
# ---------------------------------------------------------------------------------------------
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS
    ${ROOT}/src/core/*.cpp ${ROOT}/src/math/*.cpp ${ROOT}/src/chess/*.cpp ${ROOT}/src/ai/*.cpp ${ROOT}/src/audio/*.cpp
    ${ROOT}/src/i18n/*.cpp ${ROOT}/src/net/*.cpp ${ROOT}/src/coach/*.cpp ${ROOT}/src/tts/*.cpp)
set(GAME_CORE_SOURCES ${ROOT}/src/game/elo.cpp ${ROOT}/src/game/camera_flight.cpp ${ROOT}/src/game/observer_camera.cpp
    ${ROOT}/src/game/hotseat.cpp ${ROOT}/src/game/scoresheet_layout.cpp ${ROOT}/src/game/physical_board.cpp
    ${ROOT}/src/game/settings_coach.cpp ${ROOT}/src/game/game_archive.cpp ${ROOT}/src/game/game_saving.cpp
    ${ROOT}/src/game/replay.cpp ${ROOT}/src/game/online_account.cpp ${ROOT}/src/game/online_mock.cpp)
list(APPEND CORE_SOURCES ${GAME_CORE_SOURCES})
list(REMOVE_ITEM CORE_SOURCES
    # Audio: the platform backends that are not ours.
    ${ROOT}/src/audio/backend_alsa.cpp
    ${ROOT}/src/audio/backend_wasapi.cpp
    # Networking: replaced by the Android files (transport_android.cpp, direct_crypto_android.cpp).
    ${ROOT}/src/net/transport_openssl.cpp
    ${ROOT}/src/net/transport_win32.cpp
    ${ROOT}/src/net/direct_crypto.cpp
    # TTS: the x86 kernel units (kernels_portable.cpp provides the table of this build).
    ${ROOT}/src/tts/kernels_sse2.cpp
    ${ROOT}/src/tts/kernels_avx2.cpp
    ${ROOT}/src/tts/kernels_avxvnni.cpp
    ${ROOT}/src/tts/kernels_avx512.cpp)

add_library(scacelith_core STATIC ${CORE_SOURCES} ${EMBED_CPP})
# The speech kernels are the portable table (src/tts/kernels_portable.cpp, vectorized to NEON by
# the compiler), checked against the x86 ones by the desktop tests.
target_include_directories(scacelith_core PUBLIC ${ROOT}/src ${ROOT}/third_party ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_options(scacelith_core PRIVATE ${SCACELITH_WARNINGS})
# The bundled <KHR/khrplatform.h> of third_party/khronos/GL/glcorearb.h, as on the desktop.
target_include_directories(scacelith_core SYSTEM PUBLIC ${ROOT}/third_party/khronos)

# ---------------------------------------------------------------------------------------------
# Stockfish (GPL-3.0): two arm64 variants compiled into the game, the NNUE network embedded with
# Stockfish's own INCBIN, exactly as on the desktop (third_party/stockfish/CMakeLists.txt) but
# with the x86 variants and the GNU binutils isolation replaced by the arm64 ones. The game only
# sees scacelith/stockfish_embedded.h (src/ai runs the UCI loop on a thread);
# scacelith/cpu_arm64.cpp is the dispatcher this build has.
# ---------------------------------------------------------------------------------------------
if(SCACELITH_STOCKFISH_ANDROID)
    set(SF_DIR ${ROOT}/third_party/stockfish)
    set(SF_SRC ${SF_DIR}/src)
    set(SF_GLUE ${SF_DIR}/scacelith)
    set(SF_NET ${SF_SRC}/nn-1a298aa575a0.nnue)
    if(NOT EXISTS ${SF_NET})
        message(FATAL_ERROR "SCACELITH_STOCKFISH_ANDROID is ON but ${SF_NET} is missing (a sparse-checkout filtered clone leaves the *.nnue out: materialize it as in third_party/stockfish/README.scacelith.md, or -DSCACELITH_STOCKFISH_ANDROID=OFF builds without the engine)")
    endif()

    # The variants, from the baseline up, with the flags of upstream's Makefile for their ARCH:
    # armv8: -O3 -funroll-loops -fno-exceptions -Wall -Wcast-qual, -DUSE_POPCNT -DUSE_NEON=8 (no
    # -mfpu and no -march: NEON is the arm64 baseline); armv8-dotprod adds -march=armv8.2-a+dotprod
    # -DUSE_NEON_DOTPROD. clang 18 emits SDOT for the dotprod variant and none for the baseline
    # (cmake/isa_check_arm64.cmake enforces it).
    set(SF_VARIANTS armv8 armv8-dotprod)
    set(SF_ISA_armv8 -DUSE_POPCNT -DUSE_NEON=8)
    set(SF_ISA_armv8-dotprod ${SF_ISA_armv8} -march=armv8.2-a+dotprod -DUSE_NEON_DOTPROD)

    # The NDK ships LLVM's tools, not GNU binutils; resolve them next to the compiler.
    get_filename_component(_sf_tool_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(SF_LLD NAMES ld.lld HINTS "${_sf_tool_dir}" REQUIRED)
    find_program(SF_OBJCOPY NAMES llvm-objcopy HINTS "${_sf_tool_dir}" REQUIRED)
    find_program(SF_OBJDUMP NAMES llvm-objdump HINTS "${_sf_tool_dir}" REQUIRED)
    find_program(SF_NM NAMES llvm-nm HINTS "${_sf_tool_dir}" REQUIRED)
    find_program(SF_READELF NAMES llvm-readelf HINTS "${_sf_tool_dir}" REQUIRED)

    # The same upstream sources as the desktop's sf_obj_<tag>, byte-identical, each variant with
    # its namespace renamed (Stockfish_armv8 / Stockfish_armv8_dotprod, as upstream's universal
    # build names them) and the network only declared: UNIVERSAL_BINARY makes nnue/network.cpp
    # declare what scacelith/nnue_incbin.cpp embeds. The forced include replaces upstream's
    # src/shm.h: the network stays in process memory (no /tmp directory, no Unix socket, no
    # memfd — local_shm.h).
    set(SF_SOURCES
        attacks.cpp benchmark.cpp bitboard.cpp engine.cpp evaluate.cpp memory.cpp misc.cpp movegen.cpp
        movepick.cpp position.cpp score.cpp search.cpp thread.cpp timeman.cpp tt.cpp tune.cpp uci.cpp
        ucioption.cpp syzygy/tbprobe.cpp nnue/network.cpp nnue/nnue_accumulator.cpp nnue/nnue_misc.cpp
        nnue/features/full_threats.cpp nnue/features/half_ka_v2_hm.cpp nnue/features/pp_3wide.cpp)
    list(TRANSFORM SF_SOURCES PREPEND ${SF_SRC}/)
    set(SF_ISO_OBJECTS "")
    set(SF_VARIANT_TABLE "")
    foreach(arch IN LISTS SF_VARIANTS)
        string(REPLACE "-" "_" tag ${arch})
        add_library(sf_${tag} OBJECT ${SF_SOURCES} ${SF_GLUE}/entry.cpp)
        target_include_directories(sf_${tag} PRIVATE ${SF_SRC})
        target_compile_definitions(sf_${tag} PRIVATE
            Stockfish=Stockfish_${tag} UNIVERSAL_BINARY ARCH=${arch} SCACELITH_SF_TAG=${tag}
            NDEBUG IS_64BIT)
        # -fconstexpr-steps is clang's -fconstexpr-ops-limit (upstream passes it for its clang
        # builds; the 500M steps are what the constexpr attack-table loops need). No
        # -fno-ipa-cp-clone: clang has no such flag (upstream only passes it to GCC). Like on the
        # desktop, SF_ISA_<arch> carries both -D definitions and -m options, so it goes into
        # compile_options (definitions would turn -march=... into -D-march=...).
        target_compile_options(sf_${tag} PRIVATE
            -O3 -funroll-loops -fno-exceptions -fconstexpr-steps=500000000 -Wall -Wcast-qual
            ${SF_ISA_${arch}}
            "SHELL:-include ${SF_GLUE}/local_shm.h")
        set_target_properties(sf_${tag} PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS OFF)

        # One isolated object sf_<tag>.iso.o exporting scacelith_sf_main_<tag> and the bounds of
        # its initialiser table, then the arm64 ISA check on it (the desktop audits the final link
        # with tools/isa_audit.py; here one object per variant keeps it simple and exact).
        list(FIND SF_VARIANTS ${arch} sf_level)
        set(iso ${CMAKE_CURRENT_BINARY_DIR}/sf_${tag}.iso.o)
        add_custom_command(OUTPUT ${iso}
            COMMAND ${CMAKE_COMMAND} -DTAG=${tag} -DENTRY=scacelith_sf_main_${tag} -DOUT=${iso}
                    "-DINPUTS=$<JOIN:$<TARGET_OBJECTS:sf_${tag}>,|>"
                    -DLD=${SF_LLD} -DOBJCOPY=${SF_OBJCOPY} -DOBJDUMP=${SF_OBJDUMP} -DNM=${SF_NM}
                    -DREADELF=${SF_READELF}
                    -P ${SF_DIR}/cmake/isolate_llvm.cmake
            COMMAND ${CMAKE_COMMAND} -DOBJ=${iso} -DLEVEL=${sf_level} -DOBJDUMP=${SF_OBJDUMP}
                    -P ${SF_DIR}/cmake/isa_check_arm64.cmake
            DEPENDS sf_${tag} $<TARGET_OBJECTS:sf_${tag}>
                    ${SF_DIR}/cmake/isolate_llvm.cmake ${SF_DIR}/cmake/isa_check_arm64.cmake
            COMMENT "Isolating Stockfish variant ${arch}" VERBATIM)
        list(APPEND SF_ISO_OBJECTS ${iso})
        string(APPEND SF_VARIANT_TABLE " X(${tag}, \"${arch}\", ${sf_level})")
    endforeach()
    set_source_files_properties(${SF_ISO_OBJECTS} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/sf_variants.h CONTENT
"// Generated by android/app/src/main/cpp/CMakeLists.txt: the Stockfish variants of this build,
// from the baseline up, as X(tag, Stockfish ARCH name, index in SF_VARIANTS).
#define SCACELITH_SF_VARIANTS(X)@SF_VARIANT_TABLE@
" @ONLY)

    # The library the game links: the dispatcher (cpu_arm64.cpp: two variants, getauxval
    # dispatch), the embedded network and the variants' isolated objects. INCBIN's
    # .incbin "nn-1a298aa575a0.nnue" is resolved by clang's integrated assembler (no binutils in
    # the NDK): -Wa,-I points it at the network's directory (verified: NDK r27 clang 18, cc1as
    # honours -I for .incbin), and the file is a dependency so a changed network recompiles it.
    # Bionic keeps pthreads in libc (no -lpthread, unlike the desktop builds).
    add_library(stockfish_embedded STATIC
        ${SF_GLUE}/cpu_arm64.cpp ${SF_GLUE}/nnue_incbin.cpp ${SF_ISO_OBJECTS})
    set_source_files_properties(${SF_GLUE}/nnue_incbin.cpp PROPERTIES
        COMPILE_OPTIONS "-Wa,-I${SF_SRC}" OBJECT_DEPENDS ${SF_NET})
    target_compile_options(stockfish_embedded PRIVATE -O2 -Wall -Wextra)
    target_include_directories(stockfish_embedded PRIVATE ${SF_SRC} ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_include_directories(stockfish_embedded PUBLIC ${SF_GLUE})

    # As on the desktop CMakeLists.txt: the core modules see the engine (SCACELITH_HAS_STOCKFISH
    # gates src/ai/uci_host.cpp) and the whole engine stays private to stockfish_embedded.
    target_link_libraries(scacelith_core PUBLIC stockfish_embedded)
    target_compile_definitions(scacelith_core PUBLIC SCACELITH_HAS_STOCKFISH=1)
endif()

# ---------------------------------------------------------------------------------------------
# The game: every other .cpp, plus the Android platform layer, the ES backend and AAudio
# ---------------------------------------------------------------------------------------------
file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS
    ${ROOT}/src/platform/*.cpp ${ROOT}/src/gl/*.cpp ${ROOT}/src/render/*.cpp ${ROOT}/src/scene/*.cpp
    ${ROOT}/src/character/*.cpp ${ROOT}/src/anim/*.cpp ${ROOT}/src/ui/*.cpp ${ROOT}/src/game/*.cpp
    ${ROOT}/src/app/*.cpp)
list(REMOVE_ITEM GAME_SOURCES ${GAME_CORE_SOURCES} ${ROOT}/src/platform/platform_win32.cpp
    ${ROOT}/src/platform/platform_x11.cpp)

add_library(scacelith SHARED
    ${ROOT}/src/main.cpp
    ${GAME_SOURCES}
    ${ROOT}/src/platform/platform_android.cpp
    ${ROOT}/src/audio/backend_aaudio.cpp
    ${ROOT}/src/net/transport_android.cpp
    ${ROOT}/src/net/direct_crypto_android.cpp
    ${ROOT}/src/tts/kernels_portable.cpp)
target_compile_options(scacelith PRIVATE ${SCACELITH_WARNINGS})
# SCACELITH_GLES: shader.cpp emits GLSL ES 3.20 and the renderer leaves out what ES has no
# equivalent of (tessellation); gl46_gles.cpp is compiled in.
target_compile_definitions(scacelith PRIVATE SCACELITH_GLES=1)
target_link_libraries(scacelith PRIVATE scacelith_core android log EGL GLESv3 aaudio dl)
set_target_properties(scacelith PROPERTIES PREFIX "lib" OUTPUT_NAME "scacelith")
