// Kernel bodies shared by the per-instruction-set translation units (kernels_*.cpp), written once
// against a small vector trait V that each unit defines for its instruction set:
//
//   R (float vector) and I (int32 vector) register types, W floats per register,
//   zero, set1, loadu, load (aligned), storeu, store, add, sub, mul, div, fmadd(a, b, c) = a*b + c,
//   max, min, hsum, cvtRound (float -> int32, nearest), cvtI2F, pow2i (2^n from int32 n, as float),
//   copySign, abs, cmpGt, select, cvtI8 / cvtU8 (W bytes to floats), sqrtScalar;
//   izero, iset1, ibcast32 (broadcast 4 bytes), iload, iloadu, istore, istoreu, iadd, storeU8
//   (W int32 in 0..255 to bytes), madd16 (pmaddwd), and dpbusd (u8 x s8 dot of 4, VNNI units only).
// SSE2 intrinsics may be used directly (every unit has them), wider ones only through V.
//
// Rules that keep instruction-set code confined to its unit (no isolate step as Stockfish has):
// this header and the AVX units include only <immintrin.h>, <cstddef> and <cstdint>; everything
// is in an anonymous namespace (internal linkage, never a merged COMDAT copy); no std:: templates
// or library calls (math is done with the polynomials below); the only external symbols of a unit
// are the tts::kern::table*() functions returning plain data. The baseline unit
// (kernels_sse2.cpp) is compiled with the normal flags and may use the standard library.
#pragma once
#include <cstddef>
#include <cstdint>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#include <immintrin.h>   // the trait of a non-x86 unit uses its own (NEON) intrinsics instead
#endif

