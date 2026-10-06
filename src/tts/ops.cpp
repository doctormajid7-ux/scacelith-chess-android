// Operator implementations of the graph interpreter (ONNX opset 19 semantics, numpy-style
// broadcasting). Heavy operators go through the kernel table (GEMM, depthwise convolution, GELU,
// LayerNorm, Softmax); shape arithmetic and small tensors use plain loops.
#include "gemm.h"
#include "graph.h"
#include "threads.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#include <xmmintrin.h>
#endif

namespace tts {
namespace {

bool fail(std::string* e, const std::string& msg) {
    if (e) *e = msg;
    return false;
}

std::string dimsStr(const Dims& d) {
    std::string s = "[";
    for (size_t i = 0; i < d.size(); ++i) s += (i ? "," : "") + std::to_string(d[i]);
    return s + "]";
}

int64_t normAxis(int64_t axis, int64_t rank) { return axis < 0 ? axis + rank : axis; }

Dims stridesOf(const Dims& d) {
    Dims s(d.size());
    int64_t st = 1;
    for (size_t i = d.size(); i-- > 0;) {
        s[i] = st;
        st *= d[i];
    }
    return s;
}

// Output tensor that shares the input's storage (Reshape, Squeeze, Unsqueeze, Identity).
Tensor alias(const Tensor& t, const Dims& d) {
    Tensor r = t;
    r.dims = d;
    return r;
}

// Dequantised float copy of a QuantWeight (generic operators never see int8 weights otherwise),
// in the tensor's own shape: a Reshape, Squeeze or Unsqueeze of the weight keeps its flat order.
Tensor materialize(const Tensor& t) {
    if (!t.qweight || t.data) return t;
    const QuantWeight& q = *t.qweight;
    int64_t n = elementCount(q.dims);
    Tensor r = Tensor::alloc(DType::F32, t.count() == n ? t.dims : q.dims);
    float* o = r.mut<float>();
    int64_t inner = 1;
    for (size_t d = size_t(q.axis) + 1; d < q.dims.size(); ++d) inner *= q.dims[d];
    int64_t ch = q.scale.size() > 1 ? q.dims[size_t(q.axis)] : 1;
    for (int64_t i = 0; i < n; ++i) {
        size_t c = ch > 1 ? size_t((i / inner) % ch) : 0;
        int v = q.isUnsigned ? int(q.q[i]) : int(int8_t(q.q[i]));
        o[i] = float(v - q.zeroPoint[c]) * q.scale[c];
    }
    return r;
}

// Runs fn(begin, end) over [0, n) in chunks of at least 'grain' spread over the pool (the caller
// takes part); element-wise work that is not worth a thread hand-off runs inline.
void parallelRange(const ExecContext* ctx, int64_t n, int64_t grain, const std::function<void(int64_t, int64_t)>& fn) {
    int threads = ctx && ctx->pool ? ctx->pool->size() : 1;
    int64_t chunks = std::min<int64_t>(int64_t(threads) * 4, n / std::max<int64_t>(1, grain));
    if (threads <= 1 || chunks <= 1) {
        if (n > 0) fn(0, n);
        return;
    }
    ctx->pool->run(int(chunks), [&](int c) { fn(n * c / chunks, n * (c + 1) / chunks); });
}

constexpr int64_t kGrain = 16384;   // elements per chunk of parallel element-wise work

// ------------------------------------------------------------------------------------------------
// Broadcasting: collapsed output shape and per-input strides (0 on broadcast dimensions).
// ------------------------------------------------------------------------------------------------
struct Bcast {
    Dims out;
    int nin = 0;
    std::vector<int64_t> shape;
    std::vector<std::array<int64_t, 3>> st;

    bool init(const Dims* const* ins, int n, std::string* err) {
        nin = n;
        size_t rank = 0;
        for (int i = 0; i < n; ++i) rank = std::max(rank, ins[i]->size());
        out.assign(rank, 1);
        std::vector<Dims> padded(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            padded[size_t(i)].assign(rank - ins[i]->size(), 1);
            padded[size_t(i)].insert(padded[size_t(i)].end(), ins[i]->begin(), ins[i]->end());
            for (size_t d = 0; d < rank; ++d) {
                int64_t v = padded[size_t(i)][d];
                if (v == out[d] || v == 1) continue;
                if (out[d] == 1) out[d] = v;
                else return fail(err, "cannot broadcast " + dimsStr(*ins[0]) + " with " + dimsStr(*ins[i]));
            }
        }
        // Per-dimension strides, then drop size-1 dimensions and merge contiguous ones.
        std::vector<Dims> strides(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            Dims s = stridesOf(padded[size_t(i)]);
            for (size_t d = 0; d < rank; ++d)
                if (padded[size_t(i)][d] == 1) s[d] = 0;
            strides[size_t(i)] = s;
        }
        shape.clear();
        st.clear();
        for (size_t d = 0; d < rank; ++d) {
            if (out[d] == 1) continue;
            std::array<int64_t, 3> s = {0, 0, 0};
            for (int i = 0; i < n; ++i) s[size_t(i)] = strides[size_t(i)][d];
            if (!shape.empty()) {
                bool merge = true;
                for (int i = 0; i < n; ++i)
                    if (st.back()[size_t(i)] != s[size_t(i)] * out[d]) merge = false;
                if (merge) {
                    shape.back() *= out[d];
                    st.back() = s;
                    continue;
                }
            }
            shape.push_back(out[d]);
            st.push_back(s);
        }
        if (shape.empty()) {
            shape.push_back(1);
            st.push_back({0, 0, 0});
        }
        return true;
    }

    int64_t outerCount() const {
        int64_t outer = 1;
        for (size_t d = 0; d + 1 < shape.size(); ++d) outer *= shape[d];
        return outer;
    }

    // fn(count, off[3], stride[3], outOffset) for every innermost run.
    template <class F>
    void forRuns(F fn) const {
        forRunsRange(0, outerCount(), fn);
    }

