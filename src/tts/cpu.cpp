// Run-time choice of the kernel table (baseline code). Mirrors Stockfish's dispatcher
// (third_party/stockfish/scacelith/cpu.cpp): __builtin_cpu_supports also checks that the OS saves
// the AVX / AVX-512 register state (XCR0), and __builtin_cpu_init must run first when this is
// reached during static initialisation.
#include "kernels.h"
#include "core/log.h"
#include <atomic>
#include <cstring>

namespace tts {
namespace kern {
namespace {

std::atomic<int> g_cap{kLevelCount - 1};

const Table* builtTable(int level) {
    switch (level) {
    case kScalar: return tableScalar();
    case kSse2: return tableSse2();
    case kAvx2: return tableAvx2();
    case kAvxVnni: return tableAvxVnni();
    case kAvx512: return tableAvx512();
    default: return nullptr;
    }
}

}  // namespace

const char* levelName(int level) {
    static const char* const names[kLevelCount] = {"scalar", "sse2", "avx2", "avxvnni", "avx512"};
    return level >= 0 && level < kLevelCount ? names[level] : "?";
}

bool cpuRuns(int level) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
    __builtin_cpu_init();
    switch (level) {
    case kScalar:
    case kSse2: return true;
    case kAvx2: return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
    case kAvxVnni:
        return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") && __builtin_cpu_supports("avxvnni");
    case kAvx512:
        return __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw") &&
               __builtin_cpu_supports("avx512vl") && __builtin_cpu_supports("avx512dq") &&
               __builtin_cpu_supports("avx512vnni") && __builtin_cpu_supports("fma");
    default: return false;
    }
#else
    return level == kScalar;
#endif
}

const Table* tableFor(int level) { return cpuRuns(level) ? builtTable(level) : nullptr; }

const Table& active() {
    // Levels are not a strict chain (a CPU may have AVX-512 VNNI without AVX-VNNI or the reverse):
    // take the highest one the CPU runs, under the cap. A level this build did not compile (a cap
    // set from the settings file on an architecture whose units are not there, e.g. sse2 on
    // arm64, where only the scalar table exists) falls back to the scalar one rather than to a
    // null table: the cap is a troubleshooting knob, not a promise that the level exists.
    int cap = g_cap.load(std::memory_order_relaxed);
    for (int level = cap; level > kScalar; --level)
        if (cpuRuns(level)) {
            if (const Table* t = builtTable(level)) return *t;
        }
    if (const Table* t = builtTable(kSse2 <= cap ? kSse2 : kScalar)) return *t;
    return *builtTable(kScalar);
}

bool setArchCap(const char* arch) {
    if (!arch || !*arch || std::strcmp(arch, "auto") == 0) {
        g_cap.store(kLevelCount - 1);
        return true;
    }
    for (int level = 0; level < kLevelCount; ++level)
        if (std::strcmp(arch, levelName(level)) == 0) {
            g_cap.store(level);
            return true;
        }
    LOGW("tts: unknown arch cap '%s' (auto, scalar, sse2, avx2, avxvnni, avx512)", arch);
    return false;
}

}  // namespace kern
}  // namespace tts