namespace {

// ------------------------------------------------------------------------------------------------
// Scalar helpers (no <cmath>)
// ------------------------------------------------------------------------------------------------
inline float fabsScalar(float x) { return x < 0.0f ? -x : x; }

// ------------------------------------------------------------------------------------------------
// Vector math. exp: Cephes-style range reduction (x = n ln2 + r) and a degree-6 polynomial
// (relative error 1.2e-7 over [-87, 88]). erf: odd polynomial below |x| = 0.927734375 and
// 1 - exp(Q(|x|)) above, least-squares fits made for this runtime (max error 7.1e-8, 1.2 ulp,
// measured against math.erf on 300k points).
// ------------------------------------------------------------------------------------------------
template <class V>
inline typename V::R vexp(typename V::R x) {
    using R = typename V::R;
    const R hi = V::set1(88.3762626647949f), lo = V::set1(-87.3365447505531f);
    x = V::min(V::max(x, lo), hi);
    R n = V::cvtI2F(V::cvtRound(V::mul(x, V::set1(1.44269504088896341f))));
    R r = V::fmadd(n, V::set1(-0.693359375f), x);
    r = V::fmadd(n, V::set1(2.12194440e-4f), r);
    R p = V::set1(1.9875691500e-4f);
    p = V::fmadd(p, r, V::set1(1.3981999507e-3f));
    p = V::fmadd(p, r, V::set1(8.3334519073e-3f));
    p = V::fmadd(p, r, V::set1(4.1665795894e-2f));
    p = V::fmadd(p, r, V::set1(1.6666665459e-1f));
    p = V::fmadd(p, r, V::set1(5.0000001201e-1f));
    R r2 = V::mul(r, r);
    p = V::fmadd(p, r2, V::add(r, V::set1(1.0f)));
    return V::mul(p, V::pow2i(V::cvtRound(n)));
}

template <class V>
inline typename V::R verf(typename V::R a) {
    using R = typename V::R;
    R t = V::abs(a);
    R s = V::mul(a, a);
    // |a| <= 0.927734375: a + a P(a^2)
    R p = V::set1(-5.998056750e-4f);
    p = V::fmadd(p, s, V::set1(4.995838937e-3f));
    p = V::fmadd(p, s, V::set1(-2.676965344e-2f));
    p = V::fmadd(p, s, V::set1(1.128195399e-1f));
    p = V::fmadd(p, s, V::set1(-3.761251696e-1f));
    p = V::fmadd(p, s, V::set1(1.283791587e-1f));
    R small = V::fmadd(p, a, a);
    // |a| > 0.927734375: 1 - exp(Q(|a|)), |a| clamped where erf rounds to 1
    R tc = V::min(t, V::set1(3.925f));
    R r = V::set1(-2.035694541e-7f);
    r = V::fmadd(r, tc, V::set1(-1.866861141e-5f));
    r = V::fmadd(r, tc, V::set1(4.222222855e-4f));
    r = V::fmadd(r, tc, V::set1(-4.137007377e-3f));
    r = V::fmadd(r, tc, V::set1(2.506245476e-2f));
    r = V::fmadd(r, tc, V::set1(-1.082216957e-1f));
    r = V::fmadd(r, tc, V::set1(-6.333824248e-1f));
    r = V::fmadd(r, tc, V::set1(-1.129503377e+0f));
    r = V::fmadd(r, tc, V::set1(1.731901655e-4f));
    R big = V::sub(V::set1(1.0f), vexp<V>(r));
    big = V::copySign(big, a);
    return V::select(V::cmpGt(t, V::set1(0.927734375f)), big, small);
}

// ------------------------------------------------------------------------------------------------
// Element-wise loops
// ------------------------------------------------------------------------------------------------
template <class V>
void kErf(const float* x, float* y, size_t n) {
    size_t i = 0;
    for (; i + V::W <= n; i += V::W) V::storeu(y + i, verf<V>(V::loadu(x + i)));
    if (i < n) {
        alignas(64) float tx[64], ty[64];
        size_t m = n - i;
        for (size_t j = 0; j < size_t(V::W); ++j) tx[j] = j < m ? x[i + j] : 0.0f;
        V::store(ty, verf<V>(V::load(tx)));
        for (size_t j = 0; j < m; ++j) y[i + j] = ty[j];
    }
}

// Same operation order as the exported graph: Div(x, sqrt 2), Erf, Add 1, Mul x, Mul 0.5.
template <class V>
inline typename V::R vgelu(typename V::R x) {
    typename V::R e = verf<V>(V::div(x, V::set1(1.41421353816986083984375f)));
    return V::mul(V::mul(x, V::add(e, V::set1(1.0f))), V::set1(0.5f));
}

template <class V>
void kGelu(const float* x, float* y, size_t n) {
    size_t i = 0;
    for (; i + V::W <= n; i += V::W) V::storeu(y + i, vgelu<V>(V::loadu(x + i)));
    if (i < n) {
        alignas(64) float tx[64], ty[64];
        size_t m = n - i;
        for (size_t j = 0; j < size_t(V::W); ++j) tx[j] = j < m ? x[i + j] : 0.0f;
        V::store(ty, vgelu<V>(V::load(tx)));
        for (size_t j = 0; j < m; ++j) y[i + j] = ty[j];
    }
}

template <class V>
void kExp(const float* x, float* y, size_t n) {
    size_t i = 0;
    for (; i + V::W <= n; i += V::W) V::storeu(y + i, vexp<V>(V::loadu(x + i)));
    if (i < n) {
        alignas(64) float tx[64], ty[64];
        size_t m = n - i;
        for (size_t j = 0; j < size_t(V::W); ++j) tx[j] = j < m ? x[i + j] : 0.0f;
        V::store(ty, vexp<V>(V::load(tx)));
        for (size_t j = 0; j < m; ++j) y[i + j] = ty[j];
    }
}

// tanh(x) = sign(x) (1 - 2 / (exp(2|x|) + 1)); below |x| = 0.0625 the odd Taylor series avoids
// the cancellation.
template <class V>
void kTanh(const float* x, float* y, size_t n) {
    for (size_t i = 0; i < n; i += V::W) {
        alignas(64) float tx[64], ty[64];
        size_t m = n - i < size_t(V::W) ? n - i : size_t(V::W);
        for (size_t j = 0; j < size_t(V::W); ++j) tx[j] = j < m ? x[i + j] : 0.0f;
        typename V::R v = V::load(tx);
        typename V::R t = V::abs(v);
        typename V::R e = vexp<V>(V::add(t, t));
        typename V::R big = V::sub(V::set1(1.0f), V::div(V::set1(2.0f), V::add(e, V::set1(1.0f))));
        typename V::R s = V::mul(v, v);
        typename V::R p = V::fmadd(s, V::set1(-17.0f / 315.0f), V::set1(2.0f / 15.0f));
        p = V::fmadd(p, s, V::set1(-1.0f / 3.0f));
        typename V::R small = V::fmadd(V::mul(p, s), v, v);
        V::store(ty, V::select(V::cmpGt(t, V::set1(0.0625f)), V::copySign(big, v), small));
        for (size_t j = 0; j < m; ++j) y[i + j] = ty[j];
    }
}

template <class V>
void kDwconv(const float* x, const float* w, int k, int dil, float bias, float* y, int n) {
    int t = 0;
    for (; t + V::W <= n; t += V::W) {
        typename V::R acc = V::set1(bias);
        for (int j = 0; j < k; ++j) acc = V::fmadd(V::set1(w[j]), V::loadu(x + t + j * dil), acc);
        V::storeu(y + t, acc);
    }
    for (; t < n; ++t) {
        float acc = bias;
        for (int j = 0; j < k; ++j) acc += w[j] * x[t + j * dil];
        y[t] = acc;
    }
}

template <class V>
inline float vsum(const float* x, int n) {
    typename V::R acc = V::zero();
    int i = 0;
    for (; i + V::W <= n; i += V::W) acc = V::add(acc, V::loadu(x + i));
    float s = V::hsum(acc);
    for (; i < n; ++i) s += x[i];
    return s;
}

template <class V>
void kLayerNormRows(const float* x, ptrdiff_t ld, int rows, int cols, const float* g, const float* b, float eps,
                    float* y) {
    for (int r = 0; r < rows; ++r) {
        const float* xr = x + r * ld;
        float* yr = y + r * ld;
        float mean = vsum<V>(xr, cols) / float(cols);
        typename V::R vm = V::set1(mean), acc = V::zero();
        int i = 0;
        for (; i + V::W <= cols; i += V::W) {
            typename V::R d = V::sub(V::loadu(xr + i), vm);
            acc = V::fmadd(d, d, acc);
        }
        float var = V::hsum(acc);
        for (; i < cols; ++i) var += (xr[i] - mean) * (xr[i] - mean);
        var /= float(cols);
        float inv = 1.0f / V::sqrtScalar(var + eps);
        typename V::R vi = V::set1(inv);
        i = 0;
        for (; i + V::W <= cols; i += V::W) {
            typename V::R d = V::mul(V::sub(V::loadu(xr + i), vm), vi);
            typename V::R o = b ? V::fmadd(d, V::loadu(g + i), V::loadu(b + i)) : V::mul(d, V::loadu(g + i));
            V::storeu(yr + i, o);
        }
        for (; i < cols; ++i) yr[i] = (xr[i] - mean) * inv * g[i] + (b ? b[i] : 0.0f);
    }
}

template <class V>
void kSoftmaxRows(float* x, ptrdiff_t ld, int rows, int cols) {
    for (int r = 0; r < rows; ++r) {
        float* xr = x + r * ld;
        float m = xr[0];
        for (int i = 1; i < cols; ++i) m = xr[i] > m ? xr[i] : m;
        typename V::R vm = V::set1(m), acc = V::zero();
        int i = 0;
        for (; i + V::W <= cols; i += V::W) {
            typename V::R e = vexp<V>(V::sub(V::loadu(xr + i), vm));
            V::storeu(xr + i, e);
            acc = V::add(acc, e);
        }
        float s = V::hsum(acc);
        if (i < cols) {
            alignas(64) float t[V::W];
            int rem = cols - i;
            for (int j = 0; j < V::W; ++j) t[j] = j < rem ? xr[i + j] - m : -1000.0f;
            V::store(t, vexp<V>(V::load(t)));
            for (int j = 0; j < rem; ++j) {
                xr[i + j] = t[j];
                s += t[j];
            }
        }
        typename V::R inv = V::set1(1.0f / s);
        i = 0;
        for (; i + V::W <= cols; i += V::W) V::storeu(xr + i, V::mul(V::loadu(xr + i), inv));
        for (; i < cols; ++i) xr[i] *= 1.0f / s;
    }
}

// ------------------------------------------------------------------------------------------------
// f32 GEMM micro-kernel: MR rows x NV registers of W columns.
// ------------------------------------------------------------------------------------------------
template <class V, int MR, int NV>
void kSgemmTile(int kc, const float* Ap, const float* Bp, float* C, ptrdiff_t ldc, int rows, int cols,
                bool accumulate) {
    using R = typename V::R;
    constexpr int NR = NV * V::W;
    R c[MR][NV];
    for (int i = 0; i < MR; ++i)
        for (int j = 0; j < NV; ++j) c[i][j] = V::zero();
    for (int k = 0; k < kc; ++k) {
        R b[NV];
        for (int j = 0; j < NV; ++j) b[j] = V::load(Bp + k * NR + j * V::W);
        for (int i = 0; i < MR; ++i) {
            R a = V::set1(Ap[i * kc + k]);
            for (int j = 0; j < NV; ++j) c[i][j] = V::fmadd(a, b[j], c[i][j]);
        }
    }
    if (rows == MR && cols == NR) {
        for (int i = 0; i < MR; ++i)
            for (int j = 0; j < NV; ++j) {
                float* p = C + i * ldc + j * V::W;
                V::storeu(p, accumulate ? V::add(V::loadu(p), c[i][j]) : c[i][j]);
            }
        return;
    }
    alignas(64) float t[MR * NR];
    for (int i = 0; i < MR; ++i)
        for (int j = 0; j < NV; ++j) V::store(t + i * NR + j * V::W, c[i][j]);
    for (int i = 0; i < rows; ++i)
        for (int j = 0; j < cols; ++j) C[i * ldc + j] = (accumulate ? C[i * ldc + j] : 0.0f) + t[i * NR + j];
}

template <class V>
void kPackAf32(const float* A, ptrdiff_t lda, int rows, int kc, float* Ap) {
    for (int r = 0; r < rows; ++r) {
        const float* s = A + r * lda;
        float* d = Ap + r * kc;
        int k = 0;
        for (; k + V::W <= kc; k += V::W) V::storeu(d + k, V::loadu(s + k));
        for (; k < kc; ++k) d[k] = V::loadScalar(s + k);
    }
}

template <class V>
void kPackAi8(const int8_t* A, ptrdiff_t lda, int rows, int kc, const float* scale, const int32_t* zp, float* Ap) {
    for (int r = 0; r < rows; ++r) {
        const int8_t* s = A + r * lda;
        float* d = Ap + r * kc;
        float sc = scale[r];
        int z = zp ? zp[r] : 0;
        typename V::R vs = V::set1(sc), vz = V::set1(float(z));
        int k = 0;
        for (; k + V::W <= kc; k += V::W) V::storeu(d + k, V::mul(V::sub(V::cvtI8(s + k), vz), vs));
        for (; k < kc; ++k) d[k] = float(int(s[k]) - z) * sc;
    }
}

template <class V, int NR>
void kPackBf32(const float* B, ptrdiff_t ldb, int kc, int cols, float* Bp) {
    for (int k = 0; k < kc; ++k) {
        const float* s = B + k * ldb;
        float* d = Bp + k * NR;
        if (cols == NR) {
            for (int j = 0; j < NR; j += V::W) V::store(d + j, V::loadu(s + j));
        } else {
            int j = 0;
            for (; j < cols; ++j) d[j] = V::loadScalar(s + j);
            for (; j < NR; ++j) d[j] = 0.0f;
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Integer GEMM tiles store sum + rowOff[i] + colOff[j] (zero-point corrections, may be null).
// ------------------------------------------------------------------------------------------------
template <class V, int MR, int NV>
inline void storeIntTile(typename V::I (&c)[MR][NV], int32_t* C, ptrdiff_t ldc, int rows, int cols,
                         const int32_t* rowOff, const int32_t* colOff) {
    using I = typename V::I;
    constexpr int NR = NV * V::W;
    if (rows == MR && cols == NR) {
        I co[NV];
        for (int j = 0; j < NV; ++j) co[j] = colOff ? V::iloadu(colOff + j * V::W) : V::izero();
        for (int i = 0; i < MR; ++i) {
            I ro = rowOff ? V::iset1(rowOff[i]) : V::izero();
            for (int j = 0; j < NV; ++j) V::istoreu(C + i * ldc + j * V::W, V::iadd(V::iadd(c[i][j], co[j]), ro));
        }
        return;
    }
    alignas(64) int32_t t[MR * NR];
    for (int i = 0; i < MR; ++i)
        for (int j = 0; j < NV; ++j) V::istore(t + i * NR + j * V::W, c[i][j]);
    for (int i = 0; i < rows; ++i)
        for (int j = 0; j < cols; ++j)
            C[i * ldc + j] = t[i * NR + j] + (rowOff ? rowOff[i] : 0) + (colOff ? colOff[j] : 0);
}

// ------------------------------------------------------------------------------------------------
// Integer GEMM, 16-bit path (SSE2 / AVX2 / AVX-512 without VNNI): both operands widened to int16
// in the packing (the unsigned one minus its zero point, so |value| <= 255), k taken in pairs,
// pmaddwd sums two exact products into int32 (at most 2 x 255 x 128 = 65,280).
// ------------------------------------------------------------------------------------------------
inline int16_t widen(uint8_t v, bool isUnsigned, int zp) {
    return int16_t(isUnsigned ? int(v) - zp : int(int8_t(v)) - zp);
}

inline void kPackIntA16(const uint8_t* A, ptrdiff_t lda, int rows, int K, bool isUnsigned, int zp, void* Ap) {
    int kg = (K + 1) / 2;
    int16_t* d = static_cast<int16_t*>(Ap);
    for (int r = 0; r < rows; ++r) {
        const uint8_t* s = A + r * lda;
        int16_t* dr = d + r * kg * 2;
        if (isUnsigned)
            for (int k = 0; k < K; ++k) dr[k] = int16_t(int(s[k]) - zp);
        else
            for (int k = 0; k < K; ++k) dr[k] = int16_t(int(int8_t(s[k])) - zp);
        if (K & 1) dr[K] = 0;
    }
}

// Two rows of 8 bytes widened to int16 (zero point removed) and interleaved: 8 (k0, k1) pairs.
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
inline void widenPair8(const uint8_t* r0, const uint8_t* r1, bool isUnsigned, int zp, int16_t* d) {
    long long a, b;
    __builtin_memcpy(&a, r0, 8);
    __builtin_memcpy(&b, r1, 8);
    __m128i x = _mm_cvtsi64_si128(a), y = _mm_cvtsi64_si128(b);
    if (isUnsigned) {
        __m128i z = _mm_setzero_si128(), vz = _mm_set1_epi16(short(zp));
        x = _mm_sub_epi16(_mm_unpacklo_epi8(x, z), vz);
        y = _mm_sub_epi16(_mm_unpacklo_epi8(y, z), vz);
    } else {
        x = _mm_srai_epi16(_mm_unpacklo_epi8(x, x), 8);
        y = _mm_srai_epi16(_mm_unpacklo_epi8(y, y), 8);
    }
    _mm_storeu_si128(reinterpret_cast<__m128i*>(d), _mm_unpacklo_epi16(x, y));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(d + 8), _mm_unpackhi_epi16(x, y));
}
#else
inline void widenPair8(const uint8_t* r0, const uint8_t* r1, bool isUnsigned, int zp, int16_t* d) {
    for (int j = 0; j < 8; ++j) {   // non-x86 units (kernels_portable.cpp): plain loops
        d[2 * j] = widen(r0[j], isUnsigned, zp);
        d[2 * j + 1] = widen(r1[j], isUnsigned, zp);
    }
}
#endif

template <class V, int NR>
void kPackIntB16(const uint8_t* B, ptrdiff_t ldb, int K, int cols, bool isUnsigned, int zp, void* Bp) {
    int kg = (K + 1) / 2;
    int16_t* d = static_cast<int16_t*>(Bp);
    for (int g = 0; g < kg; ++g) {
        int16_t* dg = d + g * NR * 2;
        int k0 = 2 * g, k1 = 2 * g + 1;
        if constexpr (V::W > 1) {
            if (cols == NR && k1 < K) {
                for (int j = 0; j < NR; j += 8) widenPair8(B + k0 * ldb + j, B + k1 * ldb + j, isUnsigned, zp, dg + 2 * j);
                continue;
            }
        }
        for (int j = 0; j < NR; ++j) {
            dg[2 * j] = j < cols ? widen(B[k0 * ldb + j], isUnsigned, zp) : int16_t(0);
            dg[2 * j + 1] = (j < cols && k1 < K) ? widen(B[k1 * ldb + j], isUnsigned, zp) : int16_t(0);
        }
    }
}

template <class V, int MR, int NV>
void kIgemmTile16(int kg, const void* Apv, const void* Bpv, int32_t* C, ptrdiff_t ldc, int rows, int cols, bool,
                  const int32_t* rowOff, const int32_t* colOff) {
    using I = typename V::I;
    constexpr int NR = NV * V::W;
    const int32_t* Ap = static_cast<const int32_t*>(Apv);   // one word = two int16 of consecutive k
    const int32_t* Bp = static_cast<const int32_t*>(Bpv);
    I c[MR][NV];
    for (int i = 0; i < MR; ++i)
        for (int j = 0; j < NV; ++j) c[i][j] = V::izero();
    for (int g = 0; g < kg; ++g) {
        I b[NV];
        for (int j = 0; j < NV; ++j) b[j] = V::iload(Bp + g * NR + j * V::W);
        for (int i = 0; i < MR; ++i) {
            I a = V::ibcast32(Ap + i * kg + g);
            for (int j = 0; j < NV; ++j) c[i][j] = V::iadd(c[i][j], V::madd16(a, b[j]));
        }
    }
    storeIntTile<V, MR, NV>(c, C, ldc, rows, cols, rowOff, colOff);
}

// ------------------------------------------------------------------------------------------------
// Integer GEMM, VNNI path: bytes kept as they are, k taken by four, vpdpbusd (u8 x s8, exact
// int32 accumulation). Zero points are corrected by the caller with the sums of the signed side
// (rowOff / colOff).
// ------------------------------------------------------------------------------------------------
inline void kPackIntA8(const uint8_t* A, ptrdiff_t lda, int rows, int K, bool, int, void* Ap) {
    int kg = (K + 3) / 4;
    uint8_t* d = static_cast<uint8_t*>(Ap);
    for (int r = 0; r < rows; ++r) {
        const uint8_t* s = A + r * lda;
        uint8_t* dr = d + r * kg * 4;
        int k = 0;
        for (; k < K; ++k) dr[k] = s[k];
        for (; k < kg * 4; ++k) dr[k] = 0;
    }
}

// Four rows of 16 bytes interleaved: 16 groups of (k0, k1, k2, k3).
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
inline void interleave4x16(const uint8_t* r0, const uint8_t* r1, const uint8_t* r2, const uint8_t* r3, uint8_t* d) {
    __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(r0));
    __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(r1));
    __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(r2));
    __m128i e = _mm_loadu_si128(reinterpret_cast<const __m128i*>(r3));
    __m128i ab0 = _mm_unpacklo_epi8(a, b), ab1 = _mm_unpackhi_epi8(a, b);
    __m128i ce0 = _mm_unpacklo_epi8(c, e), ce1 = _mm_unpackhi_epi8(c, e);
    __m128i* o = reinterpret_cast<__m128i*>(d);
    _mm_storeu_si128(o, _mm_unpacklo_epi16(ab0, ce0));
    _mm_storeu_si128(o + 1, _mm_unpackhi_epi16(ab0, ce0));
    _mm_storeu_si128(o + 2, _mm_unpacklo_epi16(ab1, ce1));
    _mm_storeu_si128(o + 3, _mm_unpackhi_epi16(ab1, ce1));
}
#else
inline void interleave4x16(const uint8_t* r0, const uint8_t* r1, const uint8_t* r2, const uint8_t* r3, uint8_t* d) {
    for (int j = 0; j < 16; ++j) {   // non-x86 units: plain loops
        d[4 * j] = r0[j];
        d[4 * j + 1] = r1[j];
        d[4 * j + 2] = r2[j];
        d[4 * j + 3] = r3[j];
    }
}
#endif