    // The same for the runs [o0, o1).
    template <class F>
    void forRunsRange(int64_t o0, int64_t o1, F fn) const {
        size_t r = shape.size();
        int64_t inner = shape[r - 1];
        std::vector<int64_t> idx(r, 0);
        std::array<int64_t, 3> off = {0, 0, 0};
        int64_t rem = o0;
        for (size_t d = r - 1; d-- > 0;) {
            idx[d] = rem % shape[d];
            rem /= shape[d];
            for (int i = 0; i < nin; ++i) off[size_t(i)] += idx[d] * st[d][size_t(i)];
        }
        for (int64_t o = o0; o < o1; ++o) {
            fn(inner, off, st[r - 1], o * inner);
            for (size_t d = r - 1; d-- > 0;) {
                ++idx[d];
                for (int i = 0; i < nin; ++i) off[size_t(i)] += st[d][size_t(i)];
                if (idx[d] < shape[d]) break;
                for (int i = 0; i < nin; ++i) off[size_t(i)] -= st[d][size_t(i)] * shape[d];
                idx[d] = 0;
            }
        }
    }
};

template <class T, class R, class F>
bool binaryT(const Tensor& a, const Tensor& b, Tensor& out, DType outType, F f, std::string* err,
             const ExecContext* ctx = nullptr) {
    const Dims* ins[2] = {&a.dims, &b.dims};
    Bcast bc;
    if (!bc.init(ins, 2, err)) return false;
    out = Tensor::alloc(outType, bc.out);
    if (out.count() == 0) return true;
    const T* pa = a.as<T>();
    const T* pb = b.as<T>();
    R* po = out.mut<R>();
    auto run = [&](int64_t n, const std::array<int64_t, 3>& off, const std::array<int64_t, 3>& s, int64_t o) {
        const T* x = pa + off[0];
        const T* y = pb + off[1];
        R* z = po + o;
        if (s[0] == 1 && s[1] == 1) {
            for (int64_t i = 0; i < n; ++i) z[i] = f(x[i], y[i]);
        } else if (s[0] == 1 && s[1] == 0) {
            T yv = *y;
            for (int64_t i = 0; i < n; ++i) z[i] = f(x[i], yv);
        } else if (s[0] == 0 && s[1] == 1) {
            T xv = *x;
            for (int64_t i = 0; i < n; ++i) z[i] = f(xv, y[i]);
        } else {
            for (int64_t i = 0; i < n; ++i) z[i] = f(x[i * s[0]], y[i * s[1]]);
        }
    };
    int64_t outer = bc.outerCount(), inner = bc.shape.back();
    if (!ctx || !ctx->pool || out.count() < 2 * kGrain) {
        bc.forRuns(run);
    } else if (outer == 1) {   // one contiguous run: split it
        parallelRange(ctx, inner, kGrain, [&](int64_t i0, int64_t i1) {
            const std::array<int64_t, 3>& s = bc.st.back();
            run(i1 - i0, {i0 * s[0], i0 * s[1], 0}, s, i0);
        });
    } else {
        parallelRange(ctx, outer, std::max<int64_t>(1, kGrain / std::max<int64_t>(1, inner)),
                      [&](int64_t o0, int64_t o1) { bc.forRunsRange(o0, o1, run); });
    }
    return true;
}

bool arith(const ExecContext& ctx, Op op, const Tensor& a0, const Tensor& b0, Tensor& out, std::string* err) {
    Tensor a = materialize(a0), b = materialize(b0);
    if (a.type != b.type) return fail(err, std::string("type mismatch ") + dtypeName(a.type) + "/" + dtypeName(b.type));
    switch (a.type) {
    case DType::F32:
        switch (op) {
        case Op::Add:
            return binaryT<float, float>(a, b, out, DType::F32, [](float x, float y) { return x + y; }, err, &ctx);
        case Op::Sub:
            return binaryT<float, float>(a, b, out, DType::F32, [](float x, float y) { return x - y; }, err, &ctx);
        case Op::Mul:
            return binaryT<float, float>(a, b, out, DType::F32, [](float x, float y) { return x * y; }, err, &ctx);
        case Op::Div:
            return binaryT<float, float>(a, b, out, DType::F32, [](float x, float y) { return x / y; }, err, &ctx);
        default: break;
        }
        break;
    case DType::I64:
        switch (op) {
        case Op::Add: return binaryT<int64_t, int64_t>(a, b, out, DType::I64, [](int64_t x, int64_t y) { return x + y; }, err);
        case Op::Sub: return binaryT<int64_t, int64_t>(a, b, out, DType::I64, [](int64_t x, int64_t y) { return x - y; }, err);
        case Op::Mul: return binaryT<int64_t, int64_t>(a, b, out, DType::I64, [](int64_t x, int64_t y) { return x * y; }, err);
        case Op::Div:
            return binaryT<int64_t, int64_t>(a, b, out, DType::I64, [](int64_t x, int64_t y) { return y ? x / y : 0; }, err);
        default: break;
        }
        break;
    case DType::I32:
        switch (op) {
        case Op::Add: return binaryT<int32_t, int32_t>(a, b, out, DType::I32, [](int32_t x, int32_t y) { return x + y; }, err);
        case Op::Sub: return binaryT<int32_t, int32_t>(a, b, out, DType::I32, [](int32_t x, int32_t y) { return x - y; }, err);
        case Op::Mul: return binaryT<int32_t, int32_t>(a, b, out, DType::I32, [](int32_t x, int32_t y) { return x * y; }, err);
        case Op::Div:
            return binaryT<int32_t, int32_t>(a, b, out, DType::I32, [](int32_t x, int32_t y) { return y ? x / y : 0; }, err);
        default: break;
        }
        break;
    default: break;
    }
    return fail(err, std::string("unsupported type ") + dtypeName(a.type));
}

bool opPow(const Tensor& a, const Tensor& b, Tensor& out, std::string* err) {
    if (a.type == DType::I64) {   // shape arithmetic (relative-position attention windows)
        // By squaring: an exponent from the file never loops for long. A square is only taken when
        // a later bit needs it, so 'overflow' means the result itself does not fit.
        bool overflow = false;
        auto ipow = [&overflow](int64_t x, int64_t y) {
            if (y < 0) return int64_t(0);
            int64_t r = 1;
            for (; y; y >>= 1) {
                if ((y & 1) && __builtin_mul_overflow(r, x, &r)) overflow = true;
                if (y > 1 && __builtin_mul_overflow(x, x, &x)) overflow = true;
            }
            return r;
        };
        bool ok;
        if (b.type == DType::I64) {
            ok = binaryT<int64_t, int64_t>(a, b, out, DType::I64, ipow, err);
        } else if (b.type == DType::F32) {
            float e = b.scalarFloat();
            if (!(e >= -0x1p63f && e < 0x1p63f)) return fail(err, "Pow exponent out of range");
            ok = binaryT<int64_t, int64_t>(a, Tensor::fromInts({int64_t(e)}), out, DType::I64, ipow, err);
        } else {
            return fail(err, "Pow exponent type");
        }
        return ok && (!overflow || fail(err, "Pow overflow"));
    }
    if (a.type != DType::F32) return fail(err, "Pow base must be float or int64");
    if (b.type == DType::F32 && b.count() == 1) {
        float e = b.scalarFloat();
        if (e == 2.0f) return binaryT<float, float>(a, b, out, DType::F32, [](float x, float) { return x * x; }, err);
        if (e == 0.5f) return binaryT<float, float>(a, b, out, DType::F32, [](float x, float) { return std::sqrt(x); }, err);
    }
    if (b.type == DType::F32)
        return binaryT<float, float>(a, b, out, DType::F32, [](float x, float y) { return std::pow(x, y); }, err);
    if (b.type == DType::I64) {
        Tensor bf = Tensor::alloc(DType::F32, b.dims);
        for (int64_t i = 0; i < b.count(); ++i) bf.mut<float>()[i] = float(b.as<int64_t>()[i]);
        return binaryT<float, float>(a, bf, out, DType::F32, [](float x, float y) { return std::pow(x, y); }, err);
    }
    return fail(err, "Pow exponent type");
}

bool opEqual(const Tensor& a, const Tensor& b, Tensor& out, std::string* err) {
    if (a.type != b.type) return fail(err, "Equal type mismatch");
    switch (a.type) {
    case DType::F32: return binaryT<float, uint8_t>(a, b, out, DType::Bool, [](float x, float y) { return uint8_t(x == y); }, err);
    case DType::I64:
        return binaryT<int64_t, uint8_t>(a, b, out, DType::Bool, [](int64_t x, int64_t y) { return uint8_t(x == y); }, err);
    case DType::I32:
        return binaryT<int32_t, uint8_t>(a, b, out, DType::Bool, [](int32_t x, int32_t y) { return uint8_t(x == y); }, err);
    case DType::Bool:
        return binaryT<uint8_t, uint8_t>(a, b, out, DType::Bool, [](uint8_t x, uint8_t y) { return uint8_t(x == y); }, err);
    default: return fail(err, "Equal type");
    }
}

template <class T>
bool whereT(const Tensor& c, const Tensor& x, const Tensor& y, Tensor& out, std::string* err) {
    const Dims* ins[3] = {&c.dims, &x.dims, &y.dims};
    Bcast bc;
    if (!bc.init(ins, 3, err)) return false;
    out = Tensor::alloc(x.type, bc.out);
    if (out.count() == 0) return true;
    const uint8_t* pc = c.as<uint8_t>();
    const T* px = x.as<T>();
    const T* py = y.as<T>();
    T* po = out.mut<T>();
    bc.forRuns([&](int64_t n, const std::array<int64_t, 3>& off, const std::array<int64_t, 3>& s, int64_t o) {
        for (int64_t i = 0; i < n; ++i)
            po[o + i] = pc[off[0] + i * s[0]] ? px[off[1] + i * s[1]] : py[off[2] + i * s[2]];
    });
    return true;
}

bool opWhere(const Tensor& c, const Tensor& x0, const Tensor& y0, Tensor& out, std::string* err) {
    Tensor x = materialize(x0), y = materialize(y0);
    if (c.type != DType::Bool || x.type != y.type) return fail(err, "Where types");
    switch (x.type) {
    case DType::F32: case DType::I32: return whereT<int32_t>(c, x, y, out, err);
    case DType::I64: return whereT<int64_t>(c, x, y, out, err);
    case DType::Bool: case DType::U8: case DType::I8: return whereT<uint8_t>(c, x, y, out, err);
    default: return fail(err, "Where type");
    }
}

bool opPRelu(const Tensor& x, const Tensor& s, Tensor& out, std::string* err) {
    Tensor sl = materialize(s);
    return binaryT<float, float>(x, sl, out, DType::F32, [](float v, float a) { return v < 0.0f ? v * a : v; }, err);
}

// ------------------------------------------------------------------------------------------------
// Unary
// ------------------------------------------------------------------------------------------------
template <class F>
bool unaryF(const Tensor& x0, Tensor& out, F f, std::string* err) {
    Tensor x = materialize(x0);
    if (x.type != DType::F32) return fail(err, "expects float");
    out = Tensor::alloc(DType::F32, x.dims);
    const float* a = x.as<float>();
    float* o = out.mut<float>();
    for (int64_t i = 0, n = x.count(); i < n; ++i) o[i] = f(a[i]);
    return true;
}

template <class K>
bool unaryKernel(const ExecContext& ctx, const Tensor& x0, Tensor& out, K kfn, std::string* err) {
    Tensor x = materialize(x0);
    if (x.type != DType::F32) return fail(err, "expects float");
    out = Tensor::alloc(DType::F32, x.dims);
    const float* src = x.as<float>();
    float* dst = out.mut<float>();
    parallelRange(&ctx, x.count(), kGrain, [&](int64_t i0, int64_t i1) { kfn(src + i0, dst + i0, size_t(i1 - i0)); });
    return true;
}

bool opCast(const Tensor& x0, int to, Tensor& out, std::string* err) {
    Tensor x = materialize(x0);
    DType t;
    switch (to) {
    case onnx::kFloat: t = DType::F32; break;
    case onnx::kInt64: t = DType::I64; break;
    case onnx::kInt32: t = DType::I32; break;
    case onnx::kBool: t = DType::Bool; break;
    case onnx::kUint8: t = DType::U8; break;
    case onnx::kInt8: t = DType::I8; break;
    default: return fail(err, "Cast to type " + std::to_string(to));
    }
    out = Tensor::alloc(t, x.dims);
    int64_t n = x.count();
    auto get = [&](int64_t i) -> double {
        switch (x.type) {
        case DType::F32: return x.as<float>()[i];
        case DType::I64: return double(x.as<int64_t>()[i]);
        case DType::I32: return x.as<int32_t>()[i];
        case DType::U8: case DType::Bool: return x.as<uint8_t>()[i];
        case DType::I8: return x.as<int8_t>()[i];
        default: return 0.0;
        }
    };
    if (x.type == DType::I32 && t == DType::F32) {   // unfused MatMulInteger outputs (the shipped model fuses them)
        for (int64_t i = 0; i < n; ++i) out.mut<float>()[i] = float(x.as<int32_t>()[i]);
        return true;
    }
    for (int64_t i = 0; i < n; ++i) {
        double v = get(i);
        switch (t) {
        case DType::F32: out.mut<float>()[i] = float(v); break;
        case DType::I64:
            out.mut<int64_t>()[i] = x.type == DType::I64 ? x.as<int64_t>()[i] : int64_t(v);
            break;
        case DType::I32: out.mut<int32_t>()[i] = int32_t(v); break;
        case DType::Bool: out.mut<uint8_t>()[i] = v != 0.0; break;
        case DType::U8: out.mut<uint8_t>()[i] = uint8_t(int(v)); break;
        case DType::I8: out.mut<int8_t>()[i] = int8_t(int(v)); break;
        default: break;
        }
    }
    return true;
}

// ------------------------------------------------------------------------------------------------
// Shape and data movement
// ------------------------------------------------------------------------------------------------
bool opReshape(const Tensor& x, const Tensor& shape, bool allowZero, Tensor& out, std::string* err) {
    std::vector<int64_t> s = shape.toInts();
    int64_t known = 1;
    int infer = -1;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == 0 && !allowZero) {
            if (i >= x.dims.size()) return fail(err, "Reshape 0 beyond input rank");
            s[i] = x.dims[i];
        }
        if (s[i] == -1) {
            if (infer >= 0) return fail(err, "Reshape with two -1");
            infer = int(i);
        } else if (s[i] < 0 || __builtin_mul_overflow(known, s[i], &known)) {
            return fail(err, "Reshape to " + dimsStr(s));
        }
    }
    int64_t total = x.count();
    if (infer >= 0) s[size_t(infer)] = known ? total / known : 0;
    if (!validDims(s) || elementCount(s) != total) return fail(err, "Reshape " + dimsStr(x.dims) + " to " + dimsStr(s));
    out = alias(x, s);
    return true;
}

