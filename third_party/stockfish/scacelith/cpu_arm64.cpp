// Scacelith glue (not part of upstream Stockfish): the arm64 counterpart of cpu.cpp. The Android
// build compiles two instruction-set variants, armv8 (baseline AArch64 with NEON, upstream's
// ARCH=armv8) and armv8-dotprod (adds FEAT_DOTPROD, upstream's ARCH=armv8-dotprod), each isolated
// into one object by cmake/isolate_llvm.cmake (the LLVM-toolchain counterpart of the desktop's
// isolate.cmake: the NDK ships lld and llvm-objcopy, no GNU binutils). This dispatcher compiles
// once with the baseline flags, so it runs on any arm64 CPU. Each variant's isolated object
// exports only scacelith_sf_main_<tag> and the bounds of its initialiser table,
// sfinit_<tag>_start / sfinit_<tag>_end.
//
// The variants' static initialisers may use the variant's own instructions (they build the
// engine's global tables through std:: containers compiled with the variant's flags), so they do
// not run at library load: the isolation moves them into the private sfinit_<tag> section and
// this dispatcher runs those of the variant it chooses (std::call_once), as on the desktop.
// NEON and POPCNT are part of the arm64-v8a ABI, so the baseline variant runs on every arm64 CPU
// and stockfish_embedded_supported() always succeeds (every accepted cap leaves the baseline in).
//
// The runtime check for the dotprod variant is the one upstream's universal/entry_arm64.cpp
// makes: the kernel reports FEAT_DOTPROD (SDOT/UDOT) in getauxval(AT_HWCAP) as HWCAP_ASIMDDP
// ("Advanced SIMD dot product"). bionic headers of the older API levels do not name the bit,
// hence the fallback define.
//
// stockfish_embedded_limit_arch() accepts "auto" (and empty), "armv8" and "armv8-dotprod": the
// name caps the choice from above (a cap of "armv8" pins the engine to the baseline even on a
// dotprod CPU); every other name returns false, leaving the previous setting in place — a
// Scacelith.ini copied from an x86 machine ("x86-64-avx2", ...) must not disable the engine
// here; the caller logs "unknown engine.arch" and keeps the previous setting.
#include "stockfish_embedded.h"

#include <atomic>
#include <cstring>
#include <mutex>

#include <sys/auxv.h>

// The kernel's AT_HWCAP bit for FEAT_DOTPROD. Upstream's universal/entry_arm64.cpp carries the
// same fallback (it reports "no dotprod" rather than failing when the header lacks the name).
#ifndef AT_HWCAP
#define AT_HWCAP 16UL
#endif
#ifndef HWCAP_ASIMDDP
#define HWCAP_ASIMDDP (1UL << 20)
#endif

#include "sf_variants.h"  // generated: SCACELITH_SF_VARIANTS(X), the variants of this build