template <class V, int NR>
void kPackIntB8(const uint8_t* B, ptrdiff_t ldb, int K, int cols, bool, int, void* Bp) {
    int kg = (K + 3) / 4;
    uint8_t* d = static_cast<uint8_t*>(Bp);
    for (int g = 0; g < kg; ++g) {
        uint8_t* dg = d + g * NR * 4;
        if (cols == NR && 4 * g + 3 < K) {
            const uint8_t* r = B + 4 * g * ldb;
            for (int j = 0; j < NR; j += 16) interleave4x16(r + j, r + ldb + j, r + 2 * ldb + j, r + 3 * ldb + j, dg + 4 * j);
            continue;
        }
        for (int j = 0; j < NR; ++j)
            for (int q = 0; q < 4; ++q) {
                int k = 4 * g + q;
                dg[4 * j + q] = (j < cols && k < K) ? B[k * ldb + j] : uint8_t(0);
            }
    }
}

template <class V, int MR, int NV>
void kIgemmTileVnni(int kg, const void* Apv, const void* Bpv, int32_t* C, ptrdiff_t ldc, int rows, int cols,
                    bool aUnsigned, const int32_t* rowOff, const int32_t* colOff) {
    using I = typename V::I;
    constexpr int NR = NV * V::W;
    const int32_t* Ap = static_cast<const int32_t*>(Apv);
    const int32_t* Bp = static_cast<const int32_t*>(Bpv);
    I c[MR][NV];
    for (int i = 0; i < MR; ++i)
        for (int j = 0; j < NV; ++j) c[i][j] = V::izero();
    if (aUnsigned) {
        for (int g = 0; g < kg; ++g) {
            I b[NV];
            for (int j = 0; j < NV; ++j) b[j] = V::iload(Bp + g * NR + j * V::W);
            for (int i = 0; i < MR; ++i) {
                I a = V::ibcast32(Ap + i * kg + g);
                for (int j = 0; j < NV; ++j) c[i][j] = V::dpbusd(c[i][j], a, b[j]);
            }
        }
    } else {
        for (int g = 0; g < kg; ++g) {
            I b[NV];
            for (int j = 0; j < NV; ++j) b[j] = V::iload(Bp + g * NR + j * V::W);
            for (int i = 0; i < MR; ++i) {
                I a = V::ibcast32(Ap + i * kg + g);
                for (int j = 0; j < NV; ++j) c[i][j] = V::dpbusd(c[i][j], b[j], a);
            }
        }
    }
    storeIntTile<V, MR, NV>(c, C, ldc, rows, cols, rowOff, colOff);
}