bool opTranspose(const Tensor& x0, std::vector<int64_t> perm, Tensor& out, std::string* err) {
    Tensor x = materialize(x0);
    size_t r = x.dims.size();
    if (perm.empty())
        for (size_t i = 0; i < r; ++i) perm.push_back(int64_t(r - 1 - i));
    if (perm.size() != r) return fail(err, "Transpose perm rank");
    std::vector<bool> seen(r, false);
    for (int64_t p : perm) {
        if (p < 0 || p >= int64_t(r) || seen[size_t(p)]) return fail(err, "Transpose perm is not a permutation");
        seen[size_t(p)] = true;
    }
    Dims od(r);
    Dims is = stridesOf(x.dims);
    Dims ps(r);   // input stride of each output dimension
    for (size_t i = 0; i < r; ++i) {
        od[i] = x.dims[size_t(perm[i])];
        ps[i] = is[size_t(perm[i])];
    }
    out = Tensor::alloc(x.type, od);
    size_t es = dtypeSize(x.type);
    int64_t n = out.count();
    if (n == 0) return true;
    const uint8_t* src = x.as<uint8_t>();
    uint8_t* dst = out.mut<uint8_t>();
    int64_t inner = r ? od[r - 1] : 1, innerStride = r ? ps[r - 1] : 1;
    int64_t outer = n / inner;
    std::vector<int64_t> idx(r, 0);
    int64_t off = 0;
    for (int64_t o = 0; o < outer; ++o) {
        uint8_t* d = dst + o * inner * int64_t(es);
        if (es == 4) {
            const float* s = reinterpret_cast<const float*>(src) + off;
            float* df = reinterpret_cast<float*>(d);
            for (int64_t i = 0; i < inner; ++i) df[i] = s[i * innerStride];
        } else {
            for (int64_t i = 0; i < inner; ++i) std::memcpy(d + i * int64_t(es), src + (off + i * innerStride) * int64_t(es), es);
        }
        for (int64_t dd = int64_t(r) - 1; dd-- > 0;) {
            ++idx[size_t(dd)];
            off += ps[size_t(dd)];
            if (idx[size_t(dd)] < od[size_t(dd)]) break;
            off -= ps[size_t(dd)] * od[size_t(dd)];
            idx[size_t(dd)] = 0;
        }
    }
    return true;
}

bool opConcat(const Tensor* const* in, size_t n, int64_t axis, Tensor& out, std::string* err) {
    std::vector<Tensor> xs;
    for (size_t i = 0; i < n; ++i) xs.push_back(materialize(*in[i]));
    const Tensor& first = xs[0];
    int64_t r = first.rank();
    axis = normAxis(axis, r);
    if (axis < 0 || axis >= r) return fail(err, "Concat axis");
    Dims od = first.dims;
    od[size_t(axis)] = 0;
    for (const Tensor& t : xs) {
        if (t.rank() != r || t.type != first.type) return fail(err, "Concat inputs differ in rank or type");
        for (int64_t d = 0; d < r; ++d)
            if (d != axis && t.dims[size_t(d)] != first.dims[size_t(d)]) return fail(err, "Concat shape mismatch");
        od[size_t(axis)] += t.dims[size_t(axis)];
    }
    out = Tensor::alloc(first.type, od);
    size_t es = dtypeSize(first.type);
    int64_t outer = 1, inner = 1;
    for (int64_t d = 0; d < axis; ++d) outer *= od[size_t(d)];
    for (int64_t d = axis + 1; d < r; ++d) inner *= od[size_t(d)];
    uint8_t* dst = out.mut<uint8_t>();
    int64_t rowOut = od[size_t(axis)] * inner * int64_t(es);
    int64_t pos = 0;
    for (const Tensor& t : xs) {
        int64_t chunk = t.dims[size_t(axis)] * inner * int64_t(es);
        for (int64_t o = 0; o < outer; ++o)
            if (chunk) std::memcpy(dst + o * rowOut + pos, t.as<uint8_t>() + o * chunk, size_t(chunk));
        pos += chunk;
    }
    return true;
}

bool opSplit(const Node& nd, const Tensor& x0, const Tensor* splitT, Tensor* out, std::string* err) {
    Tensor x = materialize(x0);
    int64_t r = x.rank();
    int64_t axis = normAxis(nd.axis, r);
    if (axis < 0 || axis >= r) return fail(err, "Split axis");
    size_t nout = nd.out.size();
    std::vector<int64_t> sizes = splitT && splitT->valid() ? splitT->toInts() : nd.ints;
    int64_t len = x.dims[size_t(axis)];
    if (nout == 0) return fail(err, "Split without outputs");
    if (sizes.empty()) {
        int64_t each = (len + int64_t(nout) - 1) / int64_t(nout);
        for (size_t i = 0; i < nout; ++i) sizes.push_back(std::min(each, len - each * int64_t(i)));
    }
    if (sizes.size() != nout) return fail(err, "Split sizes count");
    int64_t sum = 0;
    for (int64_t v : sizes) {
        if (v < 0 || v > len - sum) return fail(err, "Split sizes exceed the axis");
        sum += v;
    }
    int64_t outer = 1, inner = 1;
    for (int64_t d = 0; d < axis; ++d) outer *= x.dims[size_t(d)];
    for (int64_t d = axis + 1; d < r; ++d) inner *= x.dims[size_t(d)];
    size_t es = dtypeSize(x.type);
    int64_t rowIn = len * inner * int64_t(es);
    int64_t pos = 0;
    for (size_t i = 0; i < nout; ++i) {
        Dims od = x.dims;
        od[size_t(axis)] = sizes[i];
        out[i] = Tensor::alloc(x.type, od);
        int64_t chunk = sizes[i] * inner * int64_t(es);
        for (int64_t o = 0; o < outer; ++o)
            if (chunk) std::memcpy(out[i].mut<uint8_t>() + o * chunk, x.as<uint8_t>() + o * rowIn + pos, size_t(chunk));
        pos += chunk;
    }
    return true;
}

bool opSlice(const Tensor& x0, const Tensor& starts, const Tensor& ends, const Tensor* axesT, const Tensor* stepsT,
             Tensor& out, std::string* err) {
    Tensor x = materialize(x0);
    int64_t r = x.rank();
    std::vector<int64_t> st = starts.toInts(), en = ends.toInts();
    std::vector<int64_t> ax, sp;
    if (axesT && axesT->valid()) ax = axesT->toInts();
    else
        for (size_t i = 0; i < st.size(); ++i) ax.push_back(int64_t(i));
    if (stepsT && stepsT->valid()) sp = stepsT->toInts();
    else sp.assign(st.size(), 1);
    if (en.size() != st.size() || ax.size() != st.size() || sp.size() != st.size()) return fail(err, "Slice arity");
    std::vector<int64_t> b(size_t(r), 0), step(size_t(r), 1);
    Dims od = x.dims;
    for (size_t i = 0; i < st.size(); ++i) {
        int64_t a = normAxis(ax[i], r);
        if (a < 0 || a >= r) return fail(err, "Slice axis");
        int64_t dim = x.dims[size_t(a)], s = st[i], e = en[i], k = sp[i];
        if (k == 0) return fail(err, "Slice step 0");
        if (s < 0) s += dim;
        if (e < 0) e += dim;
        if (k > 0) {
            s = std::clamp<int64_t>(s, 0, dim);
            e = std::clamp<int64_t>(e, 0, dim);
            od[size_t(a)] = e > s ? (e - s + k - 1) / k : 0;
        } else {
            s = std::clamp<int64_t>(s, 0, dim - 1);
            e = std::clamp<int64_t>(e, -1, dim - 1);
            od[size_t(a)] = s > e ? (s - e - k - 1) / -k : 0;
        }
        b[size_t(a)] = s;
        step[size_t(a)] = k;
    }
    out = Tensor::alloc(x.type, od);
    int64_t n = out.count();
    if (n == 0) return true;
    size_t es = dtypeSize(x.type);
    Dims is = stridesOf(x.dims);
    std::vector<int64_t> idx(size_t(r), 0);
    int64_t inner = r ? od[size_t(r - 1)] : 1;
    int64_t outer = n / inner;
    const uint8_t* src = x.as<uint8_t>();
    uint8_t* dst = out.mut<uint8_t>();
    for (int64_t o = 0; o < outer; ++o) {
        int64_t off = 0;
        for (int64_t d = 0; d + 1 < r; ++d) off += (b[size_t(d)] + idx[size_t(d)] * step[size_t(d)]) * is[size_t(d)];
        int64_t last = r ? b[size_t(r - 1)] : 0, ls = r ? step[size_t(r - 1)] : 1;
        if (ls == 1) {
            std::memcpy(dst + o * inner * int64_t(es), src + (off + last) * int64_t(es), size_t(inner) * es);
        } else {
            for (int64_t i = 0; i < inner; ++i)
                std::memcpy(dst + (o * inner + i) * int64_t(es), src + (off + last + i * ls) * int64_t(es), es);
        }
        for (int64_t d = r - 1; d-- > 0;) {
            if (++idx[size_t(d)] < od[size_t(d)]) break;
            idx[size_t(d)] = 0;
        }
    }
    return true;
}

