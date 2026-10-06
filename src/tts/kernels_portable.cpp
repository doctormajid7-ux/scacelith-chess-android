// Portable compute kernels: the kernel bodies of kernels_impl.h over a vector trait written in
// plain C++ (four floats per "register", every operation a fixed four-iteration loop), which
// compilers turn into NEON on arm64 (clang -O2 vectorizes these loops) and SSE on x86.
//
// The table of the non-x86 builds (Android/arm64): tableScalar() returns it there. Every build
// compiles it, as tablePortable(), so the desktop tests check it against the scalar reference and
// the x86 tables (tests/tts_tests.cpp) -- the only way to validate it on a machine that runs no
// arm64 code.
#include "kernels.h"

#include <cmath>
#include <cstring>

#include "kernels_impl.h"

// Same results as the SSE2 table on every machine: no contraction of a * b + c into an fma
// (arm64 compilers contract by default).
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

namespace {

struct VPortable {
    static constexpr int W = 4;
    struct R { float v[4]; };
    struct I { int32_t v[4]; };
    struct M { bool v[4]; };

    static R zero() { return R{{0.0f, 0.0f, 0.0f, 0.0f}}; }
    static R set1(float x) { return R{{x, x, x, x}}; }
    static R loadu(const float* p) {
        R r;
        std::memcpy(r.v, p, sizeof(r.v));
        return r;
    }
    static R load(const float* p) { return loadu(p); }
    static void storeu(float* p, R a) { std::memcpy(p, a.v, sizeof(a.v)); }
    static void store(float* p, R a) { storeu(p, a); }
    static float loadScalar(const float* p) {
        float v;
        std::memcpy(&v, p, 4);
        return v;
    }
#define VP_MAP2(name, expr)                         \
    static R name(R a, R b) {                       \
        R r;                                        \
        for (int i = 0; i < 4; ++i) r.v[i] = (expr); \
        return r;                                   \
    }
    VP_MAP2(add, a.v[i] + b.v[i])
    VP_MAP2(sub, a.v[i] - b.v[i])
    VP_MAP2(mul, a.v[i] * b.v[i])
    VP_MAP2(div, a.v[i] / b.v[i])
    VP_MAP2(max, a.v[i] > b.v[i] ? a.v[i] : b.v[i])
    VP_MAP2(min, a.v[i] < b.v[i] ? a.v[i] : b.v[i])
#undef VP_MAP2
    // Multiply then add, rounded twice like the SSE2 table: FP_CONTRACT is off in this file, so
    // the compiler emits no fused multiply-add for it.
    static R fmadd(R a, R b, R c) {
        R r;
        for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] * b.v[i] + c.v[i];
        return r;
    }
    static float hsum(R a) { return (a.v[0] + a.v[2]) + (a.v[1] + a.v[3]); }   // SSE2's pairing
    static I cvtRound(R a) {
        I r;
        for (int i = 0; i < 4; ++i) r.v[i] = int32_t(std::nearbyint(a.v[i]));   // nearest even
        return r;
    }
    static R cvtI2F(I a) {
        R r;
        for (int i = 0; i < 4; ++i) r.v[i] = float(a.v[i]);
        return r;
    }
    static R pow2i(I n) {
        R r;
        for (int i = 0; i < 4; ++i) {
            uint32_t bits = uint32_t(n.v[i] + 127) << 23;
            std::memcpy(&r.v[i], &bits, 4);
        }
        return r;
    }
    static R abs(R a) {
        R r;
        for (int i = 0; i < 4; ++i) r.v[i] = std::fabs(a.v[i]);
        return r;
    }
    static R copySign(R mag, R sgn) {
        R r;
        for (int i = 0; i < 4; ++i) r.v[i] = std::copysign(mag.v[i], sgn.v[i]);
        return r;
    }
    static M cmpGt(R a, R b) {
        M m;
        for (int i = 0; i < 4; ++i) m.v[i] = a.v[i] > b.v[i];
        return m;
    }
    static R select(M m, R a, R b) {
        R r;
        for (int i = 0; i < 4; ++i) r.v[i] = m.v[i] ? a.v[i] : b.v[i];
        return r;
    }
    static R cvtI8(const int8_t* p) {
        R r;
        for (int i = 0; i < 4; ++i) r.v[i] = float(p[i]);
        return r;
    }
    static R cvtU8(const uint8_t* p) {
        R r;
        for (int i = 0; i < 4; ++i) r.v[i] = float(p[i]);
        return r;
    }
    static float sqrtScalar(float x) { return std::sqrt(x); }

