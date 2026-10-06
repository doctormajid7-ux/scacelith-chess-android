cmake_minimum_required(VERSION 3.21)
# The version: cmake/version.cmake (SCACELITH_VERSION_CORE for project(), SCACELITH_VERSION in full).
include(cmake/version.cmake)
project(Scacelith VERSION ${SCACELITH_VERSION_CORE} LANGUAGES C CXX)

# ---------------------------------------------------------------------------------------------
# Build configuration
#   Windows (shipping):  cmake -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
#   Linux (tests only):  cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   Quicker local builds: add -DSCACELITH_SF_VARIANTS=x86-64-avx2 (or another variant this CPU
#   runs) to compile Stockfish once instead of once per instruction set (third_party/stockfish)
# ---------------------------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(SCACELITH_CONSOLE "Windows: build a console-subsystem exe (logs visible)" OFF)
option(SCACELITH_STOCKFISH "Embed the Stockfish engine" ON)
set(SCACELITH_OFFICIAL_SERVER "" CACHE STRING "Official online server: host[:apiPort[:wsPort]] (empty = caissa.scacelith.com:443, apiPort 443 when omitted, none = no official server)")

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR})
if(WIN32)
    enable_language(RC)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()
include(cmake/embed.cmake)

set(COMMON_WARNINGS -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers $<$<CXX_COMPILER_ID:GNU>:-Wno-dangling-reference>)
set(OPT_FLAGS $<$<CONFIG:Release>:-O2> $<$<CONFIG:RelWithDebInfo>:-O2 -g>)

# ---------------------------------------------------------------------------------------------
# Embedded data (shaders, fonts, ...) -> generated incbin table
# ---------------------------------------------------------------------------------------------
set(EMBED_CPP ${CMAKE_CURRENT_BINARY_DIR}/embedded_files.cpp)
set(EMBED_ITEMS shaders assets)
if(NOT WIN32)
    list(APPEND EMBED_ITEMS res/icons/scacelith.ico)   # the X11 window icon (Windows: the resource)
endif()
scacelith_embed_files(${EMBED_CPP} ${ROOT} ${EMBED_ITEMS})

# ---------------------------------------------------------------------------------------------
# Stockfish (GPL-3.0), compiled into the executable. See third_party/stockfish/README.scacelith.md
# ---------------------------------------------------------------------------------------------
if(SCACELITH_STOCKFISH)
    if(NOT EXISTS ${ROOT}/third_party/stockfish/CMakeLists.txt)
        message(FATAL_ERROR "SCACELITH_STOCKFISH is ON but third_party/stockfish is missing (-DSCACELITH_STOCKFISH=OFF builds without the engine)")
    endif()
    # The variants take GCC-only options and are isolated with GNU binutils (their isolate.cmake):
    # another compiler, or LLVM's ld/objcopy/objdump/nm, would fail deep in the build instead of here.
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        message(FATAL_ERROR "SCACELITH_STOCKFISH is ON but the compiler is ${CMAKE_CXX_COMPILER_ID}: the embedded Stockfish variants need GCC with GNU binutils (-DSCACELITH_STOCKFISH=OFF builds without the engine)")
    endif()
    foreach(tool LINKER OBJCOPY OBJDUMP NM)
        if(CMAKE_${tool})   # a missing one is reported by third_party/stockfish
            execute_process(COMMAND ${CMAKE_${tool}} --version OUTPUT_VARIABLE tool_version ERROR_QUIET)
            if(NOT tool_version MATCHES "^GNU ")
                message(FATAL_ERROR "SCACELITH_STOCKFISH is ON but ${CMAKE_${tool}} is not from GNU binutils: the embedded Stockfish variants need GCC with GNU binutils (-DSCACELITH_STOCKFISH=OFF builds without the engine)")
            endif()
        endif()
    endforeach()
    add_subdirectory(third_party/stockfish)
endif()