bool opGather(const Tensor& data0, const Tensor& indices, int64_t axis, Tensor& out, std::string* err) {
    Tensor data = materialize(data0);
    int64_t r = data.rank();
    axis = normAxis(axis, r);
    if (axis < 0 || axis >= r) return fail(err, "Gather axis");
    std::vector<int64_t> idx = indices.toInts();
    int64_t dim = data.dims[size_t(axis)];
    Dims od(data.dims.begin(), data.dims.begin() + axis);
    od.insert(od.end(), indices.dims.begin(), indices.dims.end());
    od.insert(od.end(), data.dims.begin() + axis + 1, data.dims.end());
    out = Tensor::alloc(data.type, od);
    int64_t outer = 1, inner = 1;
    for (int64_t d = 0; d < axis; ++d) outer *= data.dims[size_t(d)];
    for (int64_t d = axis + 1; d < r; ++d) inner *= data.dims[size_t(d)];
    size_t es = dtypeSize(data.type);
    int64_t chunk = inner * int64_t(es);
    const uint8_t* src = data.as<uint8_t>();
    uint8_t* dst = out.mut<uint8_t>();
    for (int64_t o = 0; o < outer; ++o)
        for (size_t i = 0; i < idx.size(); ++i) {
            int64_t k = idx[i] < 0 ? idx[i] + dim : idx[i];   // negative indices count from the end
            if (k < 0 || k >= dim) return fail(err, "Gather index out of range");
            std::memcpy(dst + (o * int64_t(idx.size()) + int64_t(i)) * chunk, src + (o * dim + k) * chunk, size_t(chunk));
        }
    return true;
}

bool opUnsqueeze(const Tensor& x, const Tensor& axesT, Tensor& out, std::string* err) {
    std::vector<int64_t> axes = axesT.toInts();
    int64_t r = x.rank() + int64_t(axes.size());
    for (int64_t& a : axes) a = normAxis(a, r);
    std::sort(axes.begin(), axes.end());
    Dims od;
    size_t src = 0, ai = 0;
    for (int64_t d = 0; d < r; ++d) {
        if (ai < axes.size() && axes[ai] == d) {
            od.push_back(1);
            ++ai;
        } else {
            if (src >= x.dims.size()) return fail(err, "Unsqueeze axes");
            od.push_back(x.dims[src++]);
        }
    }
    out = alias(x, od);
    return true;
}

bool opSqueeze(const Tensor& x, const Tensor* axesT, Tensor& out, std::string* err) {
    std::vector<int64_t> axes;
    if (axesT && axesT->valid()) axes = axesT->toInts();
    for (int64_t& a : axes) a = normAxis(a, x.rank());
    Dims od;
    for (int64_t d = 0; d < x.rank(); ++d) {
        bool listed = std::find(axes.begin(), axes.end(), d) != axes.end();
        if (axes.empty() ? x.dims[size_t(d)] == 1 : listed) {
            if (x.dims[size_t(d)] != 1) return fail(err, "Squeeze of a dimension != 1");
            continue;
        }
        od.push_back(x.dims[size_t(d)]);
    }
    out = alias(x, od);
    return true;
}

bool opExpand(const Tensor& x0, const Tensor& shape, Tensor& out, std::string* err) {
    Tensor x = materialize(x0);
    Dims s = shape.toInts();
    if (!validDims(s)) return fail(err, "Expand shape " + dimsStr(s));
    const Dims* ins[2] = {&x.dims, &s};
    Bcast bc;
    if (!bc.init(ins, 2, err)) return false;
    if (!validDims(bc.out)) return fail(err, "Expand shape " + dimsStr(bc.out));
    out = Tensor::alloc(x.type, bc.out);
    size_t es = dtypeSize(x.type);
    const uint8_t* src = x.as<uint8_t>();
    uint8_t* dst = out.mut<uint8_t>();
    if (out.count() == 0) return true;
    bc.forRuns([&](int64_t n, const std::array<int64_t, 3>& off, const std::array<int64_t, 3>& st, int64_t o) {
        if (st[0] == 1) {
            std::memcpy(dst + o * int64_t(es), src + off[0] * int64_t(es), size_t(n) * es);
        } else {
            for (int64_t i = 0; i < n; ++i) std::memcpy(dst + (o + i) * int64_t(es), src + (off[0] + i * st[0]) * int64_t(es), es);
        }
    });
    return true;
}

bool opTile(const Tensor& x0, const Tensor& repeats, Tensor& out, std::string* err) {
    Tensor x = materialize(x0);
    std::vector<int64_t> rep = repeats.toInts();
    if (int64_t(rep.size()) != x.rank()) return fail(err, "Tile repeats rank");
    Dims od = x.dims;
    for (size_t d = 0; d < od.size(); ++d)
        if (rep[d] < 0 || __builtin_mul_overflow(od[d], rep[d], &od[d])) return fail(err, "Tile repeats");
    if (!validDims(od)) return fail(err, "Tile output " + dimsStr(od));
    out = Tensor::alloc(x.type, od);
    int64_t n = out.count();
    size_t es = dtypeSize(x.type);
    Dims is = stridesOf(x.dims), os = stridesOf(od);
    const uint8_t* src = x.as<uint8_t>();
    uint8_t* dst = out.mut<uint8_t>();
    int64_t r = x.rank();
    int64_t inner = r ? x.dims[size_t(r - 1)] : 1;
    // Copy whole innermost input rows, then replicate.
    for (int64_t o = 0; o < n; o += inner) {
        int64_t off = 0, rem = o;
        for (int64_t d = 0; d < r; ++d) {
            int64_t c = rem / os[size_t(d)];
            rem %= os[size_t(d)];
            off += (c % x.dims[size_t(d)]) * is[size_t(d)];
        }
        std::memcpy(dst + o * int64_t(es), src + off * int64_t(es), size_t(inner) * es);
    }
    return true;
}

bool opPad(const ExecContext& ctx, const Node& nd, const Tensor& x0, const Tensor& padsT, const Tensor* valueT,
           const Tensor* axesT, Tensor& out, std::string* err) {
    Tensor x = materialize(x0);
    int64_t r = x.rank();
    std::vector<int64_t> p = padsT.toInts();
    std::vector<int64_t> axes;
    if (axesT && axesT->valid()) axes = axesT->toInts();
    else
        for (int64_t d = 0; d < r; ++d) axes.push_back(d);
    if (p.size() != 2 * axes.size()) return fail(err, "Pad pads size");
    std::vector<int64_t> lo(size_t(r), 0), hi(size_t(r), 0);
    for (size_t i = 0; i < axes.size(); ++i) {
        int64_t a = normAxis(axes[i], r);
        if (a < 0 || a >= r) return fail(err, "Pad axis");
        lo[size_t(a)] = p[i];
        hi[size_t(a)] = p[i + axes.size()];
    }
    if (r == 0) {   // a scalar: nothing to pad
        out = x;
        return true;
    }
    // Negative pads crop (the relative-position attention of the text encoders uses them).
    Dims od = x.dims;
    for (int64_t d = 0; d < r; ++d) {
        int64_t& v = od[size_t(d)];
        if (__builtin_add_overflow(v, lo[size_t(d)], &v) || __builtin_add_overflow(v, hi[size_t(d)], &v))
            return fail(err, "Pad pads");
        if (v < 0) return fail(err, "Pad crops more than the dimension");
    }
    if (!validDims(od)) return fail(err, "Pad output " + dimsStr(od));
    out = Tensor::alloc(x.type, od);
    size_t es = dtypeSize(x.type);
    bool edge = nd.mode == "edge";
    // Constant fill value (in the input type).
    uint8_t fill[8] = {0};
    if (!edge && valueT && valueT->valid() && valueT->count() > 0) std::memcpy(fill, valueT->data, dtypeSize(valueT->type));
    uint32_t fill4;
    std::memcpy(&fill4, fill, 4);
    int64_t n = out.count();
    if (n == 0) return true;
    Dims is = stridesOf(x.dims);
    int64_t inner = od[size_t(r - 1)];
    int64_t outer = n / inner;
    const uint8_t* src = x.as<uint8_t>();
    uint8_t* dst = out.mut<uint8_t>();
    int64_t lastLo = lo[size_t(r - 1)], lastDim = x.dims[size_t(r - 1)];
    // Output rows [o0, o1) (rows of the last dimension), split over the pool.
    parallelRange(&ctx, outer, std::max<int64_t>(1, kGrain / std::max<int64_t>(1, inner)), [&](int64_t o0, int64_t o1) {
        std::vector<int64_t> idx(size_t(r), 0);
        for (int64_t d = r - 1, rem = o0; d-- > 0;) {
            idx[size_t(d)] = rem % od[size_t(d)];
            rem /= od[size_t(d)];
        }
        for (int64_t o = o0; o < o1; ++o) {
            uint8_t* row = dst + o * inner * int64_t(es);
            uint32_t* row4 = reinterpret_cast<uint32_t*>(row);   // 4-byte elements: whole runs, no per-element memcpy
            bool inside = true;
            int64_t off = 0;
            for (int64_t d = 0; d + 1 < r; ++d) {
                int64_t c = idx[size_t(d)] - lo[size_t(d)];
                if (c < 0 || c >= x.dims[size_t(d)]) {
                    if (!edge) inside = false;
                    c = std::clamp<int64_t>(c, 0, x.dims[size_t(d)] - 1);
                }
                off += c * is[size_t(d)];
            }
            if (!inside || lastDim == 0) {
                if (es == 4) std::fill(row4, row4 + inner, fill4);
                else
                    for (int64_t i = 0; i < inner; ++i) std::memcpy(row + i * int64_t(es), fill, es);
            } else {
                const uint8_t* s = src + off * int64_t(es);
                int64_t i0 = std::max<int64_t>(0, lastLo), i1 = std::min<int64_t>(inner, lastLo + lastDim);
                if (es == 4) {
                    // Before the copied run c < 0, after it c >= lastDim (crops included).
                    uint32_t left = fill4, right = fill4;
                    if (edge) {
                        std::memcpy(&left, s, 4);
                        std::memcpy(&right, s + (lastDim - 1) * 4, 4);
                    }
                    int64_t a = std::min(i0, inner), b = std::max(i1, a);
                    std::fill(row4, row4 + a, left);
                    std::fill(row4 + b, row4 + inner, right);
                } else {
                    for (int64_t i = 0; i < inner; ++i) {
                        if (i >= i0 && i < i1) continue;
                        int64_t c = i - lastLo;
                        const uint8_t* v = edge ? s + std::clamp<int64_t>(c, 0, lastDim - 1) * int64_t(es) : fill;
                        std::memcpy(row + i * int64_t(es), v, es);
                    }
                }
                if (i1 > i0) std::memcpy(row + i0 * int64_t(es), s + (i0 - lastLo) * int64_t(es), size_t(i1 - i0) * es);
            }
            for (int64_t d = r - 1; d-- > 0;) {
                if (++idx[size_t(d)] < od[size_t(d)]) break;
                idx[size_t(d)] = 0;
            }
        }
    });
    return true;
}