    static I izero() { return I{{0, 0, 0, 0}}; }
    static I iset1(int x) { return I{{x, x, x, x}}; }
    static I ibcast32(const int32_t* p) {
        int32_t v;
        std::memcpy(&v, p, 4);
        return iset1(v);
    }
    static I iloadu(const int32_t* p) {
        I r;
        std::memcpy(r.v, p, sizeof(r.v));
        return r;
    }
    static I iload(const int32_t* p) { return iloadu(p); }
    static void istoreu(int32_t* p, I a) { std::memcpy(p, a.v, sizeof(a.v)); }
    static void istore(int32_t* p, I a) { istoreu(p, a); }
    static I iadd(I a, I b) {
        I r;
        for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] + b.v[i];
        return r;
    }
    // pmaddwd: each 32-bit lane holds two int16; the lane's two products summed.
    static I madd16(I a, I b) {
        int16_t x[8], y[8];
        std::memcpy(x, a.v, 16);
        std::memcpy(y, b.v, 16);
        I r;
        for (int i = 0; i < 4; ++i) r.v[i] = int32_t(x[2 * i]) * y[2 * i] + int32_t(x[2 * i + 1]) * y[2 * i + 1];
        return r;
    }
    static void storeU8(uint8_t* p, I a) {
        for (int i = 0; i < 4; ++i) p[i] = uint8_t(a.v[i] < 0 ? 0 : a.v[i] > 255 ? 255 : a.v[i]);
    }
};

// 6 rows x 16 columns: 24 accumulators of four floats, the 32 NEON registers' worth.
template <int MR, int NV>
tts::kern::Table makeTable() {
    using V = VPortable;
    tts::kern::Table t;
    t.name = "portable";
    t.level = tts::kern::kScalar;
    t.mr = MR;
    t.nr = NV * V::W;
    t.sgemmTile = kSgemmTile<V, MR, NV>;
    t.packAf32 = kPackAf32<V>;
    t.packAi8 = kPackAi8<V>;
    t.packBf32 = kPackBf32<V, NV * V::W>;
    t.imr = MR;
    t.inr = NV * V::W;
    t.ik = 2;
    t.packIntA = kPackIntA16;
    t.packIntB = kPackIntB16<V, NV * V::W>;
    t.igemmTile = kIgemmTile16<V, MR, NV>;
    t.quantizeU8 = kQuantizeU8<V>;
    t.dequantizeU8 = kDequantizeU8<V>;
    t.erf = kErf<V>;
    t.gelu = kGelu<V>;
    t.exp = kExp<V>;
    t.tanh = kTanh<V>;
    t.dwconv = kDwconv<V>;
    t.layerNormRows = kLayerNormRows<V>;
    t.softmaxRows = kSoftmaxRows<V>;
    return t;
}

}  // namespace

namespace tts {
namespace kern {
const Table* tablePortable() {
    static const Table t = makeTable<6, 4>();
    return &t;
}
#if !(defined(__x86_64__) || defined(_M_X64) || defined(__i386__))
// The non-x86 builds have this table only (cpu.cpp: kScalar is the one level they run).
const Table* tableScalar() { return tablePortable(); }
const Table* tableSse2() { return nullptr; }
const Table* tableAvx2() { return nullptr; }
const Table* tableAvxVnni() { return nullptr; }
const Table* tableAvx512() { return nullptr; }
#endif
}  // namespace kern
}  // namespace tts
