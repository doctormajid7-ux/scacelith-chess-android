// Compute kernels of the TTS runtime, one table per instruction set, selected at run time.
//
// The kernel translation units are compiled with their own instruction-set flags
// (kernels_avx2.cpp: -mavx2 -mfma, kernels_avxvnni.cpp: + -mavxvnni, kernels_avx512.cpp:
// AVX-512 F/BW/VL/DQ/VNNI) and follow strict rules so none of their code can leak into the rest of
// the program (see kernels_impl.h). Everything else, including the blocking and threading of the
// GEMMs (gemm.cpp) and the choice of the table (cpu.cpp), is compiled for the baseline (SSE2).
//
// Micro-kernel contract (f32): C[rows x cols] (+)= A[rows x kc] * B[kc x cols] with A packed
// row-major (row stride kc, 'mr' rows, rows beyond 'rows' padded) and B packed as one column panel
// ('kc' groups of 'nr' floats, zero padded). Integer contract: the same with k grouped by 'ik'
// (2 for the 16-bit pmaddwd path, 4 for VNNI): A holds 'ik' consecutive k per 32-bit word,
// B holds for every k group 'nr' words of 'ik' values; int32 accumulation, exact for u8 x s8.
#pragma once
#include <cstddef>
#include <cstdint>

namespace tts {
namespace kern {

enum Level : int { kScalar = 0, kSse2 = 1, kAvx2 = 2, kAvxVnni = 3, kAvx512 = 4, kLevelCount = 5 };

struct Table {
    const char* name;
    int level;

    // f32 GEMM.
    int mr, nr;
    void (*sgemmTile)(int kc, const float* Ap, const float* Bp, float* C, ptrdiff_t ldc, int rows, int cols,
                      bool accumulate);
    // Packs 'rows' rows x kc columns of a float matrix (any alignment) into Ap (row stride kc).
    void (*packAf32)(const float* A, ptrdiff_t lda, int rows, int kc, float* Ap);
    // Same from int8 weights, dequantised per row: (q - zp[r]) * scale[r].
    void (*packAi8)(const int8_t* A, ptrdiff_t lda, int rows, int kc, const float* scale, const int32_t* zp,
                    float* Ap);
    // Packs kc rows x 'cols' (<= nr) columns of a row-major float matrix into one panel of nr.
    void (*packBf32)(const float* B, ptrdiff_t ldb, int kc, int cols, float* Bp);

    // Integer GEMM (u8 x s8 or s8 x u8, exact int32).
    int imr, inr, ik;
    // Packs rows of an 8-bit matrix into the A layout for kg = ceil(K/ik) groups: 'isUnsigned'
    // gives the element type, 'zp' is subtracted in the 16-bit layout (ignored by VNNI, which
    // corrects with the sums instead).
    void (*packIntA)(const uint8_t* A, ptrdiff_t lda, int rows, int K, bool isUnsigned, int zp, void* Ap);
    // Packs K x cols (<= inr) of an 8-bit matrix into one B panel; same conventions.
    void (*packIntB)(const uint8_t* B, ptrdiff_t ldb, int K, int cols, bool isUnsigned, int zp, void* Bp);
    // C[rows x cols] = sum over kg groups + rowOff[i] + colOff[j] (each may be null); never
    // accumulates into C.
    void (*igemmTile)(int kg, const void* Ap, const void* Bp, int32_t* C, ptrdiff_t ldc, int rows, int cols,
                      bool aUnsigned, const int32_t* rowOff, const int32_t* colOff);

    // 8-bit quantization, bit for bit as onnxruntime's MLAS kernels (x86):
    //   quantizeU8:   y = clamp(x / scale, -zp, 255 - zp) rounded to nearest even, + zp
    //   dequantizeU8: y = (q - zp) * scale
    void (*quantizeU8)(const float* x, size_t n, float scale, int zp, uint8_t* y);
    void (*dequantizeU8)(const uint8_t* q, size_t n, float scale, int zp, float* y);

    // Element-wise (n any, in == out allowed).
    void (*erf)(const float* x, float* y, size_t n);
    void (*gelu)(const float* x, float* y, size_t n);   // 0.5 x (1 + erf(x / sqrt 2))
    void (*exp)(const float* x, float* y, size_t n);
    void (*tanh)(const float* x, float* y, size_t n);
    // Depthwise 1-D convolution of one channel over an already padded input:
    // y[t] = bias + sum_j w[j] * x[t + j * dil], t in [0, n).
    void (*dwconv)(const float* x, const float* w, int k, int dil, float bias, float* y, int n);
    // Normalises each of 'rows' rows of 'cols' floats (row stride 'ld'): (x - mean) / sqrt(var + eps) * g + b.
    void (*layerNormRows)(const float* x, ptrdiff_t ld, int rows, int cols, const float* g, const float* b,
                          float eps, float* y);
    // Softmax of each row, in place.
    void (*softmaxRows)(float* x, ptrdiff_t ld, int rows, int cols);
};

// Tables compiled into this build, by level (nullptr when the level is not built).
const Table* tableScalar();
const Table* tableSse2();
const Table* tableAvx2();
const Table* tableAvxVnni();
const Table* tableAvx512();

// The portable table (kernels_portable.cpp: plain C++, vectorized by the compiler): the kScalar
// table of the non-x86 builds, and on x86 a table the tests compare with the others.
const Table* tablePortable();
// Whether this CPU (and OS) runs a level.
bool cpuRuns(int level);
// The table in use: the best level the CPU runs, capped by setArchCap(). Thread-safe after the
// first call.
const Table& active();
// Table of a given level when the CPU runs it (tests), else nullptr.
const Table* tableFor(int level);
// Caps the level for troubleshooting ("auto", "scalar", "sse2", "avx2", "avxvnni", "avx512").
// Takes effect from the next synthesis (and the constant folding of later loads). Returns false
// for an unknown name.
bool setArchCap(const char* arch);
const char* levelName(int level);

}  // namespace kern
}  // namespace tts