bool opReduceSum(const Node& nd, const Tensor& x0, const Tensor* axesT, Tensor& out, std::string* err) {
    Tensor x = materialize(x0);
    int64_t r = x.rank();
    std::vector<int64_t> axes;
    if (axesT && axesT->valid()) axes = axesT->toInts();
    if (axes.empty()) {
        if (nd.i1) {
            out = x;
            return true;
        }
        for (int64_t d = 0; d < r; ++d) axes.push_back(d);
    }
    std::vector<bool> red(size_t(r), false);
    for (int64_t a : axes) {
        a = normAxis(a, r);
        if (a < 0 || a >= r) return fail(err, "ReduceSum axis");
        red[size_t(a)] = true;
    }
    Dims kd = x.dims, od;
    for (int64_t d = 0; d < r; ++d) {
        if (red[size_t(d)]) kd[size_t(d)] = 1;
        if (!red[size_t(d)] || nd.i0) od.push_back(kd[size_t(d)]);
    }
    if (x.type != DType::F32 && x.type != DType::I64) return fail(err, "ReduceSum type");
    Tensor acc = Tensor::zeros(x.type, kd);
    Dims ks = stridesOf(kd);
    int64_t n = x.count();
    std::vector<int64_t> idx(size_t(r), 0);
    for (int64_t i = 0; i < n; ++i) {
        int64_t o = 0;
        for (int64_t d = 0; d < r; ++d)
            if (!red[size_t(d)]) o += idx[size_t(d)] * ks[size_t(d)];
        if (x.type == DType::F32) acc.mut<float>()[o] += x.as<float>()[i];
        else acc.mut<int64_t>()[o] += x.as<int64_t>()[i];
        for (int64_t d = r; d-- > 0;) {
            if (++idx[size_t(d)] < x.dims[size_t(d)]) break;
            idx[size_t(d)] = 0;
        }
    }
    out = alias(acc, od);
    return true;
}

// ------------------------------------------------------------------------------------------------
// Normalisation, softmax
// ------------------------------------------------------------------------------------------------
bool opSoftmax(const ExecContext& ctx, const Tensor& x0, int64_t axis, Tensor& out, std::string* err) {
    Tensor x = materialize(x0);
    if (x.type != DType::F32 || x.rank() == 0) return fail(err, "Softmax expects a float tensor of rank 1 or more");
    axis = normAxis(axis, x.rank());
    if (axis != x.rank() - 1) return fail(err, "Softmax only on the last axis");
    out = Tensor::alloc(DType::F32, x.dims);
    std::memcpy(out.mut<float>(), x.as<float>(), x.bytes());
    int cols = int(x.dims.back());
    int rows = cols ? int(x.count() / cols) : 0;
    if (rows) ctx.k->softmaxRows(out.mut<float>(), cols, rows, cols);
    return true;
}

bool opLayerNorm(const ExecContext& ctx, const Node& nd, const Tensor& x0, const Tensor& g0, const Tensor* b0, Tensor& out,
                 std::string* err) {
    Tensor x = materialize(x0), g = materialize(g0);
    Tensor b = b0 && b0->valid() ? materialize(*b0) : Tensor();
    if (x.type != DType::F32 || g.type != DType::F32 || (b.valid() && b.type != DType::F32))
        return fail(err, "LayerNorm expects float");
    int64_t axis = normAxis(nd.axis, x.rank());
    if (axis < 0 || axis >= x.rank()) return fail(err, "LayerNorm axis");
    int64_t cols = 1;
    for (int64_t d = axis; d < x.rank(); ++d) cols *= x.dims[size_t(d)];
    if (g.count() != cols || (b.valid() && b.count() != cols)) return fail(err, "LayerNorm scale/bias size");
    out = Tensor::alloc(DType::F32, x.dims);
    int64_t rows = cols ? x.count() / cols : 0;
    ctx.k->layerNormRows(x.as<float>(), cols, int(rows), int(cols), g.as<float>(), b.valid() ? b.as<float>() : nullptr,
                         nd.f0, out.mut<float>());
    return true;
}

// LayerNorm over the channel axis of [B, C, L] without the transposes: statistics per (b, t).
bool opLayerNormChannels(const ExecContext& ctx, const Node& nd, const Tensor& x0, const Tensor& g0, const Tensor* b0,
                         Tensor& out, std::string* err) {
    Tensor x = materialize(x0), g = materialize(g0);
    Tensor b = b0 && b0->valid() ? materialize(*b0) : Tensor();
    if (x.type != DType::F32 || g.type != DType::F32 || (b.valid() && b.type != DType::F32))
        return fail(err, "LayerNormChannels expects float");
    if (x.rank() != 3 || g.count() != x.dims[1] || (b.valid() && b.count() != x.dims[1]))
        return fail(err, "LayerNormChannels shape");
    int64_t B = x.dims[0], C = x.dims[1], L = x.dims[2];
    out = Tensor::alloc(DType::F32, x.dims);
    const float* gp = g.as<float>();
    const float* bp = b.valid() ? b.as<float>() : nullptr;
    float eps = nd.f0;
    // The columns (b, t) are independent: split the time axis of every batch item into pieces.
    constexpr int64_t kCols = 32;
    const int64_t perItem = (L + kCols - 1) / kCols;
    const float* xp = x.as<float>();
    float* yp = out.mut<float>();
    parallelRange(&ctx, B * perItem, std::max<int64_t>(1, kGrain / std::max<int64_t>(1, C * kCols)),
                  [&](int64_t j0, int64_t j1) {
        float mean[kCols], var[kCols];
        for (int64_t j = j0; j < j1; ++j) {
            int64_t bi = j / perItem, t0 = (j % perItem) * kCols, n = std::min(kCols, L - t0);
            const float* xs = xp + bi * C * L + t0;
            float* ys = yp + bi * C * L + t0;
            for (int64_t t = 0; t < n; ++t) mean[t] = var[t] = 0.0f;
            for (int64_t c = 0; c < C; ++c)
                for (int64_t t = 0; t < n; ++t) mean[t] += xs[c * L + t];
            for (int64_t t = 0; t < n; ++t) mean[t] /= float(C);
            for (int64_t c = 0; c < C; ++c)
                for (int64_t t = 0; t < n; ++t) {
                    float d = xs[c * L + t] - mean[t];
                    var[t] += d * d;
                }
            for (int64_t t = 0; t < n; ++t) var[t] = 1.0f / std::sqrt(var[t] / float(C) + eps);
            for (int64_t c = 0; c < C; ++c) {
                float gc = gp[c], bc = bp ? bp[c] : 0.0f;
                for (int64_t t = 0; t < n; ++t) ys[c * L + t] = (xs[c * L + t] - mean[t]) * var[t] * gc + bc;
            }
        }
    });
    return true;
}

bool opBatchNorm(const Node& nd, const Tensor* const* in, Tensor& out, std::string* err) {
    Tensor x = materialize(*in[0]);
    Tensor sc = materialize(*in[1]), bi = materialize(*in[2]), mu = materialize(*in[3]), va = materialize(*in[4]);
    if (x.rank() < 2) return fail(err, "BatchNorm rank");
    int64_t N = x.dims[0], C = x.dims[1], inner = x.count() / std::max<int64_t>(1, N * C);
    for (const Tensor* t : {&x, &sc, &bi, &mu, &va})
        if (t->type != DType::F32 || (t != &x && t->count() != C)) return fail(err, "BatchNorm expects float, C parameters");
    out = Tensor::alloc(DType::F32, x.dims);
    for (int64_t n = 0; n < N; ++n)
        for (int64_t c = 0; c < C; ++c) {
            float a = sc.as<float>()[c] / std::sqrt(va.as<float>()[c] + nd.f0);
            float b = bi.as<float>()[c] - mu.as<float>()[c] * a;
            const float* s = x.as<float>() + (n * C + c) * inner;
            float* d = out.mut<float>() + (n * C + c) * inner;
            for (int64_t i = 0; i < inner; ++i) d[i] = s[i] * a + b;
        }
    return true;
}

// ------------------------------------------------------------------------------------------------
// Matrix products and convolution
// ------------------------------------------------------------------------------------------------
GemmA gemmA(const Tensor& w, int64_t rowLen, std::vector<float>& scales, std::vector<int32_t>& zps) {
    GemmA a;
    if (w.qweight && !w.data) {
        const QuantWeight& q = *w.qweight;
        int64_t rows = w.dims.empty() ? 1 : w.dims[0];
        scales.resize(size_t(rows));
        zps.resize(size_t(rows));
        for (int64_t r = 0; r < rows; ++r) {
            size_t c = q.scale.size() > 1 ? size_t(r) : 0;
            scales[size_t(r)] = q.scale[c];
            zps[size_t(r)] = q.zeroPoint[c];
        }
        a.i8 = reinterpret_cast<const int8_t*>(q.q);
        a.scale = scales.data();
        a.zp = zps.data();
    } else {
        a.f32 = w.as<float>();
    }
    a.ld = rowLen;
    return a;
}

bool quantizedPerRow(const Tensor& w) {
    // A QuantWeight can feed the GEMM directly when it is int8 quantized per row (axis 0) or per
    // tensor.
    if (!w.qweight || w.data) return true;
    const QuantWeight& q = *w.qweight;
    if (q.isUnsigned) return false;
    // Per-row scales only while the rows are still the quantization axis (not after a reshape).
    return q.scale.size() == 1 || (q.axis == 0 && !w.dims.empty() && !q.dims.empty() && w.dims[0] == q.dims[0]);
}