# ---------------------------------------------------------------------------------------------
# Engine-free modules shared by the game and the unit tests
# ---------------------------------------------------------------------------------------------
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS
    ${ROOT}/src/core/*.cpp ${ROOT}/src/math/*.cpp ${ROOT}/src/chess/*.cpp ${ROOT}/src/ai/*.cpp ${ROOT}/src/audio/*.cpp
    ${ROOT}/src/i18n/*.cpp ${ROOT}/src/net/*.cpp ${ROOT}/src/coach/*.cpp ${ROOT}/src/tts/*.cpp)
# Engine-free parts of src/game (Elo, observer camera, camera flights, hot-seat handover, scoresheet
# layout, physical board, the [coach] and [tts] settings, saved games, what the scene saves and their
# replay, the account pages' data, the in-process fake server): in the core library so the unit tests
# can use them, and left out of the game sources below.
set(GAME_CORE_SOURCES ${ROOT}/src/game/elo.cpp ${ROOT}/src/game/camera_flight.cpp ${ROOT}/src/game/observer_camera.cpp
    ${ROOT}/src/game/hotseat.cpp ${ROOT}/src/game/scoresheet_layout.cpp ${ROOT}/src/game/physical_board.cpp
    ${ROOT}/src/game/settings_coach.cpp ${ROOT}/src/game/game_archive.cpp ${ROOT}/src/game/game_saving.cpp
    ${ROOT}/src/game/replay.cpp ${ROOT}/src/game/online_account.cpp ${ROOT}/src/game/online_mock.cpp)
list(APPEND CORE_SOURCES ${GAME_CORE_SOURCES})
add_library(scacelith_core STATIC ${CORE_SOURCES} ${EMBED_CPP})
# The full version (cmake/version.cmake), for every module (generated/scacelith_version.h).
configure_file(${ROOT}/cmake/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/scacelith_version.h @ONLY)
target_include_directories(scacelith_core PUBLIC ${ROOT}/src ${ROOT}/third_party ${CMAKE_CURRENT_BINARY_DIR}/generated)
# <KHR/khrplatform.h> of third_party/khronos/GL/glcorearb.h (src/gl/gl46.h): the bundled copy, not
# the system's (libgl-dev), which a tests-only build may lack.
target_include_directories(scacelith_core SYSTEM PUBLIC ${ROOT}/third_party/khronos)
target_compile_options(scacelith_core PRIVATE ${COMMON_WARNINGS} ${OPT_FLAGS})
if(TARGET stockfish_embedded)
    target_link_libraries(scacelith_core PUBLIC stockfish_embedded)
    target_compile_definitions(scacelith_core PUBLIC SCACELITH_HAS_STOCKFISH=1)
endif()
if(WIN32)
    target_link_libraries(scacelith_core PUBLIC ole32 winmm avrt)
else()
    find_package(Threads REQUIRED)
    # dl: libasound.so.2 is loaded at run time (src/audio/backend_alsa.cpp; in libc from glibc 2.34)
    target_link_libraries(scacelith_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()
# Text-to-speech (src/tts): the compute kernels are compiled once per instruction set and chosen at
# run time (src/tts/kernels.h); everything else is baseline x86-64. MinGW does not align the stack
# to 32 bytes for AVX spills (GCC bug 54412): the assembler turns aligned vector moves into
# unaligned ones in those units. The model files are prepared by third_party/supertonic3.
set(TTS_AVX2_FLAGS -O3 -mavx2 -mfma)
set(TTS_AVXVNNI_FLAGS ${TTS_AVX2_FLAGS} -mavxvnni)
set(TTS_AVX512_FLAGS ${TTS_AVX2_FLAGS} -mavx512f -mavx512bw -mavx512vl -mavx512dq -mavx512vnni)
if(WIN32)
    foreach(v TTS_AVX2_FLAGS TTS_AVXVNNI_FLAGS TTS_AVX512_FLAGS)
        list(APPEND ${v} -Wa,-muse-unaligned-vector-move)
    endforeach()
endif()
set_source_files_properties(${ROOT}/src/tts/kernels_sse2.cpp PROPERTIES COMPILE_OPTIONS "-O3")
# The interpreter's element-wise loops: at -O2 GCC's vectorizer refuses every loop that needs a
# run-time alias check. Never add fast-math options: the bit-exact audio depends on their absence.
set_source_files_properties(${ROOT}/src/tts/ops.cpp PROPERTIES COMPILE_OPTIONS "-O3")
set_source_files_properties(${ROOT}/src/tts/kernels_avx2.cpp PROPERTIES COMPILE_OPTIONS "${TTS_AVX2_FLAGS}")
set_source_files_properties(${ROOT}/src/tts/kernels_avxvnni.cpp PROPERTIES COMPILE_OPTIONS "${TTS_AVXVNNI_FLAGS}")
set_source_files_properties(${ROOT}/src/tts/kernels_avx512.cpp PROPERTIES COMPILE_OPTIONS "${TTS_AVX512_FLAGS}")
# Audit of those units (Stockfish's tools/isa_audit.py, part of the build when Python is available):
# a small program links the TTS runtime like the game does, and the build fails if its disassembly
# shows an instruction above baseline outside the kernel units, one above a unit's level inside it,
# or a reference into a unit other than from the dispatcher (src/tts/cpu.cpp).
find_package(Python3 COMPONENTS Interpreter)
# The shipped exe (a Windows Release build) is never built without the instruction-set audits and
# the .pdata check below; other builds skip them with a warning.
string(TOUPPER "${CMAKE_BUILD_TYPE}" build_type)   # as $<CONFIG:Release> compares it
if(WIN32 AND build_type STREQUAL "RELEASE" AND NOT Python3_Interpreter_FOUND)
    message(FATAL_ERROR "Python 3 not found: a Windows Release build needs it for the instruction-set audits and the .pdata check")
endif()
if(Python3_Interpreter_FOUND AND EXISTS ${ROOT}/third_party/stockfish/tools/isa_audit.py)
    add_executable(tts_isa_probe ${ROOT}/tools/tts_isa_probe.cpp)
    target_link_libraries(tts_isa_probe PRIVATE scacelith_core)
    target_link_options(tts_isa_probe PRIVATE -Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/tts_isa_probe.map)
    if(WIN32)
        target_link_options(tts_isa_probe PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
    set(stamp ${CMAKE_CURRENT_BINARY_DIR}/tts_isa_audit.ok)
    add_custom_command(OUTPUT ${stamp}
        COMMAND ${Python3_EXECUTABLE} ${ROOT}/third_party/stockfish/tools/isa_audit.py
                $<TARGET_FILE:tts_isa_probe> ${CMAKE_CURRENT_BINARY_DIR}/tts_isa_probe.map ${CMAKE_OBJDUMP}
                "\\(kernels_avx2\\.cpp\\.o(bj)?\\)=2" "\\(kernels_avxvnni\\.cpp\\.o(bj)?\\)=2"
                "\\(kernels_avx512\\.cpp\\.o(bj)?\\)=3" --allow "scacelith_core\\.a\\(cpu\\.cpp\\.o(bj)?\\)"
                --report ${CMAKE_CURRENT_BINARY_DIR}/tts_isa_audit.txt
        COMMAND ${CMAKE_COMMAND} -E touch ${stamp}
        DEPENDS tts_isa_probe ${ROOT}/third_party/stockfish/tools/isa_audit.py
        COMMENT "Auditing the instruction sets of the TTS kernels" VERBATIM)
    add_custom_target(tts_isa_audit ALL DEPENDS ${stamp})
else()
    message(WARNING "Python 3 not found: the TTS instruction-set audit is skipped")
endif()
include(third_party/supertonic3/supertonic3.cmake)
# Networking (src/net): online client (WinHTTP, DPAPI, the shell for SSO), direct match (Winsock,
# BCrypt ECDH P-256 / AES-GCM / SHA-256, adapter list for iphlpapi) and UPnP on Windows. Linux
# development builds require OpenSSL 3 (online client and direct match).
set_source_files_properties(${ROOT}/src/net/online_client.cpp PROPERTIES COMPILE_DEFINITIONS
    "SCACELITH_OFFICIAL_SERVER=\"${SCACELITH_OFFICIAL_SERVER}\"")
set_source_files_properties(${ROOT}/tests/net_tests.cpp PROPERTIES COMPILE_DEFINITIONS
    "SCACELITH_OFFICIAL_SERVER=\"${SCACELITH_OFFICIAL_SERVER}\"")
if(WIN32)
    target_link_libraries(scacelith_core PUBLIC winhttp crypt32 bcrypt ws2_32 iphlpapi shell32)
else()
    find_package(OpenSSL 3.0 REQUIRED)
    target_link_libraries(scacelith_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(scacelith_core PRIVATE SCACELITH_HAS_OPENSSL=1)
endif()

# ---------------------------------------------------------------------------------------------
# Game executable. Every .cpp under src/ (except the core modules above) is compiled directly
# into the exe so static registration (scenes) is never dropped by the linker.
# ---------------------------------------------------------------------------------------------
file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS
    ${ROOT}/src/platform/*.cpp ${ROOT}/src/gl/*.cpp ${ROOT}/src/render/*.cpp ${ROOT}/src/scene/*.cpp
    ${ROOT}/src/character/*.cpp ${ROOT}/src/anim/*.cpp ${ROOT}/src/ui/*.cpp ${ROOT}/src/game/*.cpp
    ${ROOT}/src/app/*.cpp)
list(REMOVE_ITEM GAME_SOURCES ${GAME_CORE_SOURCES})
# src/gl/gl46_gles.cpp is the Android-only GL ES 3.2 layer: nothing on a desktop build defines
# SCACELITH_GLES, so the file is an empty translation unit there. Leave it out rather than compile
# it for nothing (the tests build it themselves, against a fake libGLESv2: see below).
list(REMOVE_ITEM GAME_SOURCES ${ROOT}/src/gl/gl46_gles.cpp)
set(GAME_EXTRA)
if(WIN32)
    # The icon, the version resource and the manifest, with the version of cmake/version.cmake.
    set(SCACELITH_ICON_ICO ${ROOT}/res/icons/scacelith.ico)
    configure_file(${ROOT}/res/scacelith.rc.in ${CMAKE_CURRENT_BINARY_DIR}/generated/scacelith.rc @ONLY)
    configure_file(${ROOT}/res/scacelith.manifest.in ${CMAKE_CURRENT_BINARY_DIR}/generated/scacelith.manifest @ONLY)
    # windres reports no dependencies: a changed icon or manifest must still rebuild the resource.
    set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/generated/scacelith.rc PROPERTIES
        OBJECT_DEPENDS "${SCACELITH_ICON_ICO};${CMAKE_CURRENT_BINARY_DIR}/generated/scacelith.manifest")
    list(APPEND GAME_EXTRA ${CMAKE_CURRENT_BINARY_DIR}/generated/scacelith.rc)
endif()
add_executable(scacelith ${ROOT}/src/main.cpp ${GAME_SOURCES} ${GAME_EXTRA})
target_compile_options(scacelith PRIVATE ${COMMON_WARNINGS} ${OPT_FLAGS})
target_link_libraries(scacelith PRIVATE scacelith_core)
if(WIN32)
    target_link_libraries(scacelith PRIVATE opengl32 gdi32 user32 shell32 dwmapi)
    target_link_options(scacelith PRIVATE -static -static-libgcc -static-libstdc++)
    # The release exe without its symbol table and debug sections: nothing reads them at run time.
    target_link_options(scacelith PRIVATE $<$<CONFIG:Release>:-s>)
    if(NOT SCACELITH_CONSOLE)
        target_link_options(scacelith PRIVATE -mwindows)
    endif()
    set_target_properties(scacelith PROPERTIES OUTPUT_NAME "Scacelith")
else()
    find_package(X11 REQUIRED)
    target_link_libraries(scacelith PRIVATE X11 GL dl)
    # The release executable runs on other distributions than the one it was built on: libstdc++
    # and libgcc are linked in (their versions differ between distributions, glibc's are
    # backward-compatible), and it is stripped like the Windows one.
    target_link_options(scacelith PRIVATE "$<$<CONFIG:Release>:-static-libgcc;-static-libstdc++;-s>")
    # The Android renderer on the desktop: the same sources built as the Android build builds them
    # (SCACELITH_GLES: GLSL ES 3.20 shaders, the DSA emulation of src/gl/gl46_gles.cpp), on an
    # OpenGL ES 3.2 context of the desktop driver (GLX_EXT_create_context_es_profile). A way to see
    # the ES path draw without a device: `ninja scacelith_gles` (not part of the default build).
    add_executable(scacelith_gles EXCLUDE_FROM_ALL ${ROOT}/src/main.cpp ${GAME_SOURCES} ${ROOT}/src/gl/gl46_gles.cpp)
    target_compile_definitions(scacelith_gles PRIVATE SCACELITH_GLES=1)
    target_compile_options(scacelith_gles PRIVATE ${COMMON_WARNINGS} ${OPT_FLAGS})
    target_link_libraries(scacelith_gles PRIVATE scacelith_core X11 GL dl)
endif()

# ---------------------------------------------------------------------------------------------
# Unit tests (no GPU): tests/*.cpp linked against the core modules. Run: ./scacelith_tests
# ---------------------------------------------------------------------------------------------
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${ROOT}/tests/*.cpp)
# The fake libGLESv2 of the GL ES compatibility layer's tests (see below): it is a library of its
# own, not a test file. It exports the classic ES entry points, and src/gl/gl46.cpp defines the
# gl46 globals under the same names, so compiling it into the test binary would be a name clash.
list(REMOVE_ITEM TEST_SOURCES ${ROOT}/tests/gles_fake.cpp)
if(TEST_SOURCES)
    add_executable(scacelith_tests ${TEST_SOURCES})
    # The shader preprocessor's tests: shader.cpp needs the GL entry points only to link (they
    # stay null, no GL library: the tests never compile a program).
    target_sources(scacelith_tests PRIVATE ${ROOT}/src/render/shader.cpp ${ROOT}/src/gl/gl46.cpp)
    # The hall ceiling's geometry tests: CPU meshes only (mesh.cpp's upload is never called).
    target_sources(scacelith_tests PRIVATE ${ROOT}/src/scene/hall_ceiling.cpp ${ROOT}/src/scene/hall_geom.cpp
                   ${ROOT}/src/scene/hall_ornament.cpp ${ROOT}/src/render/mesh.cpp)
    # The pieces' picking outlines, from the chess set's meshes (CPU only).
    target_sources(scacelith_tests PRIVATE ${ROOT}/src/scene/piece_silhouette.cpp ${ROOT}/src/scene/pieces.cpp
                   ${ROOT}/src/scene/piece_profile.cpp ${ROOT}/src/scene/sdf_mesher.cpp)
    target_compile_options(scacelith_tests PRIVATE ${COMMON_WARNINGS} ${OPT_FLAGS})
    target_link_libraries(scacelith_tests PRIVATE scacelith_core)
    # The Android touch gesture machine (src/platform/touch_input.cpp) is platform-neutral -- it
    # reads plat::TouchEvent and writes plat::Input, with no Android header in sight -- so its tests
    # (tests/touch_input_tests.cpp) run on the desktop like any other.
    target_sources(scacelith_tests PRIVATE ${ROOT}/src/platform/touch_input.cpp)
    # The GL ES 3.2 compatibility layer (src/gl/gl46_gles.cpp) is Android-only code -- the desktop
    # builds never define SCACELITH_GLES -- but what it does is platform-neutral bookkeeping, and a
    # machine that can run no device and no emulator (no /dev/kvm, x86-64 system images only) has
    # no other way to exercise it. tests/gles_tests.cpp runs it against the fake libGLESv2 that
    # tests/gles_fake.cpp builds, which the layer loads through SCACELITH_GLES_LIBRARY.
    if(NOT WIN32)
        add_library(gles_fake SHARED ${ROOT}/tests/gles_fake.cpp)
        target_compile_options(gles_fake PRIVATE ${COMMON_WARNINGS} ${OPT_FLAGS})
        target_include_directories(gles_fake SYSTEM PRIVATE ${ROOT}/third_party/khronos)
        target_sources(scacelith_tests PRIVATE ${ROOT}/src/gl/gl46_gles.cpp)
        # Only that one source file is built as the ES layer: SCACELITH_GLES also tells
        # src/render/shader.cpp which #version to emit, and the shader tests want the desktop one.
        set_property(SOURCE ${ROOT}/src/gl/gl46_gles.cpp TARGET_DIRECTORY scacelith_tests
                     PROPERTY COMPILE_DEFINITIONS SCACELITH_GLES=1)
        target_compile_definitions(scacelith_tests PRIVATE SCACELITH_GLES_TESTS=1)
        # The fake's absolute path, for tests/gles_tests.cpp to hand the layer: a generated header,
        # because a compile definition cannot carry a path without the quoting getting in the way.
        # (It goes into the generated/ directory scacelith_core already puts on the include path.)
        file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/gles_fake_path.h
             CONTENT "#define SCACELITH_GLES_FAKE_LIB \"$<TARGET_FILE:gles_fake>\"\n")
        target_link_libraries(scacelith_tests PRIVATE gles_fake)
    endif()
    if(WIN32)
        target_link_options(scacelith_tests PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
endif()

# Windows: the exception table (.pdata) of each exe is checked after its link when Python is
# available (tools/check_pdata.py). Windows and Wine binary-search it to unwind every C++ throw:
# the build fails if its entries are unsorted, overlap or point at bad unwind data, or if libgcc's
# unwinder would find another function's entry (a throw would then loop forever or crash).
# A Release scacelith.exe is linked stripped (-s): only its table is checked, while scacelith_tests,
# which keeps its symbols, also gets the per-function check.
if(WIN32 AND Python3_Interpreter_FOUND)
    foreach(exe scacelith scacelith_tests)
        if(TARGET ${exe})
            add_custom_command(TARGET ${exe} POST_BUILD
                COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/check_pdata.py --quiet $<TARGET_FILE:${exe}>
                VERBATIM)
        endif()
    endforeach()
endif()