// ------------------------------------------------------------------------------------------------
// 8-bit quantization (the arithmetic of onnxruntime's MLAS x86 kernels). Tails go through a
// register-sized buffer so every element takes the same vector path.
// ------------------------------------------------------------------------------------------------
template <class V>
void kQuantizeU8(const float* x, size_t n, float scale, int zp, uint8_t* y) {
    using R = typename V::R;
    const R s = V::set1(scale), lo = V::set1(float(-zp)), hi = V::set1(float(255 - zp));
    const typename V::I z = V::iset1(zp);
    size_t i = 0;
    for (; i + V::W <= n; i += V::W) {
        R v = V::min(V::max(V::div(V::loadu(x + i), s), lo), hi);
        V::storeU8(y + i, V::iadd(V::cvtRound(v), z));
    }
    if (i < n) {
        alignas(64) float t[V::W];
        alignas(64) uint8_t u[V::W];
        for (int j = 0; j < V::W; ++j) t[j] = i + size_t(j) < n ? x[i + size_t(j)] : 0.0f;
        R v = V::min(V::max(V::div(V::load(t), s), lo), hi);
        V::storeU8(u, V::iadd(V::cvtRound(v), z));
        for (size_t j = 0; i + j < n; ++j) y[i + j] = u[j];
    }
}

template <class V>
void kDequantizeU8(const uint8_t* q, size_t n, float scale, int zp, float* y) {
    using R = typename V::R;
    const R s = V::set1(scale), z = V::set1(float(zp));
    size_t i = 0;
    for (; i + V::W <= n; i += V::W) V::storeu(y + i, V::mul(V::sub(V::cvtU8(q + i), z), s));
    for (; i < n; ++i) y[i] = float(int(q[i]) - zp) * scale;
}

}  // namespace