bool opMatMul(const ExecContext& ctx, const Tensor& a0, const Tensor& b0, Tensor& out, std::string* err) {
    Tensor a = materialize(a0), b = materialize(b0);
    if (a.type != DType::F32 || b.type != DType::F32) return fail(err, "MatMul expects float");
    Dims ad = a.dims, bd = b.dims;
    bool squeezeM = false, squeezeN = false;
    if (ad.size() == 1) {
        ad.insert(ad.begin(), 1);
        squeezeM = true;
    }
    if (bd.size() == 1) {
        bd.push_back(1);
        squeezeN = true;
    }
    int64_t M = ad[ad.size() - 2], K = ad.back(), N = bd.back();
    if (bd[bd.size() - 2] != K) return fail(err, "MatMul " + dimsStr(a.dims) + " x " + dimsStr(b.dims));
    Dims abatch(ad.begin(), ad.end() - 2), bbatch(bd.begin(), bd.end() - 2);
    const Dims* ins[2] = {&abatch, &bbatch};
    Bcast bc;
    if (!bc.init(ins, 2, err)) return false;
    Dims od = bc.out;
    od.push_back(M);
    od.push_back(N);
    int64_t batches = elementCount(bc.out);
    Dims outDims = od;
    if (squeezeN) outDims.pop_back();
    if (squeezeM) outDims.erase(outDims.end() - (squeezeN ? 1 : 2));
    out = Tensor::alloc(DType::F32, od);
    out.dims = outDims;
    const float* pa = a.as<float>();
    const float* pb = b.as<float>();
    float* po = out.mut<float>();
    if (batches == 0 || M == 0 || N == 0) return true;
    // B shared by every batch item with contiguous A rows: one tall GEMM.
    if (elementCount(bbatch) == 1) {
        GemmA ga;
        ga.f32 = pa;
        ga.ld = K;
        GemmB gb;
        gb.f32 = pb;
        gb.ld = N;
        if (elementCount(abatch) == batches) {
            sgemm(*ctx.k, ctx.pool, int(batches * M), int(N), int(K), ga, gb, po, N);
            return true;
        }
    }
    // Batch offsets.
    std::vector<int64_t> offA, offB;
    offA.reserve(size_t(batches));
    offB.reserve(size_t(batches));
    bc.forRuns([&](int64_t n, const std::array<int64_t, 3>& off, const std::array<int64_t, 3>& s, int64_t) {
        for (int64_t i = 0; i < n; ++i) {
            offA.push_back((off[0] + i * s[0]) * M * K);
            offB.push_back((off[1] + i * s[1]) * K * N);
        }
    });
    auto one = [&](int64_t i, ThreadPool* pool) {
        GemmA ga;
        ga.f32 = pa + offA[size_t(i)];
        ga.ld = K;
        GemmB gb;
        gb.f32 = pb + offB[size_t(i)];
        gb.ld = N;
        sgemm(*ctx.k, pool, int(M), int(N), int(K), ga, gb, po + i * M * N, N);
    };
    int threads = ctx.pool ? ctx.pool->size() : 1;
    if (threads > 1 && batches >= threads) {
        ctx.pool->run(int(batches), [&](int i) { one(i, nullptr); });
    } else {
        for (int64_t i = 0; i < batches; ++i) one(i, ctx.pool);
    }
    return true;
}

bool opGemm(const ExecContext& ctx, const Node& nd, const Tensor& a0, const Tensor& b0, const Tensor* c0, Tensor& out,
            std::string* err) {
    Tensor a = materialize(a0), b = materialize(b0);
    if (a.rank() != 2 || b.rank() != 2) return fail(err, "Gemm expects matrices");
    auto transposed = [](const Tensor& t) {
        Tensor r = Tensor::alloc(DType::F32, {t.dims[1], t.dims[0]});
        for (int64_t i = 0; i < t.dims[0]; ++i)
            for (int64_t j = 0; j < t.dims[1]; ++j) r.mut<float>()[j * t.dims[0] + i] = t.as<float>()[i * t.dims[1] + j];
        return r;
    };
    if (nd.i0) a = transposed(a);
    if (nd.i1) b = transposed(b);
    int64_t M = a.dims[0], K = a.dims[1], N = b.dims[1];
    if (b.dims[0] != K) return fail(err, "Gemm inner dimension");
    out = Tensor::alloc(DType::F32, {M, N});
    GemmA ga;
    ga.f32 = a.as<float>();
    ga.ld = K;
    GemmB gb;
    gb.f32 = b.as<float>();
    gb.ld = N;
    sgemm(*ctx.k, ctx.pool, int(M), int(N), int(K), ga, gb, out.mut<float>(), N);
    float* o = out.mut<float>();
    for (int64_t i = 0; i < M * N; ++i) o[i] *= nd.f0;
    if (c0 && c0->valid()) {
        Tensor c = materialize(*c0);
        Tensor tmp;
        if (!binaryT<float, float>(out, c, tmp, DType::F32, [&nd](float y, float cv) { return y + nd.f1 * cv; }, err))
            return false;
        if (tmp.dims != out.dims) return fail(err, "Gemm C broadcast");
        out = tmp;
    }
    return true;
}

bool opConv(const ExecContext& ctx, const Node& nd, const Tensor& x0, const Tensor& w, const Tensor* b0, Tensor& out,
            std::string* err) {
    Tensor x = materialize(x0);
    if (x.rank() != 3 || w.dims.size() != 3) return fail(err, "only 1-D Conv is supported");
    int64_t Nb = x.dims[0], Cin = x.dims[1], L = x.dims[2];
    int64_t Cout = w.dims[0], Cg = w.dims[1], k = w.dims[2];
    int64_t group = nd.i0;
    int64_t dil = nd.ints.empty() ? 1 : nd.ints[0];
    int64_t stride = nd.ints3.empty() ? 1 : nd.ints3[0];
    int64_t p0 = nd.ints2.size() >= 2 ? nd.ints2[0] : 0, p1 = nd.ints2.size() >= 2 ? nd.ints2[1] : 0;
    if (group < 1 || dil < 1 || stride < 1 || p0 < 0 || p1 < 0) return fail(err, "Conv group, dilation, stride or pads");
    if (Cg * group != Cin) return fail(err, "Conv channels " + dimsStr(x.dims) + " weight " + dimsStr(w.dims));
    int64_t Lout = (L + p0 + p1 - dil * (k - 1) - 1) / stride + 1;
    if (Lout <= 0) return fail(err, "Conv output length");
    Tensor bias = b0 && b0->valid() ? materialize(*b0) : Tensor();
    if (bias.valid() && (bias.type != DType::F32 || bias.count() != Cout)) return fail(err, "Conv bias");
    const float* bp = bias.valid() ? bias.as<float>() : nullptr;
    out = Tensor::alloc(DType::F32, {Nb, Cout, Lout});
    float* po = out.mut<float>();

    if (group == Cin && Cg == 1 && Cout == Cin) {   // depthwise
        Tensor wf = materialize(w);
        if (wf.type != DType::F32) return fail(err, "Conv weight type");
        if (reinterpret_cast<uintptr_t>(wf.data) % 4) {   // in place in the model file: read as float below
            Tensor a = Tensor::alloc(DType::F32, wf.dims);
            std::memcpy(a.mut<float>(), wf.data, wf.bytes());
            wf = a;
        }
        std::vector<float> row(size_t(L + p0 + p1));
        for (int64_t n = 0; n < Nb; ++n)
            for (int64_t c = 0; c < Cin; ++c) {
                const float* src = x.as<float>() + (n * Cin + c) * L;
                if (p0 || p1) {
                    std::fill(row.begin(), row.end(), 0.0f);
                    std::memcpy(row.data() + p0, src, size_t(L) * 4);
                    src = row.data();
                }
                float* dst = po + (n * Cout + c) * Lout;
                const float* wc = wf.as<float>() + c * k;
                float bc = bp ? bp[c] : 0.0f;
                if (stride == 1) {
                    ctx.k->dwconv(src, wc, int(k), int(dil), bc, dst, int(Lout));
                } else {
                    for (int64_t t = 0; t < Lout; ++t) {
                        float acc = bc;
                        for (int64_t j = 0; j < k; ++j) acc += wc[j] * src[t * stride + j * dil];
                        dst[t] = acc;
                    }
                }
            }
        return true;
    }
    if (group != 1) return fail(err, "grouped Conv not supported");
    if (!quantizedPerRow(w)) return fail(err, "Conv weight quantization layout");
    std::vector<float> scales;
    std::vector<int32_t> zps;
    GemmA ga = gemmA(w, Cin * k, scales, zps);
    if (k == 1 && stride == 1 && p0 == 0 && p1 == 0) {
        GemmB gb;
        gb.f32 = x.as<float>();
        gb.ld = L;
        if (Nb == 1) {
            sgemm(*ctx.k, ctx.pool, int(Cout), int(L), int(Cin), ga, gb, po, L);
        } else {
            // All batch items in one GEMM (the vector estimator runs at batch 2 for guidance), each
            // item's output written in place.
            gb.blocks = int(Nb);
            gb.blockCols = int(L);
            gb.blockStride = Cin * L;
            sgemm(*ctx.k, ctx.pool, int(Cout), int(Nb * L), int(Cin), ga, gb, po, L, Cout * L);
        }
    } else {
        // im2col: rows (ci, j), columns t.
        Buffer col(size_t(Cin * k) * size_t(Lout) * 4);
        float* cp = static_cast<float*>(col.data);
        for (int64_t n = 0; n < Nb; ++n) {
            const float* xs = x.as<float>() + n * Cin * L;
            for (int64_t ci = 0; ci < Cin; ++ci)
                for (int64_t j = 0; j < k; ++j) {
                    float* dst = cp + (ci * k + j) * Lout;
                    for (int64_t t = 0; t < Lout; ++t) {
                        int64_t s = t * stride + j * dil - p0;
                        dst[t] = (s >= 0 && s < L) ? xs[ci * L + s] : 0.0f;
                    }
                }
            GemmB gb;
            gb.f32 = cp;
            gb.ld = Lout;
            sgemm(*ctx.k, ctx.pool, int(Cout), int(Lout), int(Cin * k), ga, gb, po + n * Cout * Lout, Lout);
        }
    }
    if (bp)
        for (int64_t n = 0; n < Nb; ++n)
            for (int64_t co = 0; co < Cout; ++co) {
                float* d = po + (n * Cout + co) * Lout;
                float bv = bp[co];
                for (int64_t t = 0; t < Lout; ++t) d[t] += bv;
            }
    return true;
}