// What each variant's isolated object exports.
#define SCACELITH_SF_DECLARE(tag, arch, level)                                                          \
    extern "C" int scacelith_sf_main_##tag();                                                           \
    extern "C" void (*const sfinit_##tag##_start[])();                                                  \
    extern "C" void (*const sfinit_##tag##_end[])();
SCACELITH_SF_VARIANTS(SCACELITH_SF_DECLARE)

namespace {

// Every variant the build knows, from the baseline up, by Stockfish ARCH name; the index is the
// variant's level (SF_VARIANTS in the Android CMakeLists.txt, same order). A cap at level N
// allows the variants 0..N.
enum Level { ARMV8, ARMV8_DOTPROD, LEVEL_COUNT };
constexpr const char* kLevelArch[LEVEL_COUNT] = {"armv8", "armv8-dotprod"};

constexpr bool sameName(const char* a, const char* b) {
    while (*a && *a == *b) ++a, ++b;
    return *a == *b;
}

#define SCACELITH_SF_CHECK(tag, arch, level)                                                            \
    static_assert(level >= 0 && level < LEVEL_COUNT && sameName(kLevelArch[level], arch),               \
                  "the Android CMakeLists.txt and cpu_arm64.cpp disagree on the variant " arch);
SCACELITH_SF_VARIANTS(SCACELITH_SF_CHECK)

struct Variant {
    int level;
    const char* arch;
    int (*main)();
    void (*const* initBegin)();
    void (*const* initEnd)();
};
#define SCACELITH_SF_ROW(tag, arch, level) {level, arch, &scacelith_sf_main_##tag, sfinit_##tag##_start, sfinit_##tag##_end},
constexpr Variant kVariants[] = {SCACELITH_SF_VARIANTS(SCACELITH_SF_ROW)};
constexpr int kVariantCount = int(sizeof(kVariants) / sizeof(kVariants[0]));

// Whether this CPU can run the code of the variant at `level`: every extension the variant's
// compiler flags enable (the Android CMakeLists.txt, SF_ISA_<arch>). armv8 is the arm64-v8a ABI
// baseline (NEON and POPCNT are guaranteed); armv8-dotprod adds FEAT_DOTPROD, which the kernel
// reports in AT_HWCAP as HWCAP_ASIMDDP — upstream's dispatcher (universal/entry_arm64.cpp) reads
// the same bit. (No __builtin_cpu_init() here: that is an x86-only builtin.)
bool cpuRuns(int level) {
    switch (level) {
    case ARMV8: return true;
    case ARMV8_DOTPROD: return (getauxval(AT_HWCAP) & HWCAP_ASIMDDP) != 0;
    }
    return false;
}

// The best built variant this CPU runs at or below `maxLevel`, or -1. The levels are a strict
// chain on arm64 (dotprod is a strict superset of the baseline), so the topmost runnable one wins.
int best(int maxLevel) {
    for (int i = kVariantCount - 1; i >= 0; --i)
        if (kVariants[i].level <= maxLevel && cpuRuns(kVariants[i].level)) return i;
    return -1;
}

std::atomic<int> gCap{LEVEL_COUNT - 1};  // highest level allowed (engine.arch)
std::atomic<int> gChosen{-1};            // index in kVariants of the last stockfish_embedded_supported()
std::once_flag gInitialised[kVariantCount];

// A variant's static initialisers, in .init_array order (ELF, forwards) — the order the C runtime
// would have run them in.
void runInitialisers(const Variant& v) {
    for (auto p = v.initBegin; p != v.initEnd; ++p) (*p)();
}

}  // namespace

int stockfish_embedded_variant_count() { return kVariantCount; }

const char* stockfish_embedded_variant(int index) {
    return index >= 0 && index < kVariantCount ? kVariants[index].arch : nullptr;
}

bool stockfish_embedded_limit_arch(const char* arch) {
    if (!arch || !*arch || std::strcmp(arch, "auto") == 0) {
        gCap = LEVEL_COUNT - 1;
        return true;
    }
    for (int level = 0; level < LEVEL_COUNT; ++level) {
        if (std::strcmp(arch, kLevelArch[level]) == 0) {
            gCap = level;
            return true;
        }
    }
    return false;
}

bool stockfish_embedded_supported() {
    const int chosen = best(gCap);
    gChosen = chosen;
    if (chosen < 0) return false;
    std::call_once(gInitialised[chosen], [chosen] { runInitialisers(kVariants[chosen]); });
    return true;
}

const char* stockfish_embedded_arch() {
    const int chosen = gChosen;
    return chosen >= 0 ? kVariants[chosen].arch : "none";
}

const char* stockfish_embedded_best_arch() {
    const int i = best(LEVEL_COUNT - 1);
    return i >= 0 ? kVariants[i].arch : "none";
}

int stockfish_embedded_main() {
    const int chosen = gChosen;
    return chosen >= 0 ? kVariants[chosen].main() : 1;
}