// MatMulInteger (u8 activations x s8 weights), optionally with the float epilogue of the
// dynamic-quantization pattern: out = float(acc) * scale [+ bias]. 'bSums': the column sums of a
// constant B (Node::bSums), or empty.
bool opMatMulInteger(const ExecContext& ctx, const Tensor* const* in, size_t nin, bool scaled,
                     const std::vector<int32_t>& bSums, Tensor& out, std::string* err) {
    const Tensor& a = *in[0];
    const Tensor& b = *in[1];
    if (b.rank() != 2 || a.rank() < 2) return fail(err, "MatMulInteger expects [..,M,K] x [K,N]");
    if (b.qweight && !b.data) return fail(err, "MatMulInteger on a dequantized weight");
    if ((a.type != DType::U8 && a.type != DType::I8) || (b.type != DType::U8 && b.type != DType::I8))
        return fail(err, "MatMulInteger expects 8-bit operands");
    if ((nin > 2 && in[2] && in[2]->valid() && in[2]->count() != 1) || (nin > 3 && in[3] && in[3]->valid() && in[3]->count() != 1))
        return fail(err, "per-row or per-column zero point not supported");
    bool aU = a.type == DType::U8, bU = b.type == DType::U8;
    int azp = (nin > 2 && in[2] && in[2]->valid()) ? int(in[2]->scalarFloat()) : 0;
    int bzp = (nin > 3 && in[3] && in[3]->valid()) ? int(in[3]->scalarFloat()) : 0;
    int64_t K = a.dims.back(), N = b.dims[1];
    if (b.dims[0] != K) return fail(err, "MatMulInteger inner dimension");
    int64_t M = a.count() / std::max<int64_t>(1, K);
    Dims od = a.dims;
    od.back() = N;
    Tensor acc = Tensor::alloc(DType::I32, od);
    const int32_t* sums = aU && b.type == DType::I8 && bSums.size() == size_t(N) ? bSums.data() : nullptr;
    if (!igemm(*ctx.k, ctx.pool, int(M), int(N), int(K), a.as<uint8_t>(), K, aU, azp, b.as<uint8_t>(), N, bU, bzp,
               acc.mut<int32_t>(), N, sums))
        return fail(err, "MatMulInteger operand types or zero points not supported");
    if (!scaled) {
        out = acc;
        return true;
    }
    float s = in[4] ? in[4]->scalarFloat() : 1.0f;
    const float* bias = (nin > 5 && in[5] && in[5]->valid()) ? in[5]->as<float>() : nullptr;
    if (in[4] && in[4]->count() != 1) return fail(err, "per-channel output scale not supported");
    if (bias && (in[5]->type != DType::F32 || in[5]->count() != N)) return fail(err, "MatMulInteger bias size");
    out = Tensor::alloc(DType::F32, od);
    const int32_t* pa = acc.as<int32_t>();
    float* po = out.mut<float>();
    parallelRange(&ctx, M, std::max<int64_t>(1, kGrain / std::max<int64_t>(1, N)), [&](int64_t i0, int64_t i1) {
        for (int64_t i = i0; i < i1; ++i)
            for (int64_t j = 0; j < N; ++j) {
                float v = float(pa[i * N + j]) * s;
                po[i * N + j] = bias ? bias[j] + v : v;
            }
    });
    return true;
}

// ------------------------------------------------------------------------------------------------
// Quantization
// ------------------------------------------------------------------------------------------------
inline float roundEven(float x) { return std::nearbyint(x); }

bool opDynamicQuantize(const ExecContext& ctx, const Tensor& x0, Tensor* out, std::string* err) {
    Tensor x = materialize(x0);
    if (x.type != DType::F32) return fail(err, "DynamicQuantizeLinear expects float");
    const float* p = x.as<float>();
    int64_t n = x.count();
    float mn = 0.0f, mx = 0.0f;
#if defined(__SSE2__) || defined(_M_X64) || defined(__x86_64__)
    // minps/maxps(x, acc) select exactly as std::min/max(acc, x) do: a NaN never replaces acc and
    // -0 never replaces the +0 start, so lanes reduced in any order give the scalar loop's bits.
    __m128 mn0 = _mm_setzero_ps(), mn1 = mn0, mx0 = mn0, mx1 = mn0;
    int64_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m128 a = _mm_loadu_ps(p + i), b = _mm_loadu_ps(p + i + 4);
        mn0 = _mm_min_ps(a, mn0);
        mn1 = _mm_min_ps(b, mn1);
        mx0 = _mm_max_ps(a, mx0);
        mx1 = _mm_max_ps(b, mx1);
    }
    alignas(16) float lanes[8];
    _mm_store_ps(lanes, _mm_min_ps(mn0, mn1));
    _mm_store_ps(lanes + 4, _mm_max_ps(mx0, mx1));
    for (int l = 0; l < 4; ++l) {
        mn = std::min(mn, lanes[l]);
        mx = std::max(mx, lanes[4 + l]);
    }
    for (; i < n; ++i) {
        mn = std::min(mn, p[i]);
        mx = std::max(mx, p[i]);
    }
#else
    // No SSE2 (aarch64): the same reduction scalar, which the compiler vectorises on NEON.
    for (int64_t i = 0; i < n; ++i) {
        mn = std::min(mn, p[i]);
        mx = std::max(mx, p[i]);
    }
#endif
    // An infinite bound (only a damaged model has one) gives no zero point: -inf makes it NaN,
    // which the cast to int below cannot take. The node fails, the line goes to subtitles.
    if (!std::isfinite(mn) || !std::isfinite(mx)) return fail(err, "DynamicQuantizeLinear: non-finite input range");
    // onnxruntime GetQuantizationParameter: scale 1 when the range is empty.
    float scale = mx == mn ? 1.0f : (mx - mn) / 255.0f;
    float zpf = std::clamp(0.0f - mn / scale, 0.0f, 255.0f);
    int zp = int(roundEven(zpf));
    out[0] = Tensor::alloc(DType::U8, x.dims);
    uint8_t* q = out[0].mut<uint8_t>();
    parallelRange(&ctx, n, kGrain, [&](int64_t i0, int64_t i1) { ctx.k->quantizeU8(p + i0, size_t(i1 - i0), scale, zp, q + i0); });
    out[1] = Tensor::scalarF32(scale);
    out[2] = Tensor::alloc(DType::U8, {});
    *out[2].mut<uint8_t>() = uint8_t(zp);
    return true;
}

// Per-tensor or per-axis parameters of Quantize/DequantizeLinear.
struct QParams {
    std::vector<float> scale;
    std::vector<int32_t> zp;
    int64_t inner = 1, channels = 1;
};

bool qparams(const Node& nd, const Tensor& x, const Tensor& s0, const Tensor* z, QParams& q, std::string* err) {
    Tensor s = materialize(s0);
    int64_t n = s.count();
    if (s.type != DType::F32 || n < 1) return fail(err, "quantization scale must be float");
    q.scale.resize(size_t(n));
    std::memcpy(q.scale.data(), s.data, size_t(n) * 4);
    q.zp.assign(size_t(n), 0);
    if (z && z->valid()) {
        if (z->count() != n) return fail(err, "zero point count");
        for (int64_t i = 0; i < n; ++i) {
            switch (z->type) {
            case DType::U8: q.zp[size_t(i)] = z->as<uint8_t>()[i]; break;
            case DType::I8: q.zp[size_t(i)] = z->as<int8_t>()[i]; break;
            case DType::I32: q.zp[size_t(i)] = z->as<int32_t>()[i]; break;
            default: return fail(err, "zero point type");
            }
        }
    }
    if (n > 1) {
        int64_t axis = normAxis(nd.axis, x.rank());
        if (axis < 0 || axis >= x.rank() || x.dims[size_t(axis)] != n) return fail(err, "per-axis scale size");
        q.channels = n;
        for (int64_t d = axis + 1; d < x.rank(); ++d) q.inner *= x.dims[size_t(d)];
    }
    return true;
}

bool opQuantize(const ExecContext& ctx, const Node& nd, const Tensor* const* in, size_t nin, Tensor& out,
                std::string* err) {
    Tensor x = materialize(*in[0]);
    const Tensor* z = nin > 2 ? in[2] : nullptr;
    QParams q;
    if (!qparams(nd, x, *in[1], z, q, err)) return false;
    DType t = z && z->valid() ? z->type : DType::U8;
    if (t != DType::U8 && t != DType::I8) return fail(err, "QuantizeLinear output type");
    if (x.type != DType::F32) return fail(err, "QuantizeLinear expects float");
    float lo = t == DType::U8 ? 0.0f : -128.0f, hi = t == DType::U8 ? 255.0f : 127.0f;
    out = Tensor::alloc(t, x.dims);
    const float* p = x.as<float>();
    int64_t n = x.count();
    if (t == DType::U8 && q.channels == 1) {   // per tensor (unfused QDQ)
        uint8_t* o = out.mut<uint8_t>();
        parallelRange(&ctx, n, kGrain, [&](int64_t i0, int64_t i1) {
            ctx.k->quantizeU8(p + i0, size_t(i1 - i0), q.scale[0], q.zp[0], o + i0);
        });
        return true;
    }
    for (int64_t i = 0; i < n; ++i) {
        size_t c = q.channels > 1 ? size_t((i / q.inner) % q.channels) : 0;
        float v = std::clamp(roundEven(p[i] / q.scale[c]) + float(q.zp[c]), lo, hi);
        if (t == DType::U8) out.mut<uint8_t>()[i] = uint8_t(v);
        else out.mut<int8_t>()[i] = int8_t(v);
    }
    return true;
}

bool opDequantize(const ExecContext& ctx, const Node& nd, const Tensor* const* in, size_t nin, Tensor& out,
                  std::string* err) {
    const Tensor& x = *in[0];
    const Tensor* z = nin > 2 ? in[2] : nullptr;
    QParams q;
    if (!qparams(nd, x, *in[1], z, q, err)) return false;
    out = Tensor::alloc(DType::F32, x.dims);
    int64_t n = x.count();
    float* o = out.mut<float>();
    if (x.type == DType::U8 && q.channels == 1) {
        const uint8_t* src = x.as<uint8_t>();
        parallelRange(&ctx, n, kGrain, [&](int64_t i0, int64_t i1) {
            ctx.k->dequantizeU8(src + i0, size_t(i1 - i0), q.scale[0], q.zp[0], o + i0);
        });
        return true;
    }
    for (int64_t i = 0; i < n; ++i) {
        size_t c = q.channels > 1 ? size_t((i / q.inner) % q.channels) : 0;
        int32_t v;
        switch (x.type) {
        case DType::U8: v = x.as<uint8_t>()[i]; break;
        case DType::I8: v = x.as<int8_t>()[i]; break;
        case DType::I32: v = x.as<int32_t>()[i]; break;
        default: return fail(err, "DequantizeLinear input type");
        }
        o[i] = float(v - q.zp[c]) * q.scale[c];
    }
    return true;
}

// DequantizeLinear(QuantizeLinear(x)) with one per-tensor uint8 scale and zero point: the same
// arithmetic as the two operators, float to float, in pieces that stay in the first-level cache
// instead of an 8-bit tensor in memory.
bool opQuantDequant(const ExecContext& ctx, const Tensor& x0, const Tensor& s, const Tensor* z, Tensor& out,
                    std::string* err) {
    Tensor x = materialize(x0);
    if (x.type != DType::F32 || s.count() != 1 || (z && (z->type != DType::U8 || z->count() != 1)))
        return fail(err, "QuantDequant expects float and a per-tensor uint8 quantization");
    float scale = s.scalarFloat();
    int zp = z ? int(*z->as<uint8_t>()) : 0;
    out = Tensor::alloc(DType::F32, x.dims);
    const float* p = x.as<float>();
    float* o = out.mut<float>();
    parallelRange(&ctx, x.count(), kGrain, [&](int64_t i0, int64_t i1) {
        alignas(64) uint8_t q[1024];
        for (int64_t i = i0; i < i1; i += 1024) {
            size_t m = size_t(std::min<int64_t>(1024, i1 - i));
            ctx.k->quantizeU8(p + i, m, scale, zp, q);
            ctx.k->dequantizeU8(q, m, scale, zp, o + i);
        }
    });
    return true;
}

}  // namespace

bool execNode(const Node& n, const Tensor* const* in, Tensor* out, const ExecContext& ctx, std::string* error) {
    size_t nin = n.in.size();
    auto need = [&](size_t count) {
        if (nin < count) return fail(error, "missing inputs");
        for (size_t i = 0; i < count; ++i)
            if (!in[i] || !in[i]->valid()) return fail(error, "input " + std::to_string(i) + " not available");
        return true;
    };
    auto opt = [&](size_t i) -> const Tensor* { return i < nin && in[i] && in[i]->valid() ? in[i] : nullptr; };
    const kern::Table& k = *ctx.k;
    switch (n.op) {
    case Op::Add: case Op::Sub: case Op::Mul: case Op::Div:
        return need(2) && arith(ctx, n.op, *in[0], *in[1], out[0], error);
    case Op::Pow: return need(2) && opPow(materialize(*in[0]), materialize(*in[1]), out[0], error);
    case Op::Equal: return need(2) && opEqual(materialize(*in[0]), materialize(*in[1]), out[0], error);
    case Op::Where: return need(3) && opWhere(*in[0], *in[1], *in[2], out[0], error);
    case Op::Cast: return need(1) && opCast(*in[0], int(n.i0), out[0], error);
    case Op::Clip: {
        if (!need(1)) return false;
        float lo = opt(1) ? opt(1)->scalarFloat() : -std::numeric_limits<float>::infinity();
        float hi = opt(2) ? opt(2)->scalarFloat() : std::numeric_limits<float>::infinity();
        if (in[0]->type == DType::I64) {
            if ((opt(1) && opt(1)->count() < 1) || (opt(2) && opt(2)->count() < 1)) return fail(error, "Clip bounds");
            int64_t ilo = opt(1) ? opt(1)->toInts()[0] : std::numeric_limits<int64_t>::min();
            int64_t ihi = opt(2) ? opt(2)->toInts()[0] : std::numeric_limits<int64_t>::max();
            Tensor x = *in[0];
            out[0] = Tensor::alloc(DType::I64, x.dims);
            for (int64_t i = 0; i < x.count(); ++i)
                out[0].mut<int64_t>()[i] = std::min(std::max(x.as<int64_t>()[i], ilo), ihi);
            return true;
        }
        return unaryF(*in[0], out[0], [lo, hi](float v) { return std::min(std::max(v, lo), hi); }, error);
    }
    case Op::Relu: return need(1) && unaryF(*in[0], out[0], [](float v) { return v > 0.0f ? v : 0.0f; }, error);
    case Op::PRelu: return need(2) && opPRelu(materialize(*in[0]), *in[1], out[0], error);
    case Op::Erf: return need(1) && unaryKernel(ctx, *in[0], out[0], k.erf, error);
    case Op::Exp: return need(1) && unaryKernel(ctx, *in[0], out[0], k.exp, error);
    case Op::Tanh: return need(1) && unaryKernel(ctx, *in[0], out[0], k.tanh, error);
    case Op::Gelu: return need(1) && unaryKernel(ctx, *in[0], out[0], k.gelu, error);
    case Op::Sin: return need(1) && unaryF(*in[0], out[0], [](float v) { return std::sin(v); }, error);
    case Op::Cos: return need(1) && unaryF(*in[0], out[0], [](float v) { return std::cos(v); }, error);
    case Op::Softplus:
        return need(1) && unaryF(*in[0], out[0], [](float v) {
                   return v > 0.0f ? v + std::log1p(std::exp(-v)) : std::log1p(std::exp(v));
               }, error);
    case Op::Reciprocal: return need(1) && unaryF(*in[0], out[0], [](float v) { return 1.0f / v; }, error);
    case Op::Concat: {
        for (size_t i = 0; i < nin; ++i)
            if (!in[i] || !in[i]->valid()) return fail(error, "Concat input missing");
        return opConcat(in, nin, n.axis, out[0], error);
    }
    case Op::Split: return need(1) && opSplit(n, *in[0], opt(1), out, error);
    case Op::Slice: return need(3) && opSlice(*in[0], *in[1], *in[2], opt(3), opt(4), out[0], error);
    case Op::Gather: return need(2) && opGather(*in[0], *in[1], n.axis, out[0], error);
    case Op::Reshape: return need(2) && opReshape(*in[0], *in[1], n.i0 != 0, out[0], error);
    case Op::Transpose: return need(1) && opTranspose(*in[0], n.ints, out[0], error);
    case Op::Unsqueeze: return need(2) && opUnsqueeze(*in[0], *in[1], out[0], error);
    case Op::Squeeze: return need(1) && opSqueeze(*in[0], opt(1), out[0], error);
    case Op::Identity: return need(1) && (out[0] = *in[0], true);
    case Op::Shape: {
        if (!need(1)) return false;
        int64_t r = in[0]->rank();
        int64_t s = std::clamp<int64_t>(normAxis(n.i0, r), 0, r), e = std::clamp<int64_t>(n.i1 < 0 ? n.i1 + r : n.i1, 0, r);
        std::vector<int64_t> v;
        for (int64_t d = s; d < e; ++d) v.push_back(in[0]->dims[size_t(d)]);
        out[0] = Tensor::fromInts(v);
        return true;
    }
    case Op::ConstantOfShape: {
        if (!need(1)) return false;
        Dims d = in[0]->toInts();
        if (!validDims(d) || n.value.count() < 1) return fail(error, "ConstantOfShape shape or value");
        out[0] = Tensor::alloc(n.value.type, d);
        size_t es = dtypeSize(n.value.type);
        for (int64_t i = 0; i < out[0].count(); ++i) std::memcpy(out[0].mut<uint8_t>() + i * int64_t(es), n.value.data, es);
        return true;
    }
    case Op::Expand: return need(2) && opExpand(*in[0], *in[1], out[0], error);
    case Op::Tile: return need(2) && opTile(*in[0], *in[1], out[0], error);
    case Op::Pad: return need(2) && opPad(ctx, n, *in[0], *in[1], opt(2), opt(3), out[0], error);
    case Op::Softmax: return need(1) && opSoftmax(ctx, *in[0], n.axis, out[0], error);
    case Op::LayerNorm: return need(2) && opLayerNorm(ctx, n, *in[0], *in[1], opt(2), out[0], error);
    case Op::LayerNormChannels: return need(2) && opLayerNormChannels(ctx, n, *in[0], *in[1], opt(2), out[0], error);
    case Op::BatchNorm: return need(5) && opBatchNorm(n, in, out[0], error);
    case Op::ReduceSum: return need(1) && opReduceSum(n, *in[0], opt(1), out[0], error);
    case Op::MatMul: return need(2) && opMatMul(ctx, *in[0], *in[1], out[0], error);
    case Op::Gemm: return need(2) && opGemm(ctx, n, *in[0], *in[1], opt(2), out[0], error);
    case Op::Conv: return need(2) && opConv(ctx, n, *in[0], *in[1], opt(2), out[0], error);
    case Op::MatMulInteger: return need(2) && opMatMulInteger(ctx, in, nin, false, n.bSums, out[0], error);
    case Op::MatMulIntegerScaled: return need(2) && opMatMulInteger(ctx, in, nin, true, n.bSums, out[0], error);
    case Op::DynamicQuantize: return need(1) && opDynamicQuantize(ctx, *in[0], out, error);
    case Op::Quantize: return need(2) && opQuantize(ctx, n, in, nin, out[0], error);
    case Op::Dequantize:
        if (!need(2)) return false;
        if (in[0]->qweight && !in[0]->data) return fail(error, "DequantizeLinear of a quantized weight");
        return opDequantize(ctx, n, in, nin, out[0], error);
    case Op::QuantDequant: return need(2) && opQuantDequant(ctx, *in[0], *in[1], opt(2), out[0], error);
    case Op::Constant: out[0] = n.value; return true;
    default: return fail(error, "operator not implemented");
    }
}

}  // namespace tts
