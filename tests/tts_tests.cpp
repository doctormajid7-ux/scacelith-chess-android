// Text-to-speech runtime (src/tts): the ONNX reader on a hand-made model, every operator against
// hand-computed (numpy semantics) results, the GEMM and element-wise kernels of every instruction
// set this CPU runs against scalar references, the text front end, and, when the model files are
// in <exe dir>/coach/, every stage against onnxruntime reference dumps (tests/data/tts, written by
// tools/tts_reference.py), the whole synthesis and the background worker. Tests that need the
// model are skipped without it.
//
// Optional, through environment variables:
//   SCACELITH_TTS_PERF=1                 real-time factor of a 4-second sentence at 1 and 2 threads
//   SCACELITH_TTS_SAMPLES=<dir>          voice and pronunciation samples (WAV) into <dir>
//   SCACELITH_TTS_NODE_DUMP=<file> SCACELITH_TTS_NODE_STAGE=dp|te|ve|voc
//                                        per-node comparison with tools/tts_node_dump.py output
#include "test.h"
#include "audio/offline.h"
#include "net/net_sys.h"
#include "repo_files.h"
#include "tts/gemm.h"
#include "tts/graph.h"
#include "tts/model.h"
#include "tts/onnx.h"
#include "tts/text.h"
#include "tts/threads.h"
#include "tts/model_store.h"
#include "tts/tts.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#define CHECK_RUN(EXPR, ERR)                                                \
    do {                                                                    \
        bool ok_ = (EXPR);                                                  \
        if (!ok_) std::fprintf(stderr, "  error: %s\n", (ERR).c_str());    \
        CHECK(ok_);                                                         \
    } while (0)

using tts::DType;
using tts::Dims;
using tts::Tensor;

namespace {

// ------------------------------------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------------------------------------
Tensor F(const Dims& d, const std::vector<float>& v) {
    Tensor t = Tensor::alloc(DType::F32, d);
    if (!v.empty()) std::memcpy(t.mut<float>(), v.data(), v.size() * 4);
    return t;
}

Tensor I(const Dims& d, const std::vector<int64_t>& v) {
    Tensor t = Tensor::alloc(DType::I64, d);
    if (!v.empty()) std::memcpy(t.mut<int64_t>(), v.data(), v.size() * 8);
    return t;
}

template <class T>
Tensor typed(DType type, const Dims& d, const std::vector<T>& v) {
    Tensor t = Tensor::alloc(type, d);
    if (!v.empty()) std::memcpy(t.mut<T>(), v.data(), v.size() * sizeof(T));
    return t;
}

const tts::kern::Table& K() { return tts::kern::active(); }

// Runs one operator node on the given inputs (nullptr = absent optional input).
bool runOp(tts::Node n, const std::vector<const Tensor*>& in, std::vector<Tensor>& out, size_t outputs = 1,
           const tts::kern::Table* k = nullptr, tts::ThreadPool* pool = nullptr) {
    n.in.assign(in.size(), 0);
    for (size_t i = 0; i < in.size(); ++i)
        if (!in[i]) n.in[i] = -1;
    n.out.assign(outputs, 0);
    out.assign(outputs, Tensor());
    tts::ExecContext ctx;
    ctx.k = k ? k : &K();
    ctx.pool = pool;
    std::string err;
    bool ok = tts::execNode(n, in.data(), out.data(), ctx, &err);
    if (!ok) std::fprintf(stderr, "  %s: %s\n", tts::opName(n.op), err.c_str());
    return ok;
}

tts::Node node(tts::Op op) {
    tts::Node n;
    n.op = op;
    return n;
}

bool near(float a, float b, float tol = 1e-5f) { return std::fabs(a - b) <= tol * std::max(1.0f, std::fabs(b)); }

bool equalF(const Tensor& t, const Dims& d, const std::vector<float>& v, float tol = 1e-5f) {
    if (t.type != DType::F32 || t.dims != d || size_t(t.count()) != v.size()) {
        std::fprintf(stderr, "  shape/type mismatch (rank %d, %lld elements)\n", t.rank(), (long long)t.count());
        return false;
    }
    for (size_t i = 0; i < v.size(); ++i)
        if (!near(t.as<float>()[i], v[i], tol)) {
            std::fprintf(stderr, "  [%zu] %.7g vs %.7g\n", i, t.as<float>()[i], v[i]);
            return false;
        }
    return true;
}

template <class T>
bool equalT(const Tensor& t, DType type, const Dims& d, const std::vector<T>& v) {
    if (t.type != type || t.dims != d || size_t(t.count()) != v.size()) return false;
    for (size_t i = 0; i < v.size(); ++i)
        if (t.as<T>()[i] != v[i]) return false;
    return true;
}

// Comparison of a result with its reference.
struct Diff {
    double maxAbs = 0, cosine = 0, snrDb = 0, refMaxAbs = 0;
};

Diff diff(const float* a, const float* ref, size_t n) {
    Diff d;
    double dot = 0, na = 0, nr = 0, err = 0;
    for (size_t i = 0; i < n; ++i) {
        double x = a[i], r = ref[i];
        d.maxAbs = std::max(d.maxAbs, std::fabs(x - r));
        d.refMaxAbs = std::max(d.refMaxAbs, std::fabs(r));
        dot += x * r;
        na += x * x;
        nr += r * r;
        err += (x - r) * (x - r);
    }
    d.cosine = na > 0 && nr > 0 ? dot / std::sqrt(na * nr) : (na == nr ? 1.0 : 0.0);
    d.snrDb = err > 0 ? 10.0 * std::log10(nr / err) : 200.0;
    return d;
}

Diff diff(const Tensor& a, const Tensor& ref) {
    if (a.type != DType::F32 || ref.type != DType::F32 || a.count() != ref.count()) {
        std::fprintf(stderr, "  diff: mismatched tensors (%lld vs %lld elements)\n", (long long)a.count(),
                     (long long)ref.count());
        return Diff();
    }
    return diff(a.as<float>(), ref.as<float>(), size_t(a.count()));
}

// Mean absolute difference of log-magnitude spectrograms (1024-point Hann frames, hop 256), in dB,
// over the bins within 60 dB of the reference's loudest: a waveform metric that tolerates the tiny
// phase shifts to which sample-wise SNR is very sensitive.
double logSpectralDistance(const float* a, const float* ref, size_t n) {
    const size_t N = 1024, hop = 256;
    if (n < N) return 0.0;
    std::vector<double> win(N);
    for (size_t i = 0; i < N; ++i) win[i] = 0.5 - 0.5 * std::cos(2.0 * 3.141592653589793 * double(i) / double(N));
    auto spectrum = [&](const float* x, size_t start, std::vector<double>& out) {
        std::vector<double> re(N), im(N, 0.0);
        for (size_t i = 0; i < N; ++i) re[i] = x[start + i] * win[i];
        for (size_t i = 1, j = 0; i < N; ++i) {   // iterative radix-2 FFT
            size_t bit = N >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(re[i], re[j]);
        }
        for (size_t len = 2; len <= N; len <<= 1) {
            double ang = -2.0 * 3.141592653589793 / double(len);
            for (size_t i = 0; i < N; i += len)
                for (size_t k = 0; k < len / 2; ++k) {
                    double wr = std::cos(ang * double(k)), wi = std::sin(ang * double(k));
                    double xr = re[i + k + len / 2] * wr - im[i + k + len / 2] * wi;
                    double xi = re[i + k + len / 2] * wi + im[i + k + len / 2] * wr;
                    re[i + k + len / 2] = re[i + k] - xr;
                    im[i + k + len / 2] = im[i + k] - xi;
                    re[i + k] += xr;
                    im[i + k] += xi;
                }
        }
        out.resize(N / 2 + 1);
        for (size_t k = 0; k <= N / 2; ++k) out[k] = 20.0 * std::log10(std::sqrt(re[k] * re[k] + im[k] * im[k]) + 1e-5);
    };
    std::vector<std::vector<double>> A, R;
    double top = -1e9;
    for (size_t s = 0; s + N <= n; s += hop) {
        A.emplace_back();
        R.emplace_back();
        spectrum(a, s, A.back());
        spectrum(ref, s, R.back());
        for (double v : R.back()) top = std::max(top, v);
    }
    double sum = 0;
    size_t cnt = 0;
    for (size_t f = 0; f < R.size(); ++f)
        for (size_t k = 0; k < R[f].size(); ++k)
            if (R[f][k] > top - 60.0) {
                sum += std::fabs(A[f][k] - R[f][k]);
                ++cnt;
            }
    return cnt ? sum / double(cnt) : 0.0;
}

void report(const char* what, const Diff& d) {
    std::fprintf(stderr, "  %-34s max|d| %.3g (ref max %.3g)  cosine %.8f  SNR %.1f dB\n", what, d.maxAbs, d.refMaxAbs,
                 d.cosine, d.snrDb);
}

// Reference dump container (tools/tts_reference.py): "STTD", u32 version, u32 count, then per
// tensor u16 name length, name, u8 ONNX element type, u8 rank, i64 dims, raw data.
struct Dump {
    std::map<std::string, Tensor> t;
    bool has(const std::string& n) const { return t.count(n) != 0; }
    const Tensor& operator[](const std::string& n) const {
        static const Tensor none;
        auto it = t.find(n);
        return it == t.end() ? none : it->second;
    }
    bool parse(const std::string& bytes) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(bytes.data());
        const uint8_t* end = p + bytes.size();
        auto need = [&](size_t n) { return size_t(end - p) >= n; };
        if (!need(12) || std::memcmp(p, "STTD", 4) != 0) return false;
        uint32_t count;
        std::memcpy(&count, p + 8, 4);
        p += 12;
        for (uint32_t i = 0; i < count; ++i) {
            if (!need(2)) return false;
            uint16_t len;
            std::memcpy(&len, p, 2);
            p += 2;
            if (!need(size_t(len) + 2)) return false;
            std::string name(reinterpret_cast<const char*>(p), len);
            p += len;
            DType type = DType(p[0]);
            int rank = p[1];
            p += 2;
            if (!need(size_t(rank) * 8)) return false;
            Dims d(size_t(rank), 0);
            if (rank) std::memcpy(d.data(), p, size_t(rank) * 8);
            p += size_t(rank) * 8;
            Tensor x = Tensor::alloc(type, d);
            if (!need(x.bytes())) return false;
            if (x.bytes()) std::memcpy(x.mut<uint8_t>(), p, x.bytes());
            p += x.bytes();
            t[name] = x;
        }
        return true;
    }
};

bool loadDump(const std::string& rel, Dump& d) {
    std::string bytes = readRepoFile(rel);
    if (bytes.empty() || !d.parse(bytes)) {
        std::fprintf(stderr, "  %s not found or unreadable (run from the repository root or set SCACELITH_SOURCE_DIR)\n",
                     rel.c_str());
        return false;
    }
    return true;
}

std::string modelDir() { return net::sys::exeDirectory() + "coach/"; }

// The synthesizer shared by the model tests (loaded once), or nullptr when the model is absent.
tts::Synthesizer* model() {
    static std::unique_ptr<tts::Synthesizer> s;
    static bool tried = false;
    if (!tried) {
        tried = true;
        auto p = std::make_unique<tts::Synthesizer>();
        std::string err;
        if (p->loadFrom(modelDir(), &err)) s = std::move(p);
        else std::fprintf(stderr, "  (model not loaded: %s)\n", err.c_str());
    }
    return s.get();
}

std::vector<int> levelsRun() {
    std::vector<int> v;
    for (int l = 0; l < tts::kern::kLevelCount; ++l)
        if (tts::kern::tableFor(l)) v.push_back(l);
    return v;
}

// The tables the kernel tests check: every level this CPU runs, then the portable table (the one
// of the arm64 builds, kernels_portable.cpp), level -1 here.
std::vector<int> tablesToCheck() {
    std::vector<int> v = levelsRun();
    v.push_back(-1);
    return v;
}
const tts::kern::Table* tableToCheck(int level) { return level < 0 ? tts::kern::tablePortable() : tts::kern::tableFor(level); }

// ------------------------------------------------------------------------------------------------
// Protobuf writer for the hand-made model
// ------------------------------------------------------------------------------------------------
struct Pb {
    std::string b;
    void varint(uint64_t v) {
        while (v >= 0x80) {
            b.push_back(char(v | 0x80));
            v >>= 7;
        }
        b.push_back(char(v));
    }
    void key(int field, int wire) { varint(uint64_t(field) << 3 | uint64_t(wire)); }
    Pb& i(int field, uint64_t v) {
        key(field, 0);
        varint(v);
        return *this;
    }
    Pb& s(int field, const std::string& v) {
        key(field, 2);
        varint(v.size());
        b += v;
        return *this;
    }
    Pb& m(int field, const Pb& v) { return s(field, v.b); }
    Pb& f32(int field, float v) {
        key(field, 5);
        char c[4];
        std::memcpy(c, &v, 4);
        b.append(c, 4);
        return *this;
    }
};

template <class T>
std::string bytesOf(const std::vector<T>& v) {
    return std::string(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(T));
}

// TensorProto with raw_data (ONNX element type 'type').
Pb tensorPb(const std::string& name, const Dims& d, int type, const std::string& raw) {
    Pb t;
    for (int64_t x : d) t.i(1, uint64_t(x));
    t.i(2, uint64_t(type));
    t.s(9, raw);
    t.s(8, name);
    return t;
}

// NodeProto with its attributes (AttributeProto messages).
Pb nodePb(const std::string& op, const std::vector<std::string>& in, const std::vector<std::string>& out,
          const std::vector<Pb>& attrs = {}) {
    Pb n;
    for (auto& s : in) n.s(1, s);
    for (auto& s : out) n.s(2, s);
    n.s(3, op + "_" + out[0]);
    n.s(4, op);
    for (const Pb& a : attrs) n.m(5, a);
    return n;
}

Pb attrInts(const std::string& name, const std::vector<int64_t>& v) {
    Pb a;
    a.s(1, name);
    for (int64_t x : v) a.i(8, uint64_t(x));
    return a;
}

Pb attrTensor(const std::string& name, const Pb& t) {
    Pb a;
    a.s(1, name);
    a.m(5, t);
    return a;
}

// ModelProto (opset 19) of one graph.
std::string modelPb(const std::vector<Pb>& nodes, const std::vector<Pb>& inits, const std::vector<std::string>& inputs,
                    const std::vector<std::string>& outputs) {
    Pb graph;
    for (const Pb& n : nodes) graph.m(1, n);
    graph.s(2, "test");
    for (const Pb& t : inits) graph.m(5, t);
    for (auto& s : inputs) graph.m(11, Pb().s(1, s));
    for (auto& s : outputs) graph.m(12, Pb().s(1, s));
    Pb opset;
    opset.s(1, "").i(2, 19);
    Pb model;
    model.i(1, 9).m(7, graph).m(8, opset);
    return model.b;
}

// Graph::load of a serialized model ('bytes' must outlive the graph).
bool loadModel(const std::string& bytes, tts::Graph& g, std::string* err) {
    return g.load(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), "test", {}, K(), err);
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// ONNX reader and graph on a hand-made model:
//   t = Transpose(x, perm=[1,0]); y = MatMul(t, w) + b; z = Gelu pattern(y); out = Relu(z)
// ------------------------------------------------------------------------------------------------
TEST(tts_onnx_reader_tiny_model) {
    auto tensor = [](const std::string& name, const Dims& d, const std::vector<float>& v, bool raw) {
        Pb t;
        for (int64_t x : d) t.i(1, uint64_t(x));
        t.i(2, 1);
        if (raw) {
            t.s(9, std::string(reinterpret_cast<const char*>(v.data()), v.size() * 4));
        } else {
            Pb packed;
            for (float f : v) {
                char c[4];
                std::memcpy(c, &f, 4);
                packed.b.append(c, 4);
            }
            t.s(4, packed.b);   // float_data, packed
        }
        t.s(8, name);
        return t;
    };
    auto nodeOf = [](const std::string& op, std::vector<std::string> in, std::vector<std::string> out) {
        Pb n;
        for (auto& s : in) n.s(1, s);
        for (auto& s : out) n.s(2, s);
        n.s(3, op + "_" + out[0]);
        n.s(4, op);
        return n;
    };
    Pb graph;
    Pb tr = nodeOf("Transpose", {"x"}, {"t"});
    Pb perm;
    perm.s(1, "perm").i(20, 7);
    Pb ints;
    ints.varint(1);
    ints.varint(0);
    perm.s(8, ints.b);   // packed ints
    tr.m(5, perm);
    graph.m(1, tr);
    graph.m(1, nodeOf("MatMul", {"t", "w"}, {"mm"}));
    graph.m(1, nodeOf("Add", {"mm", "b"}, {"y"}));
    graph.m(1, nodeOf("Div", {"y", "sqrt2"}, {"g1"}));
    graph.m(1, nodeOf("Erf", {"g1"}, {"g2"}));
    graph.m(1, nodeOf("Add", {"g2", "one"}, {"g3"}));
    graph.m(1, nodeOf("Mul", {"y", "g3"}, {"g4"}));
    graph.m(1, nodeOf("Mul", {"g4", "half"}, {"z"}));
    graph.m(1, nodeOf("Relu", {"z"}, {"out"}));
    graph.s(2, "tiny");
    graph.m(5, tensor("w", {2, 3}, {1, 2, 3, -1, 0.5f, 2}, true));
    graph.m(5, tensor("b", {3}, {0.1f, -0.2f, 0.3f}, false));
    graph.m(5, tensor("sqrt2", {}, {1.41421353816986083984375f}, false));
    graph.m(5, tensor("one", {}, {1.0f}, false));
    graph.m(5, tensor("half", {}, {0.5f}, false));
    Pb vin, vout;
    vin.s(1, "x");
    vout.s(1, "out");
    graph.m(11, vin);
    graph.m(12, vout);
    Pb opset;
    opset.s(1, "").i(2, 19);
    Pb model;
    model.i(1, 9).s(2, "hand").m(7, graph).m(8, opset);

    tts::onnx::Model m;
    std::string err;
    const uint8_t* data = reinterpret_cast<const uint8_t*>(model.b.data());
    CHECK(tts::onnx::parse(data, model.b.size(), m, &err));
    CHECK_EQ(m.irVersion, 9);
    CHECK_EQ(m.opset, 19);
    CHECK_EQ(m.producer, std::string("hand"));
    CHECK_EQ(m.nodes.size(), size_t(9));
    CHECK_EQ(m.initializers.size(), size_t(5));
    CHECK(m.nodes[0].attribute("perm") && m.nodes[0].attribute("perm")->ints == std::vector<int64_t>({1, 0}));
    CHECK(m.initializers[0].raw != nullptr && m.initializers[0].rawSize == 24);
    CHECK(m.initializers[1].raw == nullptr && m.initializers[1].decoded.size() == 12);
    // A truncated file is refused, not read past its end.
    tts::onnx::Model bad;
    CHECK(!tts::onnx::parse(data, model.b.size() - 7, bad, &err));

    tts::Graph g;
    CHECK(g.load(data, model.b.size(), "tiny", {}, K(), &err));
    CHECK_EQ(g.fusedCount(), 1);   // the GELU pattern
    tts::Session s(g);
    s.setInput(0, F({2, 2}, {1, -2, 0.5f, 3}));   // x^T = [[1, 0.5], [-2, 3]]
    tts::ExecContext ctx;
    ctx.k = &K();
    CHECK(s.run(ctx, &err));
    // y = x^T w + b
    const float xT[2][2] = {{1, 0.5f}, {-2, 3}};
    const float w[2][3] = {{1, 2, 3}, {-1, 0.5f, 2}};
    const float b[3] = {0.1f, -0.2f, 0.3f};
    std::vector<float> want;
    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 3; ++c) {
            double y = xT[r][0] * w[0][c] + xT[r][1] * w[1][c] + b[c];
            double gelu = 0.5 * y * (1.0 + std::erf(y / std::sqrt(2.0)));
            want.push_back(float(std::max(0.0, gelu)));
        }
    CHECK(equalF(s.output(0), {2, 3}, want, 1e-6f));
}

// Malformed files are refused by the reader: nothing is half-parsed, read or copied past its data.
TEST(tts_onnx_reader_malformed) {
    tts::onnx::Model m;
    std::string err;
    auto parses = [&](const std::string& b) {
        return tts::onnx::parse(reinterpret_cast<const uint8_t*>(b.data()), b.size(), m, &err);
    };
    // Constant / ConstantOfShape tensors whose data is larger or smaller than their dims say.
    for (const char* op : {"Constant", "ConstantOfShape"})
        for (size_t bytes : {size_t(4096), size_t(0)}) {
            Pb value = attrTensor("value", tensorPb("v", {}, 1, std::string(bytes, 'x')));
            std::string b = modelPb({nodePb(op, {}, {"y"}, {value})}, {}, {}, {"y"});
            CHECK(!parses(b));
            tts::Graph g;
            CHECK(!loadModel(b, g, &err) && err.find("size mismatch") != std::string::npos);
        }
    Pb value = attrTensor("value", tensorPb("v", {}, 1, std::string(4, 'x')));
    CHECK(parses(modelPb({nodePb("Constant", {}, {"y"}, {value})}, {}, {}, {"y"})));
    // Negative initializer dims, even when their product is the element count.
    Pb neg = tensorPb("w", {-1, -4}, 1, std::string(16, 'x'));
    CHECK(!parses(modelPb({nodePb("Identity", {"w"}, {"y"})}, {neg}, {}, {"y"})));
    // data_location DEFAULT (0) after the data is inline data; EXTERNAL and external_data are refused.
    Pb inl = tensorPb("w", {4}, 1, std::string(16, 'x'));
    inl.i(14, 0);
    CHECK(parses(modelPb({nodePb("Identity", {"w"}, {"y"})}, {inl}, {}, {"y"})));
    CHECK_EQ(m.initializers.size(), size_t(1));
    Pb ext = tensorPb("w", {4}, 1, std::string(16, 'x'));
    ext.i(14, 1);
    CHECK(!parses(modelPb({nodePb("Identity", {"w"}, {"y"})}, {ext}, {}, {"y"})));
    Pb ext2 = tensorPb("w", {4}, 1, "");
    ext2.m(13, Pb().s(1, "location").s(2, "w.bin"));
    CHECK(!parses(modelPb({nodePb("Identity", {"w"}, {"y"})}, {ext2}, {}, {"y"})));
    // An attribute cut inside the node's own length: the node is not kept without it.
    Pb perm = attrInts("perm", {1, 0});
    Pb tr;
    tr.s(1, "x").s(2, "y").s(3, "t").s(4, "Transpose");
    tr.s(5, perm.b.substr(0, perm.b.size() - 1));
    CHECK(!parses(modelPb({tr}, {}, {"x"}, {"y"})));
    CHECK(parses(modelPb({nodePb("Transpose", {"x"}, {"y"}, {perm})}, {}, {"x"}, {"y"})));
    // A zero byte where a tag should be (a zero-filled block) does not end the node early.
    Pb zero = nodePb("Transpose", {"x"}, {"y"}, {perm});
    zero.b.push_back('\0');
    CHECK(!parses(modelPb({zero}, {}, {"x"}, {"y"})));
}

// Nodes with fewer inputs or outputs than their operator needs are refused at load, before
// constant folding or a run writes past the outputs or reads a missing input.
TEST(tts_graph_arity) {
    std::string err;
    auto loads = [&](const Pb& node, const std::vector<std::string>& outs) {
        tts::Graph g;
        return loadModel(modelPb({node}, {}, {"x"}, outs), g, &err);
    };
    CHECK(loads(nodePb("DynamicQuantizeLinear", {"x"}, {"q", "s", "z"}), {"q"}));
    CHECK(!loads(nodePb("DynamicQuantizeLinear", {"x"}, {"q"}), {"q"}) && err.find("wrong number") != std::string::npos);
    CHECK(loads(nodePb("Add", {"x", "x"}, {"y"}), {"y"}));
    CHECK(!loads(nodePb("Add", {"x"}, {"y"}), {"y"}));
    CHECK(!loads(nodePb("DequantizeLinear", {"x", ""}, {"y"}), {"y"}));
    CHECK(loads(nodePb("Clip", {"x", "", ""}, {"y"}), {"y"}));   // optional inputs may be empty
    Pb noOutput;
    noOutput.s(1, "x").s(3, "r").s(4, "Relu");
    CHECK(!loads(noOutput, {}));
}

// The dynamic-quantized MatMul pattern fuses its bias only when it has one value per column; a
// broadcast bias stays a separate Add and is not read past its end. Folding a DequantizeLinear
// into a quantized weight checks its scale and zero point.
TEST(tts_graph_quantized_matmul) {
    std::string err;
    const std::vector<int8_t> w = {1, -2, 3, 4, 0, 5, -6, 7, 8, 9, -10, 11};   // [3, 4]
    const std::vector<uint8_t> x = {1, 2, 3, 200, 100, 0};                      // [2, 3]
    Pb to;
    to.s(1, "to").i(3, uint64_t(tts::onnx::kFloat));
    for (int64_t biasLen : {4, 1}) {
        std::vector<float> bias(static_cast<size_t>(biasLen));
        for (size_t j = 0; j < bias.size(); ++j) bias[j] = 0.25f * float(j + 1);
        std::string b = modelPb({nodePb("MatMulInteger", {"x", "w"}, {"acc"}), nodePb("Cast", {"acc"}, {"f"}, {to}),
                                 nodePb("Mul", {"f", "s"}, {"m"}), nodePb("Add", {"m", "bias"}, {"y"})},
                                {tensorPb("w", {3, 4}, tts::onnx::kInt8, bytesOf(w)),
                                 tensorPb("s", {}, tts::onnx::kFloat, bytesOf(std::vector<float>{0.5f})),
                                 tensorPb("bias", {biasLen}, tts::onnx::kFloat, bytesOf(bias))},
                                {"x"}, {"y"});
        tts::Graph g;
        CHECK_RUN(loadModel(b, g, &err), err);
        CHECK_EQ(g.fusedCount(), 1);
        tts::Session sess(g);
        sess.setInput(0, typed<uint8_t>(DType::U8, {2, 3}, x));
        tts::ExecContext ctx;
        ctx.k = &K();
        CHECK_RUN(sess.run(ctx, &err), err);
        std::vector<float> want;
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < 4; ++j) {
                int acc = 0;
                for (int k = 0; k < 3; ++k) acc += int(x[size_t(i * 3 + k)]) * int(w[size_t(k * 4 + j)]);
                want.push_back(float(acc) * 0.5f + bias[biasLen == 4 ? size_t(j) : 0]);
            }
        CHECK(equalF(sess.output(0), {2, 4}, want, 0.0f));
    }
    // A folded DequantizeLinear of a large int8 weight: an 8-bit scale or a zero point of another
    // type than the weight is refused.
    std::string q(4096, '\x01');
    for (int bad = 0; bad < 2; ++bad) {
        Pb scale = bad == 0 ? tensorPb("s", {}, tts::onnx::kInt8, std::string(1, '\x02'))
                            : tensorPb("s", {}, tts::onnx::kFloat, bytesOf(std::vector<float>{0.5f}));
        Pb zp = tensorPb("z", {}, tts::onnx::kUint8, std::string(1, '\x00'));
        std::string b = modelPb({nodePb("DequantizeLinear", {"q", "s", "z"}, {"d"}), nodePb("Add", {"x", "d"}, {"y"})},
                                {tensorPb("q", {4096}, tts::onnx::kInt8, q), scale, zp}, {"x"}, {"y"});
        tts::Graph g;
        CHECK(!loadModel(b, g, &err) && err.find("quantization") != std::string::npos);
    }
}

// ------------------------------------------------------------------------------------------------
// Operators (numpy / ONNX semantics)
// ------------------------------------------------------------------------------------------------
TEST(tts_ops_broadcast_arith) {
    std::vector<Tensor> o;
    Tensor a = F({2, 3}, {1, 2, 3, 4, 5, 6});
    Tensor b = F({3}, {10, 20, 30});
    Tensor c = F({2, 1}, {2, 4});
    CHECK(runOp(node(tts::Op::Add), {&a, &b}, o) && equalF(o[0], {2, 3}, {11, 22, 33, 14, 25, 36}));
    CHECK(runOp(node(tts::Op::Sub), {&b, &a}, o) && equalF(o[0], {2, 3}, {9, 18, 27, 6, 15, 24}));
    CHECK(runOp(node(tts::Op::Mul), {&a, &c}, o) && equalF(o[0], {2, 3}, {2, 4, 6, 16, 20, 24}));
    CHECK(runOp(node(tts::Op::Div), {&a, &c}, o) && equalF(o[0], {2, 3}, {0.5f, 1, 1.5f, 1, 1.25f, 1.5f}));
    Tensor col = F({3, 1}, {1, 2, 3}), row = F({1, 2}, {10, 20});
    CHECK(runOp(node(tts::Op::Add), {&col, &row}, o) && equalF(o[0], {3, 2}, {11, 21, 12, 22, 13, 23}));
    Tensor two = F({}, {2.0f});
    CHECK(runOp(node(tts::Op::Pow), {&a, &two}, o) && equalF(o[0], {2, 3}, {1, 4, 9, 16, 25, 36}));
    // Integer Pow: huge exponents of 0 and +-1 are instant, an overflow fails instead of wrapping.
    {
        const int64_t big = int64_t(1) << 62;
        Tensor base = I({6}, {3, -2, 1, -1, 0, 5}), ex = I({6}, {4, 63, big, big + 1, big, -1});
        CHECK(runOp(node(tts::Op::Pow), {&base, &ex}, o) &&
              equalT<int64_t>(o[0], DType::I64, {6}, {81, std::numeric_limits<int64_t>::min(), 1, -1, 0, 0}));
        Tensor b2 = I({1}, {2}), e63 = I({1}, {63}), e40 = I({1}, {40}), b3 = I({1}, {3});
        CHECK(!runOp(node(tts::Op::Pow), {&b2, &e63}, o));
        CHECK(!runOp(node(tts::Op::Pow), {&b3, &e40}, o));
        Tensor ef = F({}, {2.0f}), enan = F({}, {std::nanf("")}), ehuge = F({}, {1e30f});
        CHECK(runOp(node(tts::Op::Pow), {&base, &ef}, o) && equalT<int64_t>(o[0], DType::I64, {6}, {9, 4, 1, 1, 0, 25}));
        CHECK(!runOp(node(tts::Op::Pow), {&base, &enan}, o));
        CHECK(!runOp(node(tts::Op::Pow), {&base, &ehuge}, o));
    }
    Tensor ia = I({3}, {7, -7, 9}), ib = I({1}, {2});
    CHECK(runOp(node(tts::Op::Div), {&ia, &ib}, o) && equalT<int64_t>(o[0], DType::I64, {3}, {3, -3, 4}));
    CHECK(runOp(node(tts::Op::Equal), {&ia, &ib}, o) && equalT<uint8_t>(o[0], DType::Bool, {3}, {0, 0, 0}));
    Tensor cond = typed<uint8_t>(DType::Bool, {2, 1}, {1, 0});
    Tensor zero = F({}, {0.0f});
    CHECK(runOp(node(tts::Op::Where), {&cond, &a, &zero}, o) && equalF(o[0], {2, 3}, {1, 2, 3, 0, 0, 0}));
    Tensor slope = F({3}, {0.1f, 0.2f, 0.3f});
    Tensor neg = F({2, 3}, {-1, 2, -3, 4, -5, 6});
    CHECK(runOp(node(tts::Op::PRelu), {&neg, &slope}, o) && equalF(o[0], {2, 3}, {-0.1f, 2, -0.9f, 4, -1.0f, 6}));
    // Empty broadcast shape.
    Tensor e = F({0, 3}, {});
    CHECK(runOp(node(tts::Op::Add), {&e, &b}, o) && o[0].dims == Dims({0, 3}));
}

TEST(tts_ops_unary) {
    std::vector<Tensor> o;
    Tensor x = F({5}, {-3.0f, -0.5f, 0.0f, 0.7f, 2.5f});
    CHECK(runOp(node(tts::Op::Relu), {&x}, o) && equalF(o[0], {5}, {0, 0, 0, 0.7f, 2.5f}));
    Tensor lo = F({}, {-1.0f}), hi = F({}, {1.0f});
    CHECK(runOp(node(tts::Op::Clip), {&x, &lo, &hi}, o) && equalF(o[0], {5}, {-1, -0.5f, 0, 0.7f, 1}));
    std::vector<float> want;
    for (float v : {-3.0f, -0.5f, 0.0f, 0.7f, 2.5f}) want.push_back(std::log1p(std::exp(v)));
    CHECK(runOp(node(tts::Op::Softplus), {&x}, o) && equalF(o[0], {5}, want, 1e-6f));
    want.clear();
    for (float v : {-3.0f, -0.5f, 0.0f, 0.7f, 2.5f}) want.push_back(std::sin(v));
    CHECK(runOp(node(tts::Op::Sin), {&x}, o) && equalF(o[0], {5}, want, 1e-6f));
    tts::Node cast = node(tts::Op::Cast);
    cast.i0 = tts::onnx::kInt64;
    CHECK(runOp(cast, {&x}, o) && equalT<int64_t>(o[0], DType::I64, {5}, {-3, 0, 0, 0, 2}));
    Tensor i32 = typed<int32_t>(DType::I32, {2}, {-5, 70000});
    cast.i0 = tts::onnx::kFloat;
    CHECK(runOp(cast, {&i32}, o) && equalF(o[0], {2}, {-5, 70000}));
}

TEST(tts_ops_shapes) {
    std::vector<Tensor> o;
    Tensor x = F({2, 3, 2}, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11});
    tts::Node tr = node(tts::Op::Transpose);
    tr.ints = {0, 2, 1};
    CHECK(runOp(tr, {&x}, o) && equalF(o[0], {2, 2, 3}, {0, 2, 4, 1, 3, 5, 6, 8, 10, 7, 9, 11}));
    tr.ints = {2, 0, 1};
    CHECK(runOp(tr, {&x}, o) && equalF(o[0], {2, 2, 3}, {0, 2, 4, 6, 8, 10, 1, 3, 5, 7, 9, 11}));
    Tensor shp = I({3}, {0, -1, 3});
    CHECK(runOp(node(tts::Op::Reshape), {&x, &shp}, o) && o[0].dims == Dims({2, 2, 3}));
    tts::Node sh = node(tts::Op::Shape);
    sh.i0 = 1;
    sh.i1 = std::numeric_limits<int64_t>::max();
    CHECK(runOp(sh, {&x}, o) && equalT<int64_t>(o[0], DType::I64, {2}, {3, 2}));
    Tensor axes = I({1}, {1});
    CHECK(runOp(node(tts::Op::Unsqueeze), {&x, &axes}, o) && o[0].dims == Dims({2, 1, 3, 2}));
    Tensor u = o[0];
    CHECK(runOp(node(tts::Op::Squeeze), {&u, &axes}, o) && o[0].dims == Dims({2, 3, 2}));
    tts::Node cat = node(tts::Op::Concat);
    cat.axis = -1;
    Tensor y = F({2, 3, 1}, {-1, -2, -3, -4, -5, -6});
    CHECK(runOp(cat, {&x, &y}, o) &&
          equalF(o[0], {2, 3, 3}, {0, 1, -1, 2, 3, -2, 4, 5, -3, 6, 7, -4, 8, 9, -5, 10, 11, -6}));
    tts::Node sp = node(tts::Op::Split);
    sp.axis = 1;
    Tensor sizes = I({2}, {1, 2});
    CHECK(runOp(sp, {&x, &sizes}, o, 2) && equalF(o[0], {2, 1, 2}, {0, 1, 6, 7}) &&
          equalF(o[1], {2, 2, 2}, {2, 3, 4, 5, 8, 9, 10, 11}));
    sp.i0 = 2;   // num_outputs, uneven: ceil
    Tensor v5 = F({5}, {1, 2, 3, 4, 5});
    sp.axis = 0;
    CHECK(runOp(sp, {&v5, nullptr}, o, 2) && equalF(o[0], {3}, {1, 2, 3}) && equalF(o[1], {2}, {4, 5}));
    // Slice: negative start, step 2, and a reversed slice.
    Tensor st = I({1}, {-5}), en = I({1}, {100}), ax = I({1}, {0}), step = I({1}, {2});
    CHECK(runOp(node(tts::Op::Slice), {&v5, &st, &en, &ax, &step}, o) && equalF(o[0], {3}, {1, 3, 5}));
    Tensor rs = I({1}, {-1}), re = I({1}, {-100}), rstep = I({1}, {-1});
    CHECK(runOp(node(tts::Op::Slice), {&v5, &rs, &re, &ax, &rstep}, o) && equalF(o[0], {5}, {5, 4, 3, 2, 1}));
    Tensor st2 = I({1}, {1}), en2 = I({1}, {2}), ax2 = I({1}, {2});
    CHECK(runOp(node(tts::Op::Slice), {&x, &st2, &en2, &ax2, nullptr}, o) &&
          equalF(o[0], {2, 3, 1}, {1, 3, 5, 7, 9, 11}));
    // Gather with a negative index (from the end), on axis 1.
    tts::Node ga = node(tts::Op::Gather);
    ga.axis = 1;
    Tensor idx = I({2}, {-1, 0});
    CHECK(runOp(ga, {&x, &idx}, o) && equalF(o[0], {2, 2, 2}, {4, 5, 0, 1, 10, 11, 6, 7}));
    Tensor table = F({3, 2}, {1, 2, 3, 4, 5, 6});
    Tensor ids = I({1, 2}, {2, 0});
    ga.axis = 0;
    CHECK(runOp(ga, {&table, &ids}, o) && equalF(o[0], {1, 2, 2}, {5, 6, 1, 2}));
    Tensor es = I({3}, {2, 2, 3});
    Tensor col = F({2, 1}, {7, 8});
    CHECK(runOp(node(tts::Op::Expand), {&col, &es}, o) && equalF(o[0], {2, 2, 3}, {7, 7, 7, 8, 8, 8, 7, 7, 7, 8, 8, 8}));
    Tensor reps = I({2}, {2, 3});
    Tensor m = F({1, 2}, {1, 2});
    CHECK(runOp(node(tts::Op::Tile), {&m, &reps}, o) &&
          equalF(o[0], {2, 6}, {1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2}));
    tts::Node cos = node(tts::Op::ConstantOfShape);
    cos.value = F({1}, {3.5f});
    Tensor cs = I({2}, {2, 2});
    CHECK(runOp(cos, {&cs}, o) && equalF(o[0], {2, 2}, {3.5f, 3.5f, 3.5f, 3.5f}));
}

TEST(tts_ops_pad_reduce) {
    std::vector<Tensor> o;
    Tensor x = F({1, 2, 3}, {1, 2, 3, 4, 5, 6});
    tts::Node pad = node(tts::Op::Pad);
    pad.mode = "constant";
    Tensor pads = I({6}, {0, 0, 2, 0, 0, 1});
    Tensor val = F({}, {-1.0f});
    CHECK(runOp(pad, {&x, &pads, &val}, o) &&
          equalF(o[0], {1, 2, 6}, {-1, -1, 1, 2, 3, -1, -1, -1, 4, 5, 6, -1}));
    pad.mode = "edge";
    CHECK(runOp(pad, {&x, &pads}, o) && equalF(o[0], {1, 2, 6}, {1, 1, 1, 2, 3, 3, 4, 4, 4, 5, 6, 6}));
    Tensor pads2 = I({6}, {0, 1, 0, 0, 0, 0});
    pad.mode = "constant";
    CHECK(runOp(pad, {&x, &pads2}, o) && equalF(o[0], {1, 3, 3}, {0, 0, 0, 1, 2, 3, 4, 5, 6}));
    // Random shapes against a per-element reference: negative pads (crops), edge mode, an empty
    // last dimension, and 1-, 4- and 8-byte elements, with or without a fill value.
    {
        std::mt19937 rp(21);
        bool same = true;
        for (int round = 0; round < 600 && same; ++round) {
            const DType types[] = {DType::U8, DType::F32, DType::I64};
            DType type = types[round % 3];
            size_t es = tts::dtypeSize(type);
            bool edge = round % 2 == 1;
            int r = 1 + int(rp() % 3);
            Dims d(static_cast<size_t>(r)), od(static_cast<size_t>(r));
            std::vector<int64_t> pv(size_t(2 * r));
            for (int k = 0; k < r; ++k) {
                d[size_t(k)] = (edge || k + 1 < r ? 1 : 0) + int64_t(rp() % 6);
                pv[size_t(k)] = int64_t(rp() % 9) - 3;
                pv[size_t(k + r)] = int64_t(rp() % 9) - 3;
                if (d[size_t(k)] + pv[size_t(k)] + pv[size_t(k + r)] < 0) pv[size_t(k + r)] = -d[size_t(k)] - pv[size_t(k)];
                od[size_t(k)] = d[size_t(k)] + pv[size_t(k)] + pv[size_t(k + r)];
            }
            Tensor xr = Tensor::alloc(type, d), padsR = I({2 * r}, pv), val = Tensor::alloc(type, {});
            for (size_t i = 0; i < xr.bytes(); ++i) xr.mut<uint8_t>()[i] = uint8_t(rp());
            for (size_t i = 0; i < es; ++i) val.mut<uint8_t>()[i] = uint8_t(rp());
            bool hasValue = !edge && rp() % 2 == 0;
            pad.mode = edge ? "edge" : "constant";
            same = runOp(pad, {&xr, &padsR, hasValue ? &val : nullptr}, o) && o[0].dims == od;
            for (int64_t e = 0; same && e < o[0].count(); ++e) {
                std::vector<int64_t> c(static_cast<size_t>(r));
                for (int k = r - 1, rem = int(e); k >= 0; --k) {
                    c[size_t(k)] = rem % od[size_t(k)] - pv[size_t(k)];
                    rem /= int(od[size_t(k)]);
                }
                bool in = true;
                int64_t off = 0;
                for (int k = 0; k < r && in; ++k) {
                    if (c[size_t(k)] < 0 || c[size_t(k)] >= d[size_t(k)]) {
                        if (!edge) in = false;
                        else c[size_t(k)] = std::clamp<int64_t>(c[size_t(k)], 0, d[size_t(k)] - 1);
                    }
                    off = off * d[size_t(k)] + c[size_t(k)];
                }
                static const uint8_t zero[8] = {0};
                const uint8_t* want = in ? xr.as<uint8_t>() + off * int64_t(es) : hasValue ? val.as<uint8_t>() : zero;
                same = std::memcmp(o[0].as<uint8_t>() + e * int64_t(es), want, es) == 0;
            }
        }
        CHECK(same);
    }
    tts::Node rs = node(tts::Op::ReduceSum);
    rs.i0 = 1;
    Tensor ax = I({1}, {2});
    CHECK(runOp(rs, {&x, &ax}, o) && equalF(o[0], {1, 2, 1}, {6, 15}));
    rs.i0 = 0;
    Tensor ax1 = I({1}, {1});
    CHECK(runOp(rs, {&x, &ax1}, o) && equalF(o[0], {1, 3}, {5, 7, 9}));
    CHECK(runOp(rs, {&x, nullptr}, o) && equalF(o[0], {}, {21}));
}

// Attributes, axes and inputs out of range (a damaged model file) fail the node instead of
// reading or writing outside a buffer; scalars go through Transpose and Pad.
TEST(tts_ops_bad_attributes) {
    std::vector<Tensor> o;
    Tensor s = F({}, {4.5f}), x = F({2, 3}, {1, 2, 3, 4, 5, 6});
    CHECK(runOp(node(tts::Op::Transpose), {&s}, o) && equalF(o[0], {}, {4.5f}));
    tts::Node pad = node(tts::Op::Pad);
    Tensor noPads = I({0}, {});
    CHECK(runOp(pad, {&s, &noPads}, o) && equalF(o[0], {}, {4.5f}));
    tts::Node tr = node(tts::Op::Transpose);
    for (std::vector<int64_t> perm : {std::vector<int64_t>{0, 0}, {1, 2}, {-1, 0}}) {
        tr.ints = perm;
        CHECK(!runOp(tr, {&x}, o));
    }
    Tensor pads = I({2}, {1, 1}), axis5 = I({1}, {5}), axisM3 = I({1}, {-3});
    CHECK(!runOp(pad, {&x, &pads, nullptr, &axis5}, o));
    CHECK(!runOp(pad, {&x, &pads, nullptr, &axisM3}, o));
    CHECK(!runOp(node(tts::Op::ReduceSum), {&x, &axis5}, o));
    CHECK(!runOp(node(tts::Op::ReduceSum), {&x, &axisM3}, o));
    tts::Node sm = node(tts::Op::Softmax);
    sm.axis = -1;
    Tensor ix = I({2}, {1, 2});
    CHECK(!runOp(sm, {&s}, o));
    CHECK(!runOp(sm, {&ix}, o));
    tts::Node ln = node(tts::Op::LayerNorm);
    ln.axis = -3;
    Tensor g = F({3}, {1, 1, 1});
    CHECK(!runOp(ln, {&x, &g}, o));
    ln.axis = 2;
    CHECK(!runOp(ln, {&x, &g}, o));
    tts::Node sp = node(tts::Op::Split);
    sp.axis = 1;
    Tensor tooLong = I({2}, {2, 2}), negative = I({2}, {-1, 4});
    CHECK(!runOp(sp, {&x, &tooLong}, o, 2));
    CHECK(!runOp(sp, {&x, &negative}, o, 2));
    CHECK(!runOp(sp, {&x}, o, 0));
    Tensor fits = I({2}, {1, 2});
    CHECK(runOp(sp, {&x, &fits}, o, 2) && equalF(o[0], {2, 1}, {1, 4}) && equalF(o[1], {2, 2}, {2, 3, 5, 6}));
    Tensor empty = I({0}, {});
    CHECK(!runOp(node(tts::Op::Clip), {&ix, &empty}, o));
    Tensor cx = F({1, 2, 6}, std::vector<float>(12, 1.0f)), cw = F({2, 1, 3}, std::vector<float>(6, 1.0f));
    tts::Node cv = node(tts::Op::Conv);
    cv.i0 = 2;
    CHECK(runOp(cv, {&cx, &cw}, o) && o[0].dims == Dims({1, 2, 4}));
    for (int field = 0; field < 4; ++field) {
        tts::Node bad = cv;
        if (field == 0) bad.ints3 = {0};          // stride
        if (field == 1) bad.ints = {0};           // dilation
        if (field == 2) bad.ints2 = {-1, 0};      // pads
        if (field == 3) bad.i0 = 0;               // group
        CHECK(!runOp(bad, {&cx, &cw}, o));
    }
    // Shapes computed from data: negative or overflowing dimensions are refused, and a wrapped
    // allocation size throws instead of handing out a tiny block.
    const int64_t huge = int64_t(1) << 40;
    Tensor negShape = I({2}, {-2, -3}), hugeShape = I({2}, {huge, huge}), minus1 = I({1}, {-1});
    CHECK(!runOp(node(tts::Op::Reshape), {&x, &negShape}, o));
    tts::Node cos = node(tts::Op::ConstantOfShape);
    cos.value = F({1}, {1.0f});
    CHECK(!runOp(cos, {&minus1}, o));
    CHECK(!runOp(cos, {&hugeShape}, o));
    Tensor one = F({1}, {1.0f});
    CHECK(!runOp(node(tts::Op::Expand), {&one, &minus1}, o));
    CHECK(!runOp(node(tts::Op::Expand), {&one, &hugeShape}, o));
    Tensor tiles = I({2}, {1, -1}), hugeTiles = I({2}, {huge, huge});
    CHECK(!runOp(node(tts::Op::Tile), {&x, &tiles}, o));
    CHECK(!runOp(node(tts::Op::Tile), {&x, &hugeTiles}, o));
    Tensor hugePads = I({4}, {0, std::numeric_limits<int64_t>::max(), 0, 1});
    pad.mode = "constant";
    CHECK(!runOp(pad, {&x, &hugePads}, o));
    bool threw = false;
    try {
        Tensor::alloc(DType::F32, {-1});
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    CHECK(threw);
}

// Operand types and parameter sizes are checked, and the operators that read an input as float
// dequantize a quantized-weight placeholder (float type, no data) instead of reading null.
TEST(tts_ops_operand_checks) {
    std::vector<Tensor> o, a, b;
    std::vector<int8_t> qv(8192);
    for (size_t i = 0; i < qv.size(); ++i) qv[i] = int8_t(int(i % 251) - 125);
    auto qw = std::make_shared<tts::QuantWeight>();
    qw->q = reinterpret_cast<const uint8_t*>(qv.data());
    qw->dims = {2, 4096};
    qw->scale = {0.03f};
    qw->zeroPoint = {1};
    Tensor ph;
    ph.type = DType::F32;
    ph.dims = qw->dims;
    ph.qweight = qw;
    std::vector<float> fv(qv.size());
    for (size_t i = 0; i < fv.size(); ++i) fv[i] = float(int(qv[i]) - 1) * 0.03f;
    Tensor fl = F({2, 4096}, fv);
    auto same = [&](size_t outputs) {
        for (size_t i = 0; i < outputs; ++i)
            if (a[i].bytes() != b[i].bytes() || std::memcmp(a[i].data, b[i].data, a[i].bytes()) != 0) return false;
        return true;
    };
    CHECK(runOp(node(tts::Op::DynamicQuantize), {&ph}, a, 3) && runOp(node(tts::Op::DynamicQuantize), {&fl}, b, 3) &&
          same(3));
    tts::Node ln = node(tts::Op::LayerNorm);
    ln.axis = -1;
    ln.f0 = 1e-5f;
    Tensor g = F({4096}, std::vector<float>(4096, 1.0f));
    CHECK(runOp(ln, {&ph, &g}, a) && runOp(ln, {&fl, &g}, b) && same(1));
    Tensor sc = F({}, {0.1f});
    CHECK(runOp(node(tts::Op::Quantize), {&ph, &sc}, a) && runOp(node(tts::Op::Quantize), {&fl, &sc}, b) && same(1));
    // An 8-bit scale, a float MatMulInteger operand, per-row or per-column zero points.
    Tensor s8 = typed<int8_t>(DType::I8, {}, {2}), u4 = typed<uint8_t>(DType::U8, {4}, {1, 2, 3, 4});
    CHECK(!runOp(node(tts::Op::Dequantize), {&u4, &s8}, o));
    CHECK(!runOp(node(tts::Op::Quantize), {&fl, &s8}, o));
    Tensor fa = F({2, 3}, {1, 2, 3, 4, 5, 6}), ua = typed<uint8_t>(DType::U8, {2, 3}, {1, 2, 3, 4, 5, 6});
    Tensor ib = typed<int8_t>(DType::I8, {3, 2}, {1, -1, 2, -2, 3, -3}), zp2 = typed<uint8_t>(DType::U8, {2}, {1, 2});
    CHECK(runOp(node(tts::Op::MatMulInteger), {&ua, &ib}, o));
    CHECK(!runOp(node(tts::Op::MatMulInteger), {&fa, &ib}, o));
    CHECK(!runOp(node(tts::Op::MatMulInteger), {&ua, &ib, &zp2}, o));
    CHECK(!runOp(node(tts::Op::MatMulInteger), {&ua, &ib, nullptr, &zp2}, o));
    // Bias and parameter vectors of the wrong length.
    Tensor one = F({}, {1.0f}), p1 = F({1}, {1}), p2 = F({2}, {1, 1}), p3 = F({3}, {1, 2, 3});
    CHECK(runOp(node(tts::Op::MatMulIntegerScaled), {&ua, &ib, nullptr, nullptr, &one, &p2}, o));
    CHECK(!runOp(node(tts::Op::MatMulIntegerScaled), {&ua, &ib, nullptr, nullptr, &one, &p3}, o));
    CHECK(!runOp(node(tts::Op::MatMulIntegerScaled), {&ua, &ib, nullptr, nullptr, &one, &p1}, o));
    Tensor bx = F({1, 2, 3}, {1, 2, 3, 4, 5, 6});
    tts::Node bn = node(tts::Op::BatchNorm);
    bn.f0 = 1e-5f;
    CHECK(runOp(bn, {&bx, &p2, &p2, &p2, &p2}, o));
    CHECK(!runOp(bn, {&bx, &p2, &p1, &p2, &p2}, o));
    tts::Node lc = node(tts::Op::LayerNormChannels);
    lc.f0 = 1e-5f;
    CHECK(runOp(lc, {&bx, &p2, &p2}, o));
    CHECK(!runOp(lc, {&bx, &p2, &p1}, o));
    Tensor cx = F({1, 2, 6}, std::vector<float>(12, 1.0f)), cw = F({2, 1, 3}, std::vector<float>(6, 1.0f));
    tts::Node cv = node(tts::Op::Conv);
    cv.i0 = 2;
    CHECK(runOp(cv, {&cx, &cw, &p2}, o));
    CHECK(!runOp(cv, {&cx, &cw, &p1}, o));
}

// A quantized weight reshaped before its consumer: generic operators see the reshaped shape, the
// GEMM pairs per-tensor scales with the reshaped rows and refuses per-row scales that no longer
// match them. A depthwise weight misaligned in the model file is read through an aligned copy.
TEST(tts_ops_reshaped_quant_weight) {
    std::vector<Tensor> o, r, want;
    std::vector<int8_t> qv(12);
    for (size_t i = 0; i < qv.size(); ++i) qv[i] = int8_t(int(i * 7 % 23) - 11);
    auto per = [&](std::vector<float> scale, Dims dims) {
        auto q = std::make_shared<tts::QuantWeight>();
        q->q = reinterpret_cast<const uint8_t*>(qv.data());
        q->dims = dims;
        q->scale = scale;
        q->zeroPoint.assign(scale.size(), 0);
        Tensor t;
        t.type = DType::F32;
        t.dims = dims;
        t.qweight = q;
        return t;
    };
    std::vector<float> fv;
    for (int8_t v : qv) fv.push_back(float(v) * 0.5f);
    Tensor ph = per({0.5f}, {2, 6}), shape34 = I({2}, {3, 4}), shape431 = I({3}, {4, 3, 1});
    CHECK(runOp(node(tts::Op::Reshape), {&ph, &shape34}, r));
    Tensor f34 = F({3, 4}, fv);
    CHECK(runOp(node(tts::Op::Transpose), {&r[0]}, o) && runOp(node(tts::Op::Transpose), {&f34}, want) &&
          equalF(o[0], {4, 3}, std::vector<float>(want[0].as<float>(), want[0].as<float>() + 12), 0.0f));
    // Pointwise Conv with the per-tensor weight reshaped to [4, 3, 1]: four rows, not two.
    std::vector<float> xv;
    for (int i = 0; i < 15; ++i) xv.push_back(0.25f * float(i % 7) - 0.6f);
    Tensor x = F({1, 3, 5}, xv);
    tts::Node cv = node(tts::Op::Conv);
    cv.i0 = 1;
    CHECK(runOp(node(tts::Op::Reshape), {&ph, &shape431}, r));
    Tensor f431 = F({4, 3, 1}, fv);
    CHECK(runOp(cv, {&x, &r[0]}, o) && runOp(cv, {&x, &f431}, want));
    CHECK(equalF(o[0], {1, 4, 5}, std::vector<float>(want[0].as<float>(), want[0].as<float>() + 20), 1e-6f));
    Tensor rows = per({0.5f, 0.25f}, {2, 6, 1});
    CHECK(runOp(node(tts::Op::Reshape), {&rows, &shape431}, r));
    CHECK(!runOp(cv, {&x, &r[0]}, o));
    // Depthwise weights at an odd address.
    std::vector<uint8_t> raw(6 * 4 + 1);
    std::vector<float> wv = {0.5f, -1, 2, 0.25f, 3, -0.5f};
    std::memcpy(raw.data() + 1, wv.data(), 24);
    Tensor wOdd = Tensor::view(DType::F32, {2, 1, 3}, raw.data() + 1), wAligned = F({2, 1, 3}, wv);
    Tensor dx = F({1, 2, 6}, {1, 2, 3, 4, 5, 6, -1, -2, -3, -4, -5, -6});
    cv.i0 = 2;
    CHECK(runOp(cv, {&dx, &wOdd}, o) && runOp(cv, {&dx, &wAligned}, want));
    CHECK(o[0].bytes() == want[0].bytes() && std::memcmp(o[0].data, want[0].data, o[0].bytes()) == 0);
}

TEST(tts_ops_norms) {
    std::vector<Tensor> o;
    Tensor x = F({2, 4}, {1, 2, 3, 4, -1, 0, 5, 100});
    tts::Node sm = node(tts::Op::Softmax);
    sm.axis = -1;
    CHECK(runOp(sm, {&x}, o));
    for (int r = 0; r < 2; ++r) {
        double mx = -1e30, s = 0;
        for (int c = 0; c < 4; ++c) mx = std::max(mx, double(x.as<float>()[r * 4 + c]));
        for (int c = 0; c < 4; ++c) s += std::exp(x.as<float>()[r * 4 + c] - mx);
        for (int c = 0; c < 4; ++c)
            CHECK(near(o[0].as<float>()[r * 4 + c], float(std::exp(x.as<float>()[r * 4 + c] - mx) / s), 1e-6f));
    }
    Tensor g = F({4}, {1, 2, 0.5f, 1}), b = F({4}, {0, 1, 0, -1});
    tts::Node ln = node(tts::Op::LayerNorm);
    ln.axis = -1;
    ln.f0 = 1e-5f;
    CHECK(runOp(ln, {&x, &g, &b}, o));
    for (int r = 0; r < 2; ++r) {
        double mean = 0, var = 0;
        for (int c = 0; c < 4; ++c) mean += x.as<float>()[r * 4 + c] / 4.0;
        for (int c = 0; c < 4; ++c) var += std::pow(x.as<float>()[r * 4 + c] - mean, 2) / 4.0;
        for (int c = 0; c < 4; ++c) {
            double want = (x.as<float>()[r * 4 + c] - mean) / std::sqrt(var + 1e-5) * g.as<float>()[c] + b.as<float>()[c];
            CHECK(near(o[0].as<float>()[r * 4 + c], float(want), 1e-5f));
        }
    }
    // Channel LayerNorm on [B, C, L] equals Transpose -> LayerNorm -> Transpose.
    Tensor xc = F({1, 4, 2}, {1, -1, 2, 0, 3, 5, 4, 100});
    tts::Node lc = node(tts::Op::LayerNormChannels);
    lc.f0 = 1e-6f;
    CHECK(runOp(lc, {&xc, &g, &b}, o));
    tts::Node tr = node(tts::Op::Transpose);
    tr.ints = {0, 2, 1};
    std::vector<Tensor> t1, t2, t3;
    ln.f0 = 1e-6f;
    CHECK(runOp(tr, {&xc}, t1) && runOp(ln, {&t1[0], &g, &b}, t2) && runOp(tr, {&t2[0]}, t3));
    CHECK(equalF(o[0], {1, 4, 2}, std::vector<float>(t3[0].as<float>(), t3[0].as<float>() + 8), 1e-5f));
    Tensor sc = F({2}, {2, 1}), bi = F({2}, {1, 0}), mu = F({2}, {0.5f, -1}), va = F({2}, {4, 0.25f});
    Tensor bx = F({1, 2, 2}, {1, 2, 3, 4});
    tts::Node bn = node(tts::Op::BatchNorm);
    bn.f0 = 0.0f;
    CHECK(runOp(bn, {&bx, &sc, &bi, &mu, &va}, o) && equalF(o[0], {1, 2, 2}, {1.5f, 2.5f, 8, 10}));
}

namespace {

// Direct 1-D convolution (NCL), the reference of every Conv test.
std::vector<float> convRef(const std::vector<float>& x, int N, int Cin, int L, const std::vector<float>& w, int Cout,
                           int k, int group, int dil, int p0, int p1, int stride, const std::vector<float>& bias,
                           int* LoutOut) {
    int Lout = (L + p0 + p1 - dil * (k - 1) - 1) / stride + 1;
    *LoutOut = Lout;
    int cg = Cin / group, og = Cout / group;
    std::vector<float> y(size_t(N * Cout * Lout));
    for (int n = 0; n < N; ++n)
        for (int co = 0; co < Cout; ++co) {
            int gi = co / og;
            for (int t = 0; t < Lout; ++t) {
                double acc = bias.empty() ? 0.0 : bias[size_t(co)];
                for (int ci = 0; ci < cg; ++ci)
                    for (int j = 0; j < k; ++j) {
                        int s = t * stride + j * dil - p0;
                        if (s < 0 || s >= L) continue;
                        acc += double(w[size_t((co * cg + ci) * k + j)]) * x[size_t((n * Cin + gi * cg + ci) * L + s)];
                    }
                y[size_t((n * Cout + co) * Lout + t)] = float(acc);
            }
        }
    return y;
}

std::vector<float> randomVec(size_t n, std::mt19937& rng, float scale = 1.0f) {
    std::uniform_real_distribution<float> u(-scale, scale);
    std::vector<float> v(n);
    for (float& x : v) x = u(rng);
    return v;
}

}  // namespace

TEST(tts_ops_conv) {
    std::mt19937 rng(3);
    tts::ThreadPool pool(3);
    struct Case {
        int N, Cin, L, Cout, k, group, dil, p0, p1, stride;
        bool bias;
    };
    const Case cases[] = {
        {2, 33, 29, 70, 1, 1, 1, 0, 0, 1, true},    // pointwise, batch 2 in one GEMM
        {1, 16, 40, 24, 7, 1, 1, 3, 3, 1, true},    // k 7 with padding (im2col)
        {1, 8, 50, 12, 5, 1, 2, 4, 4, 1, false},    // dilated
        {2, 12, 37, 12, 7, 12, 4, 0, 0, 1, true},   // depthwise, dilation 4
        {1, 6, 20, 6, 3, 6, 1, 1, 1, 1, false},     // depthwise with padding
        {1, 4, 21, 5, 3, 1, 1, 0, 0, 2, true},      // stride 2
    };
    for (const Case& c : cases) {
        int cg = c.Cin / c.group;
        std::vector<float> x = randomVec(size_t(c.N * c.Cin * c.L), rng), w = randomVec(size_t(c.Cout * cg * c.k), rng);
        std::vector<float> bias = c.bias ? randomVec(size_t(c.Cout), rng) : std::vector<float>();
        int Lout = 0;
        std::vector<float> want = convRef(x, c.N, c.Cin, c.L, w, c.Cout, c.k, c.group, c.dil, c.p0, c.p1, c.stride, bias, &Lout);
        tts::Node cv = node(tts::Op::Conv);
        cv.i0 = c.group;
        cv.ints = {c.dil};
        cv.ints2 = {c.p0, c.p1};
        cv.ints3 = {c.stride};
        Tensor tx = F({c.N, c.Cin, c.L}, x), tw = F({c.Cout, cg, c.k}, w), tb = F({c.Cout}, bias);
        std::vector<Tensor> o;
        for (tts::ThreadPool* p : {static_cast<tts::ThreadPool*>(nullptr), &pool}) {
            CHECK(runOp(cv, {&tx, &tw, c.bias ? &tb : nullptr}, o, 1, nullptr, p));
            CHECK(equalF(o[0], {c.N, c.Cout, Lout}, want, 2e-5f));
        }
    }
    // Int8 weights behind a DequantizeLinear, per output channel (a QuantWeight).
    const int Cin = 20, Cout = 9, L = 13;
    std::vector<int8_t> q(size_t(Cout * Cin));
    for (auto& v : q) v = int8_t(int(rng() % 255) - 127);
    auto qw = std::make_shared<tts::QuantWeight>();
    qw->q = reinterpret_cast<const uint8_t*>(q.data());
    qw->dims = {Cout, Cin, 1};
    qw->axis = 0;
    std::vector<float> wf(q.size());
    for (int co = 0; co < Cout; ++co) {
        qw->scale.push_back(0.01f * float(co + 1));
        qw->zeroPoint.push_back(0);
        for (int ci = 0; ci < Cin; ++ci) wf[size_t(co * Cin + ci)] = float(q[size_t(co * Cin + ci)]) * qw->scale.back();
    }
    Tensor tq;
    tq.type = DType::F32;
    tq.dims = qw->dims;
    tq.qweight = qw;
    std::vector<float> x = randomVec(size_t(Cin * L), rng);
    int Lout = 0;
    std::vector<float> want = convRef(x, 1, Cin, L, wf, Cout, 1, 1, 1, 0, 0, 1, {}, &Lout);
    Tensor tx = F({1, Cin, L}, x);
    std::vector<Tensor> o;
    tts::Node cv = node(tts::Op::Conv);
    cv.i0 = 1;
    CHECK(runOp(cv, {&tx, &tq}, o) && equalF(o[0], {1, Cout, L}, want, 2e-5f));
}

TEST(tts_ops_matmul_gemm) {
    std::mt19937 rng(5);
    std::vector<Tensor> o;
    // Batched MatMul with broadcasting: [2, 1, 3, 4] x [3, 4, 5] -> [2, 3, 3, 5]
    std::vector<float> a = randomVec(2 * 3 * 4, rng), b = randomVec(3 * 4 * 5, rng);
    Tensor ta = F({2, 1, 3, 4}, a), tb = F({3, 4, 5}, b);
    CHECK(runOp(node(tts::Op::MatMul), {&ta, &tb}, o));
    CHECK(o[0].dims == Dims({2, 3, 3, 5}));
    bool ok = true;
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 3; ++j)
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 5; ++c) {
                    double s = 0;
                    for (int k = 0; k < 4; ++k) s += double(a[size_t((i * 3 + r) * 4 + k)]) * b[size_t((j * 4 + k) * 5 + c)];
                    ok = ok && near(o[0].as<float>()[((i * 3 + j) * 3 + r) * 5 + c], float(s), 1e-5f);
                }
    CHECK(ok);
    // Shared weight: [2, 7, 6] x [6, 3] (one tall GEMM).
    std::vector<float> a2 = randomVec(2 * 7 * 6, rng), w2 = randomVec(6 * 3, rng);
    Tensor ta2 = F({2, 7, 6}, a2), tw2 = F({6, 3}, w2);
    CHECK(runOp(node(tts::Op::MatMul), {&ta2, &tw2}, o) && o[0].dims == Dims({2, 7, 3}));
    ok = true;
    for (int r = 0; r < 14; ++r)
        for (int c = 0; c < 3; ++c) {
            double s = 0;
            for (int k = 0; k < 6; ++k) s += double(a2[size_t(r * 6 + k)]) * w2[size_t(k * 3 + c)];
            ok = ok && near(o[0].as<float>()[r * 3 + c], float(s), 1e-5f);
        }
    CHECK(ok);
    // Gemm with transB, alpha, beta and a broadcast C.
    Tensor ga = F({2, 3}, {1, 2, 3, 4, 5, 6}), gb = F({2, 3}, {1, 0, -1, 2, 1, 0}), gc = F({2}, {10, 20});
    tts::Node gm = node(tts::Op::Gemm);
    gm.i1 = 1;
    gm.f0 = 2.0f;
    gm.f1 = 0.5f;
    CHECK(runOp(gm, {&ga, &gb, &gc}, o) && equalF(o[0], {2, 2}, {1, 18, 1, 36}));
}

// The element-wise operators split over threads compute exactly what they compute on one thread.
TEST(tts_ops_parallel_equals_serial) {
    std::mt19937 rng(11);
    tts::ThreadPool pool(3);
    auto same = [](const Tensor& a, const Tensor& b) {
        return a.type == b.type && a.dims == b.dims && std::memcmp(a.data, b.data, a.bytes()) == 0;
    };
    Tensor big = F({2, 64, 1001}, randomVec(2 * 64 * 1001, rng, 4.0f));
    Tensor big2 = F({2, 64, 1001}, randomVec(2 * 64 * 1001, rng, 4.0f));
    Tensor perChannel = F({1, 64, 1}, randomVec(64, rng));
    Tensor perTime = F({2, 1, 1001}, randomVec(2 * 1001, rng));
    Tensor gamma = F({64}, randomVec(64, rng)), beta = F({64}, randomVec(64, rng));
    // Pad (constant and edge, rows outside on the channel axis too) and the quantized MatMul.
    Tensor pads = I({6}, {0, 1, 3, 0, 2, 5});
    std::vector<uint8_t> qa(300 * 64);
    std::vector<int8_t> qb(64 * 128);
    for (auto& v : qa) v = uint8_t(rng() % 256);
    for (auto& v : qb) v = int8_t(int(rng() % 256) - 128);
    Tensor ta = typed<uint8_t>(DType::U8, {300, 64}, qa), tb = typed<int8_t>(DType::I8, {64, 128}, qb);
    Tensor azp = typed<uint8_t>(DType::U8, {}, {97}), qscale = F({}, {0.0125f}), qbias = F({128}, randomVec(128, rng));
    struct Case {
        tts::Node n;
        std::vector<const Tensor*> in;
        size_t outputs;
    };
    auto with = [](tts::Op op, float f0, const char* mode) {
        tts::Node n = node(op);
        n.f0 = f0;
        n.mode = mode;
        return n;
    };
    std::vector<Case> cases = {
        {node(tts::Op::Add), {&big, &big2}, 1},
        {node(tts::Op::Mul), {&big, &perChannel}, 1},
        {node(tts::Op::Sub), {&perTime, &big}, 1},
        {node(tts::Op::Div), {&big, &perChannel}, 1},
        {node(tts::Op::Gelu), {&big}, 1},
        {node(tts::Op::Tanh), {&big}, 1},
        {with(tts::Op::LayerNormChannels, 1e-6f, ""), {&big, &gamma, &beta}, 1},
        {node(tts::Op::DynamicQuantize), {&big}, 3},
        {with(tts::Op::Pad, 0.0f, "constant"), {&big, &pads}, 1},
        {with(tts::Op::Pad, 0.0f, "edge"), {&big, &pads}, 1},
        {node(tts::Op::MatMulIntegerScaled), {&ta, &tb, &azp, nullptr, &qscale, &qbias}, 1},
    };
    for (const Case& c : cases) {
        std::vector<Tensor> serial, parallel;
        CHECK(runOp(c.n, c.in, serial, c.outputs));
        CHECK(runOp(c.n, c.in, parallel, c.outputs, nullptr, &pool));
        bool ok = serial.size() == parallel.size();
        for (size_t i = 0; ok && i < serial.size(); ++i) ok = same(serial[i], parallel[i]);
        if (!ok) std::fprintf(stderr, "  %s %s differs on threads\n", tts::opName(c.n.op), c.n.mode.c_str());
        CHECK(ok);
    }
}

// Large tensor blocks are reused after they are freed, until the cache is trimmed.
TEST(tts_buffer_cache) {
    tts::trimBufferCache();
    const void* first;
    {
        Tensor a = Tensor::alloc(DType::F32, {3, 10000});
        first = a.data;
        Tensor small = Tensor::alloc(DType::F32, {100});
    }
    CHECK(tts::bufferCacheBytes() >= 120000 && tts::bufferCacheBytes() < 140000);   // the small one is not kept
    {
        Tensor b = Tensor::alloc(DType::U8, {119000});   // same size class, other type
        CHECK(b.data == first);
        CHECK_EQ(int(tts::bufferCacheBytes()), 0);
        Tensor c = Tensor::alloc(DType::F32, {3, 10000});
        CHECK(c.data != first);
    }
    tts::trimBufferCache();
    CHECK_EQ(int(tts::bufferCacheBytes()), 0);
}

TEST(tts_ops_quantization) {
    std::vector<Tensor> o;
    // DynamicQuantizeLinear, the example of the ONNX operator documentation.
    Tensor x = F({6}, {0, 2, -3, -2.5f, 1.34f, 0.5f});
    CHECK(runOp(node(tts::Op::DynamicQuantize), {&x}, o, 3));
    CHECK(equalT<uint8_t>(o[0], DType::U8, {6}, {153, 255, 0, 26, 221, 179}));
    CHECK(near(o[1].scalarFloat(), 0.0196078438f, 1e-7f));
    CHECK_EQ(int(o[2].scalarFloat()), 153);
    // All-positive input: the range still includes 0 (zero point 0).
    Tensor xp = F({3}, {1, 2.5f, 5.1f});
    CHECK(runOp(node(tts::Op::DynamicQuantize), {&xp}, o, 3));
    CHECK(equalT<uint8_t>(o[0], DType::U8, {3}, {50, 125, 255}));
    CHECK_EQ(int(o[2].scalarFloat()), 0);
    // An infinite bound of the range (a damaged model) is refused: -inf would make the zero point
    // NaN, cast to an int. NaN elements do not count in the range.
    for (float bad : {-INFINITY, INFINITY}) {
        for (int n : {3, 8, 17}) {
            std::vector<float> v(static_cast<size_t>(n), 0.5f);
            v[static_cast<size_t>(n / 2)] = bad;
            Tensor xv = F({n}, v);
            CHECK(!runOp(node(tts::Op::DynamicQuantize), {&xv}, o, 3));
        }
    }
    // The vectorized range scan equals the scalar std::min/std::max loop, NaN, -0 and the largest
    // finite values included, for lengths around the vector width.
    {
        std::mt19937 rr(11);
        const float specials[] = {std::nanf(""), -0.0f, 0.0f, 3e38f, -3e38f};
        bool same = true;
        for (int n : {0, 1, 3, 7, 8, 9, 15, 16, 17, 31, 33, 1000}) {
            for (int round = 0; round < 20; ++round) {
                std::vector<float> v(static_cast<size_t>(n));
                for (float& f : v) {
                    int pick = int(rr() % 16);
                    f = pick < 5 && round % 4 != 0 ? specials[pick] : std::normal_distribution<float>(0.0f, 2.0f)(rr);
                    if (round == 1) f = -0.0f;
                    if (round == 2) f = std::nanf("");
                }
                float mn = 0.0f, mx = 0.0f;
                for (float f : v) {
                    mn = std::min(mn, f);
                    mx = std::max(mx, f);
                }
                float scale = mx == mn ? 1.0f : (mx - mn) / 255.0f;
                int zp = int(std::nearbyint(std::clamp(0.0f - mn / scale, 0.0f, 255.0f)));
                Tensor xv = F({n}, v);
                same = same && runOp(node(tts::Op::DynamicQuantize), {&xv}, o, 3);
                float got = o[1].scalarFloat();
                same = same && std::memcmp(&got, &scale, 4) == 0 && int(o[2].scalarFloat()) == zp;
            }
        }
        CHECK(same);
    }
    // QuantizeLinear / DequantizeLinear, per tensor (round half to even, saturation) and per axis.
    Tensor qx = F({6}, {0, 1.5f, 2.5f, -1.5f, 1000, -1000});
    Tensor s = F({}, {1.0f});
    Tensor zp = typed<uint8_t>(DType::U8, {}, {128});
    tts::Node qn = node(tts::Op::Quantize);
    qn.axis = 1;
    CHECK(runOp(qn, {&qx, &s, &zp}, o) && equalT<uint8_t>(o[0], DType::U8, {6}, {128, 130, 130, 126, 255, 0}));
    Tensor q8 = typed<int8_t>(DType::I8, {2, 2}, {-128, 127, 3, -4});
    Tensor sa = F({2}, {0.5f, 2.0f});
    tts::Node dq = node(tts::Op::Dequantize);
    dq.axis = 0;
    CHECK(runOp(dq, {&q8, &sa}, o) && equalF(o[0], {2, 2}, {-64, 63.5f, 6, -8}));
    Tensor u8 = typed<uint8_t>(DType::U8, {3}, {0, 128, 255});
    dq.axis = 1;
    CHECK(runOp(dq, {&u8, &s, &zp}, o) && equalF(o[0], {3}, {-128, 0, 127}));
    // The fused QuantizeLinear -> DequantizeLinear equals the two operators, bit for bit, also
    // split over threads and with a tail that is not a whole vector.
    {
        std::mt19937 rq(5);
        std::vector<float> big(70001);
        for (float& v : big) v = std::normal_distribution<float>(0.0f, 3.0f)(rq);
        Tensor xb = F({70001}, big), sq = F({}, {0.0371f}), zq = typed<uint8_t>(DType::U8, {}, {117});
        std::vector<Tensor> q1, q2, fused;
        tts::ThreadPool pool(2);
        CHECK(runOp(node(tts::Op::Quantize), {&xb, &sq, &zq}, q1, 1, nullptr, &pool));
        CHECK(runOp(node(tts::Op::Dequantize), {&q1[0], &sq, &zq}, q2));
        CHECK(runOp(node(tts::Op::QuantDequant), {&xb, &sq, &zq}, fused, 1, nullptr, &pool));
        CHECK(fused[0].count() == 70001 && std::memcmp(fused[0].data, q2[0].data, 70001 * 4) == 0);
        bool same = true;
        for (int i = 0; i < 70001; i += 997) {
            float r = std::nearbyint(big[size_t(i)] / 0.0371f) + 117.0f;
            same = same && q1[0].as<uint8_t>()[i] == uint8_t(std::min(255.0f, std::max(0.0f, r)));
        }
        CHECK(same);
    }
    // MatMulInteger (u8 activations with a zero point, s8 weights) and the fused scaled form.
    std::mt19937 rng(9);
    const int M = 5, Kd = 37, N = 11;
    std::vector<uint8_t> A(size_t(M * Kd));
    std::vector<int8_t> B(size_t(Kd * N));
    for (auto& v : A) v = uint8_t(rng() % 256);
    for (auto& v : B) v = int8_t(int(rng() % 256) - 128);
    Tensor ta = typed<uint8_t>(DType::U8, {M, Kd}, A), tb = typed<int8_t>(DType::I8, {Kd, N}, B);
    Tensor azp = typed<uint8_t>(DType::U8, {}, {131});
    CHECK(runOp(node(tts::Op::MatMulInteger), {&ta, &tb, &azp, nullptr}, o));
    std::vector<int32_t> want(size_t(M * N));
    for (int i = 0; i < M; ++i)
        for (int j = 0; j < N; ++j) {
            int32_t acc = 0;
            for (int k = 0; k < Kd; ++k) acc += (int32_t(A[size_t(i * Kd + k)]) - 131) * int32_t(B[size_t(k * N + j)]);
            want[size_t(i * N + j)] = acc;
        }
    CHECK(equalT<int32_t>(o[0], DType::I32, {M, N}, want));
    Tensor scale = F({}, {0.25f});
    std::vector<float> biasV = randomVec(size_t(N), rng);
    Tensor bias = F({N}, biasV);
    CHECK(runOp(node(tts::Op::MatMulIntegerScaled), {&ta, &tb, &azp, nullptr, &scale, &bias}, o));
    std::vector<float> wantF;
    for (int i = 0; i < M * N; ++i) wantF.push_back(biasV[size_t(i % N)] + float(want[size_t(i)]) * 0.25f);
    CHECK(equalF(o[0], {M, N}, wantF, 1e-6f));
}

// ------------------------------------------------------------------------------------------------
// Kernels of every instruction set this CPU runs, against scalar references
// ------------------------------------------------------------------------------------------------
namespace {

// f32 GEMM in the plain loop order (row panel, k block of 512, column panel) with the table's own
// packing and tiles: the arithmetic sgemm must reproduce bit for bit.
std::vector<float> sgemmRef(const tts::kern::Table& k, int M, int N, int K, const tts::GemmA& a, const float* B) {
    const int mr = k.mr, nr = k.nr, panels = (N + nr - 1) / nr;
    tts::Buffer bbuf(size_t(panels) * size_t(K) * size_t(nr) * 4), abuf(size_t(mr) * size_t(std::min(K, 512)) * 4);
    float* Bp = static_cast<float*>(bbuf.data);
    float* Ap = static_cast<float*>(abuf.data);
    for (int p = 0; p < panels; ++p) k.packBf32(B + p * nr, N, K, std::min(nr, N - p * nr), Bp + size_t(p) * K * nr);
    std::vector<float> C(size_t(M) * size_t(N));
    for (int i0 = 0; i0 < M; i0 += mr) {
        int rows = std::min(mr, M - i0);
        for (int kb = 0; kb < K; kb += 512) {
            int kc = std::min(512, K - kb);
            std::memset(Ap, 0, size_t(mr) * size_t(kc) * 4);
            if (a.i8) k.packAi8(a.i8 + i0 * a.ld + kb, a.ld, rows, kc, a.scale + i0, a.zp ? a.zp + i0 : nullptr, Ap);
            else k.packAf32(a.f32 + i0 * a.ld + kb, a.ld, rows, kc, Ap);
            for (int p = 0; p < panels; ++p)
                k.sgemmTile(kc, Ap, Bp + (size_t(p) * K + size_t(kb)) * nr, C.data() + size_t(i0) * N + p * nr, N, rows,
                            std::min(nr, N - p * nr), kb > 0);
        }
    }
    return C;
}

}  // namespace

TEST(tts_kernels_all_levels) {
    std::vector<int> levels = tablesToCheck();
    CHECK(!levels.empty());
    std::string names;
    for (int l : levels) names += std::string(" ") + tableToCheck(l)->name;
    std::fprintf(stderr, "  levels run by this CPU:%s (active: %s)\n", names.c_str(), tts::activeArch());
    std::mt19937 rng(11);
    tts::ThreadPool pool(3);
    for (int level : levels) {
        const tts::kern::Table& k = *tableToCheck(level);
        // f32 GEMM, float and int8-weight A, odd sizes, K across the blocking depth, column blocks.
        const int shapes[][3] = {{1, 1, 1}, {7, 13, 5}, {37, 61, 70}, {6, 16, 600}, {50, 3, 1100}, {13, 100, 33}};
        for (auto& sh : shapes) {
            int M = sh[0], N = sh[1], Kd = sh[2];
            std::vector<float> A = randomVec(size_t(M * Kd), rng), B = randomVec(size_t(Kd * N), rng);
            std::vector<int8_t> Aq(size_t(M * Kd));
            std::vector<float> sc(static_cast<size_t>(M));
            std::vector<int32_t> zp(static_cast<size_t>(M));
            for (int i = 0; i < M; ++i) {
                sc[size_t(i)] = 0.01f + 0.001f * float(i);
                zp[size_t(i)] = i % 3 - 1;
            }
            for (auto& v : Aq) v = int8_t(int(rng() % 255) - 127);
            for (int variant = 0; variant < 2; ++variant) {
                tts::GemmA ga;
                ga.ld = Kd;
                if (variant % 3 == 0) ga.f32 = A.data();
                else {
                    ga.i8 = Aq.data();
                    ga.scale = sc.data();
                    ga.zp = zp.data();
                }
                tts::GemmB gb;
                gb.f32 = B.data();
                gb.ld = N;
                std::vector<float> C(size_t(M * N), -7.0f);
                tts::sgemm(k, level % 2 ? &pool : nullptr, M, N, Kd, ga, gb, C.data(), N);
                bool ok = true;
                for (int i = 0; i < M && ok; ++i)
                    for (int j = 0; j < N; ++j) {
                        double s = 0, mag = 0;
                        for (int kk = 0; kk < Kd; ++kk) {
                            double a = variant == 0 ? A[size_t(i * Kd + kk)]
                                                    : (double(Aq[size_t(i * Kd + kk)]) - zp[size_t(i)]) * sc[size_t(i)];
                            s += a * B[size_t(kk * N + j)];
                            mag += std::fabs(a * B[size_t(kk * N + j)]);
                        }
                        if (std::fabs(C[size_t(i * N + j)] - s) > 1e-6 * mag + 1e-7) {
                            std::fprintf(stderr, "  %s sgemm %dx%dx%d v%d [%d,%d] %g vs %g\n", k.name, M, N, Kd, variant,
                                         i, j, C[size_t(i * N + j)], s);
                            ok = false;
                            break;
                        }
                    }
                CHECK(ok);
            }
        }
        // B in two column blocks (two batch items of a pointwise convolution).
        {
            const int M = 9, Kd = 17, cols = 11;
            std::vector<float> A = randomVec(size_t(M * Kd), rng), B = randomVec(size_t(2 * Kd * cols), rng);
            tts::GemmA ga;
            ga.f32 = A.data();
            ga.ld = Kd;
            tts::GemmB gb;
            gb.f32 = B.data();
            gb.ld = cols;
            gb.blocks = 2;
            gb.blockCols = cols;
            gb.blockStride = Kd * cols;
            std::vector<float> C(size_t(M * 2 * cols));
            tts::sgemm(k, nullptr, M, 2 * cols, Kd, ga, gb, C.data(), 2 * cols);
            bool ok = true;
            for (int i = 0; i < M; ++i)
                for (int j = 0; j < 2 * cols; ++j) {
                    double s = 0;
                    for (int kk = 0; kk < Kd; ++kk)
                        s += double(A[size_t(i * Kd + kk)]) * B[size_t((j / cols) * Kd * cols + kk * cols + j % cols)];
                    ok = ok && std::fabs(C[size_t(i * 2 * cols + j)] - s) < 1e-4;
                }
            CHECK(ok);
        }
        // Deep K (several k blocks): the bits of the plain loop order, whatever the blocking of the
        // output and the threads.
        for (int Kd : {513, 1536, 2048}) {
            const int M = 37, cols = 23, N = 2 * cols;
            std::vector<float> A = randomVec(size_t(M * Kd), rng), B = randomVec(size_t(Kd * N), rng);
            std::vector<int8_t> Aq(size_t(M * Kd));
            for (auto& v : Aq) v = int8_t(int(rng() % 255) - 127);
            std::vector<float> sc(size_t(M), 0.02f);
            std::vector<float> Bb(B.size());   // the same B as two column blocks of K x cols
            for (int kk = 0; kk < Kd; ++kk)
                for (int j = 0; j < N; ++j)
                    Bb[size_t((j / cols) * Kd * cols + kk * cols + j % cols)] = B[size_t(kk * N + j)];
            for (int variant = 0; variant < 2; ++variant) {
                tts::GemmA ga;
                ga.ld = Kd;
                if (variant == 0) ga.f32 = A.data();
                else {
                    ga.i8 = Aq.data();
                    ga.scale = sc.data();
                }
                std::vector<float> want = sgemmRef(k, M, N, Kd, ga, B.data());
                for (tts::ThreadPool* p : {static_cast<tts::ThreadPool*>(nullptr), &pool}) {
                    tts::GemmB gb;
                    gb.f32 = B.data();
                    gb.ld = N;
                    std::vector<float> C(size_t(M * N));
                    tts::sgemm(k, p, M, N, Kd, ga, gb, C.data(), N);
                    CHECK(std::memcmp(C.data(), want.data(), C.size() * 4) == 0);
                    gb.f32 = Bb.data();
                    gb.ld = cols;
                    gb.blocks = 2;
                    gb.blockCols = cols;
                    gb.blockStride = Kd * cols;
                    std::vector<float> Cb(size_t(M * N));
                    tts::sgemm(k, p, M, N, Kd, ga, gb, Cb.data(), cols, M * cols);
                    bool same = true;
                    for (int i = 0; i < M; ++i)
                        for (int j = 0; j < N; ++j)
                            same = same && std::memcmp(&Cb[size_t((j / cols) * M * cols + i * cols + j % cols)],
                                                       &want[size_t(i * N + j)], 4) == 0;
                    CHECK(same);
                }
            }
        }
        // Integer GEMM: exact, both operand orders, with zero points on the unsigned side.
        const int ishapes[][3] = {{1, 1, 1}, {5, 9, 3}, {13, 40, 256}, {33, 17, 1025}, {4, 64, 7}};
        for (auto& sh : ishapes) {
            int M = sh[0], N = sh[1], Kd = sh[2];
            for (int order = 0; order < 2; ++order) {
                bool aU = order == 0;
                std::vector<uint8_t> A(size_t(M * Kd)), B(size_t(Kd * N));
                for (auto& v : A) v = uint8_t(rng() % 256);
                for (auto& v : B) v = uint8_t(rng() % 256);
                int azp = aU ? int(rng() % 256) : 0, bzp = aU ? 0 : int(rng() % 256);
                std::vector<int32_t> C(size_t(M * N));
                CHECK(tts::igemm(k, level % 2 ? nullptr : &pool, M, N, Kd, A.data(), Kd, aU, azp, B.data(), N, !aU,
                                 bzp, C.data(), N));
                bool ok = true;
                for (int i = 0; i < M; ++i)
                    for (int j = 0; j < N; ++j) {
                        int64_t s = 0;
                        for (int kk = 0; kk < Kd; ++kk) {
                            int a = aU ? int(A[size_t(i * Kd + kk)]) - azp : int(int8_t(A[size_t(i * Kd + kk)]));
                            int b = aU ? int(int8_t(B[size_t(kk * N + j)])) : int(B[size_t(kk * N + j)]) - bzp;
                            s += a * b;
                        }
                        ok = ok && C[size_t(i * N + j)] == s;
                    }
                if (!ok) std::fprintf(stderr, "  %s igemm %dx%dx%d order %d wrong\n", k.name, M, N, Kd, order);
                CHECK(ok);
                // The same with the sums of the signed operand given (constant weights).
                std::vector<int32_t> sums(size_t(aU ? N : M), 0), C2(size_t(M * N));
                for (int i = 0; i < M; ++i)
                    for (int j = 0; j < N; ++j)
                        for (int kk = 0; kk < Kd && (aU ? i == 0 : j == 0); ++kk)
                            sums[size_t(aU ? j : i)] +=
                                aU ? int8_t(B[size_t(kk * N + j)]) : int8_t(A[size_t(i * Kd + kk)]);
                CHECK(tts::igemm(k, level % 2 ? nullptr : &pool, M, N, Kd, A.data(), Kd, aU, azp, B.data(), N, !aU,
                                 bzp, C2.data(), N, sums.data()));
                CHECK(C2 == C);
            }
        }
        // Element-wise functions.
        std::vector<float> xs;
        for (float v = -9.0f; v <= 9.0f; v += 0.00731f) xs.push_back(v);
        xs.push_back(0.0f);
        xs.push_back(-0.0f);
        xs.push_back(0.927734375f);
        std::vector<float> y(xs.size());
        double eErf = 0, eGelu = 0, eExp = 0, eTanh = 0;
        k.erf(xs.data(), y.data(), xs.size());
        for (size_t i = 0; i < xs.size(); ++i) eErf = std::max(eErf, std::fabs(y[i] - std::erf(double(xs[i]))));
        k.gelu(xs.data(), y.data(), xs.size());
        for (size_t i = 0; i < xs.size(); ++i) {
            double x = xs[i], want = 0.5 * x * (1.0 + std::erf(x / std::sqrt(2.0)));
            eGelu = std::max(eGelu, std::fabs(y[i] - want) / std::max(1.0, std::fabs(want)));
        }
        k.exp(xs.data(), y.data(), xs.size());
        for (size_t i = 0; i < xs.size(); ++i) {
            double want = std::exp(double(xs[i]));
            eExp = std::max(eExp, std::fabs(y[i] - want) / want);
        }
        k.tanh(xs.data(), y.data(), xs.size());
        for (size_t i = 0; i < xs.size(); ++i) eTanh = std::max(eTanh, std::fabs(y[i] - std::tanh(double(xs[i]))));
        CHECK(eErf < 2.5e-7);
        CHECK(eGelu < 1e-6);
        CHECK(eExp < 3e-7);
        CHECK(eTanh < 1e-6);
        std::fprintf(stderr, "  %-8s erf %.2g  gelu %.2g  exp(rel) %.2g  tanh %.2g\n", k.name, eErf, eGelu, eExp, eTanh);
        // Depthwise convolution, LayerNorm rows, softmax rows.
        std::vector<float> in = randomVec(200, rng), w = randomVec(7, rng), out(200);
        int n = 200 - 6 * 4;
        k.dwconv(in.data(), w.data(), 7, 4, 0.25f, out.data(), n);
        bool ok = true;
        for (int t = 0; t < n; ++t) {
            double s = 0.25;
            for (int j = 0; j < 7; ++j) s += double(w[size_t(j)]) * in[size_t(t + j * 4)];
            ok = ok && std::fabs(out[size_t(t)] - s) < 1e-5;
        }
        CHECK(ok);
        std::vector<float> rows = randomVec(3 * 37, rng, 4.0f), g = randomVec(37, rng), b = randomVec(37, rng), ln(3 * 37);
        k.layerNormRows(rows.data(), 37, 3, 37, g.data(), b.data(), 1e-6f, ln.data());
        ok = true;
        for (int r = 0; r < 3; ++r) {
            double mean = 0, var = 0;
            for (int c = 0; c < 37; ++c) mean += rows[size_t(r * 37 + c)] / 37.0;
            for (int c = 0; c < 37; ++c) var += std::pow(rows[size_t(r * 37 + c)] - mean, 2) / 37.0;
            for (int c = 0; c < 37; ++c) {
                double want = (rows[size_t(r * 37 + c)] - mean) / std::sqrt(var + 1e-6) * g[size_t(c)] + b[size_t(c)];
                ok = ok && std::fabs(ln[size_t(r * 37 + c)] - want) < 1e-5;
            }
        }
        CHECK(ok);
        std::vector<float> sm = rows;
        k.softmaxRows(sm.data(), 37, 3, 37);
        ok = true;
        for (int r = 0; r < 3; ++r) {
            double mx = -1e30, s = 0;
            for (int c = 0; c < 37; ++c) mx = std::max(mx, double(rows[size_t(r * 37 + c)]));
            for (int c = 0; c < 37; ++c) s += std::exp(rows[size_t(r * 37 + c)] - mx);
            for (int c = 0; c < 37; ++c)
                ok = ok && std::fabs(sm[size_t(r * 37 + c)] - std::exp(rows[size_t(r * 37 + c)] - mx) / s) < 1e-6;
        }
        CHECK(ok);
    }
}

TEST(tts_arch_cap) {
    CHECK(tts::setArchCap("sse2"));
    CHECK_EQ(std::string(tts::activeArch()), std::string("sse2"));
    CHECK(tts::setArchCap("scalar"));
    CHECK_EQ(std::string(tts::activeArch()), std::string("scalar"));
    CHECK(!tts::setArchCap("mmx"));
    CHECK(tts::setArchCap("auto"));
    std::string best = tts::kern::levelName(levelsRun().back());
    CHECK_EQ(std::string(tts::activeArch()), best);
}

// ------------------------------------------------------------------------------------------------
// Threads
// ------------------------------------------------------------------------------------------------
TEST(tts_thread_pool) {
    for (int threads : {1, 2, 4}) {
        tts::ThreadPool pool(threads);
        CHECK_EQ(pool.size(), threads);
        for (int round = 0; round < 50; ++round) {
            std::vector<int> hits(size_t(37 + round), 0);
            pool.run(int(hits.size()), [&](int i) { hits[size_t(i)]++; });
            CHECK(std::all_of(hits.begin(), hits.end(), [](int h) { return h == 1; }));
        }
        // An exception in an item (on a helper or on the caller) reaches the caller of run() once
        // the helpers are out of the job, instead of terminating the program; the pool goes on.
        for (int throwAt : {0, 5, 63}) {
            bool caught = false;
            try {
                pool.run(64, [&](int i) {
                    if (i == throwAt) throw std::bad_alloc();
                });
            } catch (const std::bad_alloc&) {
                caught = true;
            }
            CHECK(caught);
            std::atomic<int> count{0};
            pool.run(64, [&](int) { ++count; });
            CHECK_EQ(count.load(), 64);
        }
    }
#ifndef _WIN32
    // Linux: a lowered thread runs at the process's nice + 5 (at most 19), and so does one started
    // by an already lowered thread.
    auto threadNice = [] { return getpriority(PRIO_PROCESS, id_t(syscall(SYS_gettid))); };
    int base = getpriority(PRIO_PROCESS, id_t(getpid()));
    int lowered = 0, nested = 0;
    std::thread([&] {
        tts::lowerThreadPriority();
        lowered = threadNice();
        std::thread([&] {
            tts::lowerThreadPriority();
            nested = threadNice();
        }).join();
    }).join();
    CHECK_EQ(lowered, std::min(base + 5, 19));
    CHECK_EQ(nested, std::min(base + 5, 19));
    CHECK_EQ(getpriority(PRIO_PROCESS, id_t(getpid())), base);
#endif
}

// ------------------------------------------------------------------------------------------------
// Text front end
// ------------------------------------------------------------------------------------------------
TEST(tts_text_nfkd) {
    using tts::text::nfkd;
    CHECK(nfkd(U"é") == U"é");
    CHECK(nfkd(U"ﬁ") == U"fi");
    CHECK(nfkd(U"½") == U"1⁄2");
    CHECK(nfkd(U"한") == U"한");            // Hangul syllable
    CHECK(nfkd(U"が") == U"が");                  // voiced kana
    CHECK(nfkd(U"ｶﾞ") == U"ガ");            // half-width katakana
    CHECK(nfkd(U"Ｆ３") == U"F3");                      // full-width Latin
    CHECK(nfkd(U"й") == U"й");                  // Cyrillic short i
    CHECK(nfkd(U"ậ") == U"ậ");          // canonical reordering
    CHECK(nfkd(U"ậ") == U"ậ");                 // a with dot below and circumflex
    CHECK(nfkd(U"ﻻ") == U"لا");                  // Arabic ligature lam-alef
    CHECK(nfkd(U"\U0001D400") == U"A");                         // outside the BMP
}

TEST(tts_text_preprocess) {
    using tts::text::preprocess;
    CHECK(preprocess("Knight to f3 — a “classical” move!", "en") ==
          U"<en>Knight to f3 - a \"classical\" move!</en>");
    CHECK(preprocess("Castles kingside_now   ,  then  #4 [e.g., later]", "en") ==
          U"<en>Castles kingside now , then 4 for example, later.</en>");
    CHECK(preprocess("Le cavalier va en f3 ; échec à la dame. Énorme !", "fr") ==
          U"<fr>Le cavalier va en f3; échec à la dame. Énorme!</fr>");
    CHECK(preprocess("الحصان؟", "ar") ==
          U"<ar>الحصان؟.</ar>");
    CHECK(preprocess("", "en") == U"<en>.</en>");
    CHECK(preprocess("  \t", "en") == U"<en>.</en>");
    CHECK(preprocess("He said \"\"hi\"\"", "en") == U"<en>He said \"hi\"</en>");
    CHECK(preprocess("a@b \U0001F600 ♥ x", "en") == U"<en>a at b x.</en>");
    CHECK(tts::text::modelLanguage("uk"));
    CHECK(!tts::text::modelLanguage("zh"));
}

TEST(tts_text_frontend_matches_reference) {
    tts::Synthesizer* s = model();
    if (!s) SKIP("no model files in " + modelDir());
    Dump d;
    CHECK(loadDump("tests/data/tts/frontend.bin", d));
    int cases = 0;
    for (int i = 0; d.has("case" + std::to_string(i) + ".text"); ++i, ++cases) {
        std::string base = "case" + std::to_string(i);
        auto str = [&](const std::string& n) {
            std::string r;
            for (int64_t v : d[n].toInts()) r.push_back(char(v));
            return r;
        };
        std::string text = str(base + ".text"), lang = str(base + ".lang");
        std::vector<int64_t> ids = tts::text::indices(tts::text::preprocess(text, lang), s->engine()->indexer());
        bool same = ids == d[base + ".ids"].toInts();
        if (!same) std::fprintf(stderr, "  front end case %d (%s) differs\n", i, lang.c_str());
        CHECK(same);
    }
    CHECK(cases >= 10);
}

TEST(tts_text_chunks) {
    using tts::text::chunk;
    auto c = chunk("Good move. Now the knight! Is it safe? Yes.", 300);
    CHECK_EQ(c.size(), size_t(1));
    c = chunk("Good move. Now the knight! Is it safe? Yes.", 20);
    CHECK(c == std::vector<std::string>({"Good move.", "Now the knight!", "Is it safe? Yes."}));
    c = chunk("Mr. Smith played e4. Dr. Who replied.", 22);
    CHECK(c == std::vector<std::string>({"Mr. Smith played e4.", "Dr. Who replied."}));
    c = chunk("First paragraph.\n\n  Second one.", 300);
    CHECK(c == std::vector<std::string>({"First paragraph.", "Second one."}));
    c = chunk("良い手です。次は？はい。", 7);
    CHECK(c == std::vector<std::string>({"\u826F\u3044\u624B\u3067\u3059\u3002", "\u6B21\u306F\uFF1F\u306F\u3044\u3002"}));
    c = chunk("The evaluation is +1.5 for White.", 300);
    CHECK_EQ(c.size(), size_t(1));
    // A sentence longer than the limit is cut at a comma, then at spaces; nothing is lost.
    std::string longText = "This is a rather long sentence, with a comma in the middle and many words after it";
    c = chunk(longText, 40);
    CHECK(c.size() >= 2);
    for (auto& s : c) CHECK(s.size() <= 40);
    CHECK_EQ(c[0], std::string("This is a rather long sentence,"));
    CHECK(chunk("   ", 300).empty());
    CHECK_EQ(tts::text::chunkLength("ja"), size_t(120));
    CHECK_EQ(tts::text::chunkLength("fr"), size_t(300));
}

TEST(tts_language_support) {
    for (const char* l : {"en", "fr", "de", "es", "ru", "uk", "ar", "ja"}) CHECK(tts::languageSupported(l));
    CHECK(!tts::languageSupported("zh-Hans"));
    CHECK(!tts::languageSupported("zh-Hant"));
    CHECK(!tts::languageSupported(""));
}

// ------------------------------------------------------------------------------------------------
// Model stages against onnxruntime (tests/data/tts/ref_en.bin: "Good move, well played.", voice
// M2, 5 steps, numpy seed 7)
// ------------------------------------------------------------------------------------------------
namespace {

// Vocoder against onnxruntime's optimised run from the same latent. For scale: onnxruntime without
// its graph optimisations is 26.3 dB from it (8-bit activations: one rounding flip propagates).
constexpr double VOCODER_MIN_SNR = 20.0;

struct StageFixture {
    tts::Synthesizer* s = nullptr;
    Dump ref;
    std::vector<int64_t> ids;
    int voice = 0, steps = 0;
    tts::ExecContext ctx;
    // What the stage tests need and is missing (SKIP), "" when nothing is.
    std::string init() {
        s = model();
        if (!s) return "no model files in " + modelDir();
        if (!loadDump("tests/data/tts/ref_en.bin", ref)) return "tests/data/tts/ref_en.bin not found";
        ids = ref["text_ids"].toInts();
        voice = int(ref["voice"].toInts()[0]);
        steps = int(ref["steps"].toInts()[0]);
        ctx.k = &K();
        return std::string();
    }
};

}  // namespace

// voice.bin: the voice count must be the one its size gives. A crafted count whose product with
// the per-voice size only matches modulo 2^64 is refused before any style is read.
TEST(tts_voice_file_header) {
    std::string dir = net::sys::exeDirectory() + "ttstest-voices/";
    CHECK(net::sys::makeDirectories(dir));
    auto write = [&](const char* name, const std::string& data) {
        std::FILE* f = net::sys::openFile(dir + name, "wb");
        bool ok = f && std::fwrite(data.data(), 1, data.size(), f) == data.size();
        if (f) std::fclose(f);
        return ok;
    };
    for (int i = 0; i < tts::kFileCount; ++i) CHECK(write(tts::Engine::kFiles[i], "x"));   // not a model
    CHECK(write(tts::Engine::kFiles[tts::kFileIndexer], std::string(65536 * 4, '\0')));
    // 101 * inv == 1 (mod 2^55), so inv * 4 * (50 * 256 + 8 * 16) == 512 (mod 2^64): 48 + 512 bytes.
    uint64_t inv = 101;
    for (int i = 0; i < 6; ++i) inv *= 2 - 101 * inv;
    auto voices = [](int64_t count, size_t payload) {
        int64_t h[6] = {count, 50, 256, count, 8, 16};
        return std::string(reinterpret_cast<const char*>(h), 48) + std::string(payload, '\0');
    };
    std::string err;
    // Each engine goes before the files change: Windows neither rewrites nor deletes a mapped file.
    CHECK(write(tts::Engine::kFiles[tts::kFileVoices], voices(int64_t(inv & ((uint64_t(1) << 55) - 1)), 512)));
    {
        tts::Engine crafted;
        CHECK(!crafted.loadDirectory(dir, K(), &err) && err.find("voice.bin") != std::string::npos);
    }
    // One consistent voice passes this check (and then stops at the fake duration model).
    CHECK(write(tts::Engine::kFiles[tts::kFileVoices], voices(1, 4 * (50 * 256 + 8 * 16))));
    {
        tts::Engine one;
        CHECK(!one.loadDirectory(dir, K(), &err) && err.find("voice.bin") == std::string::npos);
    }
    for (int i = 0; i < tts::kFileCount; ++i) net::sys::removeFile(dir + tts::Engine::kFiles[i]);
#ifdef _WIN32
    _rmdir(dir.c_str());
#else
    rmdir(dir.c_str());
#endif
}

TEST(tts_stage_duration_and_text_encoder) {
    StageFixture f;
    if (std::string missing = f.init(); !missing.empty()) SKIP(missing);
    const tts::Engine& e = *f.s->engine();
    CHECK(tts::text::indices(tts::text::preprocess("Good move, well played.", "en"), e.indexer()) == f.ids);
    std::string err;
    float seconds = 0;
    CHECK_RUN(e.duration(f.ids, f.voice, f.ctx, &seconds, &err), err);
    float want = f.ref["duration"].scalarFloat();
    std::fprintf(stderr, "  duration %.6f s vs %.6f s (rel %.2g)\n", seconds, want, std::fabs(seconds - want) / want);
    CHECK(std::fabs(seconds - want) < 1e-4f * want);
    Tensor emb;
    CHECK_RUN(e.encode(f.ids, f.voice, f.ctx, &emb, &err), err);
    Diff d = diff(emb, f.ref["text_emb"]);
    report("text encoder", d);
    CHECK(d.cosine > 0.999999);
    CHECK(d.maxAbs < 1e-3 * std::max(1.0, d.refMaxAbs));
}

namespace {

// One Euler step of the vector estimator from the reference's latent before 'step'.
Tensor veStep(const StageFixture& f, const tts::ExecContext& ctx, int step) {
    const tts::Engine& e = *f.s->engine();
    const tts::Graph& g = e.graph(tts::kFileVectorEstimator);
    tts::Session s(g);
    int64_t T = f.ref["text_emb"].dims[2], L = f.ref["noise"].dims[2];
    auto set = [&](const char* name, Tensor t) { s.setInput(g.inputIndex(name), std::move(t)); };
    auto ones = [](int64_t n) {
        Tensor t = Tensor::alloc(DType::F32, {1, 1, n});
        for (int64_t i = 0; i < n; ++i) t.mut<float>()[i] = 1.0f;
        return t;
    };
    set("text_emb", f.ref["text_emb"]);
    set("style_ttl", e.styleTtl(f.voice));
    set("text_mask", ones(T));
    set("latent_mask", ones(L));
    set("total_step", F({1}, {float(f.steps)}));
    set("noisy_latent", step == 0 ? f.ref["noise"] : f.ref["latent" + std::to_string(step)]);
    set("current_step", F({1}, {float(step)}));
    std::string err;
    if (!s.run(ctx, &err)) {
        std::fprintf(stderr, "  %s\n", err.c_str());
        return Tensor();
    }
    return s.output(0);
}

}  // namespace

TEST(tts_stage_vector_estimator) {
    StageFixture f;
    if (std::string missing = f.init(); !missing.empty()) SKIP(missing);
    const tts::Engine& e = *f.s->engine();
    // Every step from the reference's previous latent (isolates the error of one step).
    double worst = 1.0;
    for (int step = 0; step < f.steps; ++step) {
        Diff d = diff(veStep(f, f.ctx, step), f.ref["latent" + std::to_string(step + 1)]);
        report(("vector estimator step " + std::to_string(step + 1)).c_str(), d);
        worst = std::min(worst, d.cosine);
    }
    CHECK(worst > 0.9999);
    // The chained loop from the same noise and text embedding.
    Tensor latent;
    std::string err;
    std::vector<Tensor> each;
    CHECK_RUN(e.denoise(f.ref["text_emb"], f.voice, f.ref["noise"], f.steps, f.ctx, &latent, nullptr, &err, &each), err);
    CHECK_EQ(each.size(), size_t(f.steps));
    Diff d = diff(latent, f.ref["latent" + std::to_string(f.steps)]);
    report("vector estimator, 5 chained steps", d);
    CHECK(d.cosine > 0.9995);
}

TEST(tts_stage_vocoder_and_end_to_end) {
    StageFixture f;
    if (std::string missing = f.init(); !missing.empty()) SKIP(missing);
    const tts::Engine& e = *f.s->engine();
    std::string err;
    Tensor wav;
    CHECK_RUN(e.vocode(f.ref["latent" + std::to_string(f.steps)], f.ctx, &wav, &err), err);
    Diff d = diff(wav, f.ref["wav"]);
    report("vocoder", d);
    double lsdV = logSpectralDistance(wav.as<float>(), f.ref["wav"].as<float>(), size_t(wav.count()));
    std::fprintf(stderr, "  vocoder log-spectral distance %.2f dB\n", lsdV);
    CHECK(d.snrDb > VOCODER_MIN_SNR);
    CHECK(lsdV < 1.5);
    // Whole chain from the ids and the reference noise.
    float seconds = 0;
    Tensor emb, latent, out;
    CHECK_RUN(e.duration(f.ids, f.voice, f.ctx, &seconds, &err), err);
    CHECK_RUN(e.encode(f.ids, f.voice, f.ctx, &emb, &err), err);
    CHECK_RUN(e.denoise(emb, f.voice, f.ref["noise"], f.steps, f.ctx, &latent, nullptr, &err), err);
    CHECK_RUN(e.vocode(latent, f.ctx, &out, &err), err);
    Diff de = diff(out, f.ref["wav"]);
    report("end to end (same noise)", de);
    double lsdE = out.count() == f.ref["wav"].count()
                      ? logSpectralDistance(out.as<float>(), f.ref["wav"].as<float>(), size_t(out.count()))
                      : 99.0;
    // onnxruntime itself, without its graph optimisations (plain QDQ arithmetic), is at 10.4 dB SNR
    // and 2.55 dB log-spectral distance from its optimised run; another noise seed is at 14.5 dB.
    std::fprintf(stderr, "  end to end log-spectral distance %.2f dB\n", lsdE);
    CHECK(de.snrDb > 5.0);
    CHECK(lsdE < 4.0);
    // Every stage stops on the cancel flag (Worker::stop() and cancel() do not wait for it).
    std::atomic<bool> cancel{true};
    CHECK(!e.duration(f.ids, f.voice, f.ctx, &seconds, &err, &cancel) && err == "cancelled");
    err.clear();
    CHECK(!e.encode(f.ids, f.voice, f.ctx, &emb, &err, &cancel) && err == "cancelled");
    err.clear();
    CHECK(!e.vocode(latent, f.ctx, &out, &err, &cancel) && err == "cancelled");
}

TEST(tts_stage_every_level) {
    // One vector estimator step and the vocoder with every kernel set (same results to rounding).
    StageFixture f;
    if (std::string missing = f.init(); !missing.empty()) SKIP(missing);
    const tts::Engine& e = *f.s->engine();
    for (int level : tablesToCheck()) {
        tts::ExecContext ctx;
        ctx.k = tableToCheck(level);
        Diff d = diff(veStep(f, ctx, 0), f.ref["latent1"]);
        std::string err;
        Tensor wav;
        CHECK_RUN(e.vocode(f.ref["latent" + std::to_string(f.steps)], ctx, &wav, &err), err);
        Diff dv = diff(wav, f.ref["wav"]);
        std::fprintf(stderr, "  %-8s vector estimator step 1 cosine %.8f, vocoder SNR %.1f dB\n", ctx.k->name, d.cosine,
                     dv.snrDb);
        CHECK(d.cosine > 0.9999);
        CHECK(dv.snrDb > VOCODER_MIN_SNR);
    }
}

// ------------------------------------------------------------------------------------------------
// Synthesizer and worker
// ------------------------------------------------------------------------------------------------
TEST(tts_synthesizer_output) {
    tts::Synthesizer* s = model();
    if (!s) SKIP("no model files in " + modelDir());
    CHECK_EQ(s->sampleRate(), 44100);
    CHECK_EQ(s->voiceCount(), 10);
    CHECK_EQ(s->voiceName(tts::defaultVoice()), std::string("M3"));
    tts::Options o;
    o.seed = 1234;
    std::vector<float> a = s->synthesize("Good move. Now the knight goes to f3.", "en", o);
    CHECK(a.size() > 44100);
    float peak = 0;
    double sum = 0;
    for (float v : a) {
        peak = std::max(peak, std::fabs(v));
        sum += double(v) * v;
    }
    double rmsDb = 10.0 * std::log10(sum / double(a.size()));
    std::fprintf(stderr, "  %.2f s, peak %.2f dBFS, RMS %.1f dBFS (whole clip), %d chunk(s)\n", a.size() / 44100.0,
                 20.0 * std::log10(peak), rmsDb, s->lastStats().chunks);
    CHECK(peak <= 0.8913f);
    CHECK(rmsDb > -30.0 && rmsDb < -18.0);
    CHECK(std::fabs(a.front()) < 1e-6f && std::fabs(a.back()) < 1e-3f);
    // Deterministic for a seed; another seed gives another rendition.
    std::vector<float> b = s->synthesize("Good move. Now the knight goes to f3.", "en", o);
    CHECK(a == b);
    // The same samples for any thread count (the work is split without changing any sum).
    for (int threads : {1, 3}) {
        tts::Options ot = o;
        ot.threads = threads;
        CHECK(s->synthesize("Good move. Now the knight goes to f3.", "en", ot) == a);
    }
    o.seed = 99;
    std::vector<float> c = s->synthesize("Good move. Now the knight goes to f3.", "en", o);
    CHECK(c != a);
    // Two chunks are joined with 0.3 s of silence.
    o.seed = 5;
    std::vector<float> two = s->synthesize(std::string(200, 'a') + ". " + std::string(150, 'b') + ".", "en", o);
    CHECK_EQ(s->lastStats().chunks, 2);
    size_t run = 0, longest = 0, longestEnd = 0;
    for (size_t i = 0; i < two.size(); ++i) {
        run = two[i] == 0.0f ? run + 1 : 0;
        if (run > longest) {
            longest = run;
            longestEnd = i + 1;
        }
    }
    CHECK(longest >= 13230 && longestEnd - longest > 441 && longestEnd + 441 < two.size());
    // Cancelled before starting: nothing.
    std::atomic<bool> cancel{true};
    CHECK(s->synthesize("Hello.", "en", o, &cancel).empty());
    // Unknown language: English; known CJK characters are kept, unsupported ones (U+E000, a
    // private-use code point) are dropped and counted.
    CHECK(!s->synthesize("Hello 世.", "zz", o).empty());
    CHECK_EQ(s->lastStats().droppedCharacters, 0);
    CHECK(!s->synthesize("Hello \xEE\x80\x80.", "zz", o).empty());
    CHECK_EQ(s->lastStats().droppedCharacters, 1);
}

// Non-finite samples (a damaged model) become silence instead of skipping the loudness and peak
// normalisation and reaching the mixer.
TEST(tts_finish_pcm_non_finite) {
    std::vector<float> pcm(44100);
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = 0.5f * std::sin(0.05f * float(i));
    pcm[1000] = std::nanf("");
    pcm[20000] = INFINITY;
    pcm[30000] = -INFINITY;
    tts::finishPcm(pcm);
    bool finite = true;
    float peak = 0.0f;
    for (float v : pcm) {
        finite = finite && std::isfinite(v);
        peak = std::max(peak, std::fabs(v));
    }
    CHECK(finite);
    CHECK(pcm[1000] == 0.0f && pcm[20000] == 0.0f);
    CHECK(peak > 0.1f && peak <= 0.8913f);
}

TEST(tts_worker) {
    // The worker loads from the model store's folder: the build's development copy here.
    tts::setModelFolder(modelDir());
    struct Restore {
        ~Restore() { tts::setModelFolder(std::string()); }
    } restore;
    tts::Worker w;
    tts::Options o;
    o.threads = 2;
    CHECK(w.start(o));
    auto waitFor = [](const std::function<bool()>& pred, double seconds) {
        auto t0 = std::chrono::steady_clock::now();
        while (!pred()) {
            if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > seconds) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
    };
    CHECK(waitFor([&] { return w.ready() || w.failed(); }, 120.0));
    if (w.failed()) {
        // No model: requests are refused and stop() still joins.
        CHECK_EQ(w.request("Hello.", "en"), 0u);
        w.stop();
        SKIP("worker without model files (failed() path checked)");
    }
    // Priorities: a long request first, then a low and a high one queued behind it.
    uint32_t first = w.request("This first sentence keeps the worker busy for a little while.", "en", 0, 1);
    uint32_t low = w.request("Low priority.", "en", 0, 2);
    uint32_t high = w.request("High priority.", "en", 5, 3);
    uint32_t dropped = w.request("This one is cancelled.", "en", 0, 4);
    CHECK(first && low && high && dropped);
    w.cancel(dropped);
    std::vector<uint32_t> finished;
    CHECK(waitFor([&] {
        for (uint32_t id : {first, low, high})
            if (w.done(id) && std::find(finished.begin(), finished.end(), id) == finished.end()) finished.push_back(id);
        return finished.size() == 3;
    }, 120.0));
    // The high-priority request overtakes the low one (the first may start before or after both).
    auto at = [&](uint32_t id) { return std::find(finished.begin(), finished.end(), id) - finished.begin(); };
    CHECK(finished.size() == 3 && at(high) < at(low));
    CHECK(!w.done(dropped));
    std::vector<float> pcm;
    CHECK(w.take(high, pcm) && !pcm.empty());
    CHECK(!w.take(high, pcm));   // taken once
    CHECK(!w.take(12345, pcm));
    // Seeds: the same text and seed through the worker equal a direct synthesis.
    tts::Synthesizer* s = model();
    if (s) {
        tts::Options direct = o;
        direct.seed = 2;
        std::vector<float> want = s->synthesize("Low priority.", "en", direct);
        std::vector<float> got;
        CHECK(w.take(low, got));
        CHECK(got == want);
        // A speed of the request's own (the rules lesson's), from the worker started at 1.0 without
        // loading anything again: the direct synthesis at that speed, bit for bit.
        uint32_t slow = w.request("Low priority.", "en", 0, 2, 0.92f);
        CHECK(slow && waitFor([&] { return w.done(slow); }, 120.0));
        direct.speed = 0.92f;
        std::vector<float> slowWant = s->synthesize("Low priority.", "en", direct), slowGot;
        CHECK(w.take(slow, slowGot));
        CHECK(slowGot == slowWant && slowGot.size() > got.size());
    }
    // Cancel everything, including the request in progress, then stop while busy.
    uint32_t busy = w.request(std::string(250, 'x') + ".", "en");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    w.cancel(0);
    CHECK(waitFor([&] { return w.pending() == 0; }, 30.0));
    CHECK(!w.done(busy));
    w.request(std::string(250, 'y') + ".", "en");
    auto t0 = std::chrono::steady_clock::now();
    w.stop();
    double stopMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "  stop() while busy took %.0f ms\n", stopMs);
    CHECK(stopMs < 2000.0);
    CHECK_EQ(w.request("After stop.", "en"), 0u);
}

// Files that load but cannot speak (an indexer of the right size whose ids leave the embedding
// table, as damaged contents of a downloaded file would): the warm-up gives no samples and the
// worker fails (failed() and warmUpFailed()), taking no request. Missing files fail the load.
TEST(tts_worker_warm_up_failure) {
    if (!model()) SKIP("no model files in " + modelDir());
    const std::string dir = net::sys::exeDirectory() + "ttstest-warmup/";
    CHECK(net::sys::makeDirectories(dir));
    // The big files are links to the build's copy (copies where links are refused); the indexer is
    // a file of its own.
    for (int i = 0; i < tts::kFileCount; ++i) {
        if (i == tts::kFileIndexer) continue;
        const std::filesystem::path from = std::filesystem::u8path(modelDir() + tts::Engine::kFiles[i]);
        const std::filesystem::path to = std::filesystem::u8path(dir + tts::Engine::kFiles[i]);
        std::error_code ec;
        // A link left by an earlier run is kept: Wine refuses to delete it while the shared model
        // maps the same file, so the cleanup at the end of this test cannot remove it there.
        if (std::filesystem::equivalent(from, to, ec)) continue;
        ec.clear();
        std::filesystem::remove(to, ec);
        std::filesystem::create_hard_link(from, to, ec);
        if (ec) std::filesystem::copy_file(from, to, ec);
        CHECK(!ec);
    }
    std::string ix;
    CHECK(net::sys::readFile(modelDir() + tts::Engine::kFiles[tts::kFileIndexer], ix, size_t(1) << 20));
    const std::string intact = ix;
    for (size_t i = 0; i + 4 <= ix.size(); i += 4) {
        int32_t id;
        std::memcpy(&id, ix.data() + i, 4);
        if (id >= 0) id = 1 << 20;   // far past the table: Gather "index out of range"
        std::memcpy(&ix[i], &id, 4);
    }
    CHECK(net::sys::writeFileAtomic(dir + tts::Engine::kFiles[tts::kFileIndexer], ix, false));
    struct Restore {
        ~Restore() { tts::setModelFolder(std::string()); }
    } restore;
    auto settle = [](tts::Worker& w) {
        for (int i = 0; i < 24000 && !w.ready() && !w.failed(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };
    tts::setModelFolder(dir);
    {
        tts::Worker w;
        CHECK(w.start(tts::Options()));
        settle(w);
        CHECK(w.failed() && w.warmUpFailed() && !w.ready());
        CHECK_EQ(w.request("Hello.", "en"), 0u);
        // A download replaced the damaged file: the same worker started again warms up and speaks
        // (GameScene::coachModelDownloaded gives it that one more try). Stopped first, as in the
        // game, so its thread has unmapped the files before the replacement.
        w.stop();
        CHECK(net::sys::writeFileAtomic(dir + tts::Engine::kFiles[tts::kFileIndexer], intact, false));
        CHECK(w.start(tts::Options()));
        settle(w);
        CHECK(w.ready() && !w.failed() && !w.warmUpFailed());
    }
    tts::setModelFolder(dir + "missing/");
    {
        tts::Worker w;
        CHECK(w.start(tts::Options()));
        settle(w);
        CHECK(w.failed() && !w.warmUpFailed());
    }
    for (int i = 0; i < tts::kFileCount; ++i) net::sys::removeFile(dir + tts::Engine::kFiles[i]);
#ifdef _WIN32
    _rmdir(dir.c_str());
#else
    rmdir(dir.c_str());
#endif
}

// ------------------------------------------------------------------------------------------------
// Optional: performance, samples, per-node comparison
// ------------------------------------------------------------------------------------------------
namespace {

double loadAverage() {
    std::string s;
    if (!net::sys::readFile("/proc/loadavg", s, 256)) return -1.0;
    return std::atof(s.c_str());
}

long statusKb(const char* key) {
    std::string s;
    if (!net::sys::readFile("/proc/self/status", s, 1 << 16)) return -1;
    size_t p = s.find(key);
    return p == std::string::npos ? -1 : std::atol(s.c_str() + p + std::strlen(key) + 1);
}

}  // namespace

TEST(tts_perf) {
    if (!std::getenv("SCACELITH_TTS_PERF")) SKIP("SCACELITH_TTS_PERF not set");
    tts::Synthesizer* s = model();
    if (!s) SKIP("no model files in " + modelDir());
    const char* text = "Good move, well played. Now the knight goes to f3.";
    // The best kernel set of this CPU, and AVX2 (the common desktop case) when it is not the best.
    // SCACELITH_TTS_PERF_ARCH=sse2,avx2 measures the given sets instead.
    std::vector<std::string> arches = {"auto"};
    if (tts::kern::active().level > tts::kern::kAvx2 && tts::kern::cpuRuns(tts::kern::kAvx2)) arches.push_back("avx2");
    if (const char* list = std::getenv("SCACELITH_TTS_PERF_ARCH")) {
        arches.clear();
        std::string l = list;
        for (size_t p = 0; p <= l.size();) {
            size_t e = std::min(l.find(',', p), l.size());
            if (e > p) arches.push_back(l.substr(p, e - p));
            p = e + 1;
        }
    }
    for (const std::string& arch : arches) {
        if (!tts::setArchCap(arch.c_str())) continue;
        for (int threads : {1, 2}) {
            tts::Options o;
            o.threads = threads;
            o.seed = 7;
            s->synthesize(text, "en", o);   // warm-up
            double best = 1e9, cpu = 0;
            tts::Synthesizer::Stats st;
            for (int rep = 0; rep < 3; ++rep) {
                std::clock_t c0 = std::clock();
                s->synthesize(text, "en", o);
                double c = double(std::clock() - c0) / CLOCKS_PER_SEC;
                if (s->lastStats().total < best) {
                    best = s->lastStats().total;
                    st = s->lastStats();
                    cpu = c;
                }
            }
            std::fprintf(stderr,
                         "  %d thread(s), %s: %.2f s of audio in %.3f s, RTF %.3f, CPU %.3f s (dp %.1f ms, te %.1f ms, "
                         "ve %.1f ms, vocoder %.1f ms), load average %.2f\n",
                         threads, tts::activeArch(), st.audioSeconds, st.total, st.total / st.audioSeconds, cpu,
                         st.duration * 1e3, st.textEncoder * 1e3, st.vectorEstimator * 1e3, st.vocoder * 1e3,
                         loadAverage());
        }
    }
    tts::setArchCap("auto");
    std::fprintf(stderr,
                 "  memory: VmRSS %ld kB (RssAnon %ld kB, RssFile %ld kB: the mapped model files), VmHWM %ld kB\n",
                 statusKb("VmRSS:"), statusKb("RssAnon:"), statusKb("RssFile:"), statusKb("VmHWM:"));
    // GEMM throughput on the shapes of the vector estimator (pointwise convolutions at L = 61, batch 2)
    // and of the vocoder (T = 366, and T = 318 with K = 2048).
    {
        std::mt19937 rng(1);
        const int shapes[][3] = {{2048, 122, 512}, {512, 122, 2048}, {2048, 366, 512}, {512, 318, 2048}};
        tts::ThreadPool pool2(2);
        for (auto& sh : shapes) {
            int M = sh[0], N = sh[1], Kd = sh[2];
            std::vector<float> A = randomVec(size_t(M) * Kd, rng), B = randomVec(size_t(Kd) * N, rng), C(size_t(M) * N);
            std::vector<int8_t> Aq(size_t(M) * Kd, 3);
            std::vector<float> sc(static_cast<size_t>(M), 0.01f);
            std::vector<uint8_t> Bu(size_t(Kd) * N, 7);
            std::vector<int32_t> Ci(size_t(M) * N);
            for (int variant = 0; variant < 6; ++variant) {
                tts::ThreadPool* gp = variant >= 3 ? &pool2 : nullptr;
                tts::GemmA ga;
                ga.ld = Kd;
                if (variant % 3 == 0) ga.f32 = A.data();
                else {
                    ga.i8 = Aq.data();
                    ga.scale = sc.data();
                }
                tts::GemmB gb;
                gb.f32 = B.data();
                gb.ld = N;
                auto t0 = std::chrono::steady_clock::now();
                const int reps = 10;
                for (int r = 0; r < reps; ++r) {
                    if (variant % 3 < 2) tts::sgemm(K(), gp, M, N, Kd, ga, gb, C.data(), N);
                    else
                        tts::igemm(K(), gp, M, N, Kd, reinterpret_cast<const uint8_t*>(Aq.data()), Kd, false, 0,
                                   Bu.data(), N, true, 128, Ci.data(), N);
                }
                double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / reps;
                std::fprintf(stderr, "  gemm %s %dx%dx%d, %d thread(s): %.2f ms, %.1f G MAC/s\n",
                             variant % 3 == 0 ? "f32 " : variant % 3 == 1 ? "i8w " : "int8", M, N, Kd, gp ? 2 : 1,
                             sec * 1e3,
                             double(M) * N * Kd / sec * 1e-9);
            }
        }
    }
    // Time per operator, one thread.
    tts::Options o;
    o.threads = 1;
    o.seed = 7;
    tts::resetProfile();
    tts::setProfiling(true);
    s->synthesize(text, "en", o);
    tts::setProfiling(false);
    std::vector<std::pair<double, int>> ops;
    for (int i = 0; i < int(tts::Op::Count); ++i) ops.push_back({tts::profileSeconds()[i], i});
    std::sort(ops.rbegin(), ops.rend());
    std::string line;
    for (size_t i = 0; i < 12 && ops[i].first > 0; ++i) {
        char buf[64];
        std::snprintf(buf, sizeof buf, " %s %.1f", tts::opName(tts::Op(ops[i].second)), ops[i].first * 1e3);
        line += buf;
    }
    std::fprintf(stderr, "  ms per operator:%s\n", line.c_str());
    // SCACELITH_TTS_PERF_NODES=<n>: also the n slowest nodes.
    if (const char* nodes = std::getenv("SCACELITH_TTS_PERF_NODES"))
        std::fprintf(stderr, "%s", tts::profileReport(size_t(std::atoi(nodes))).c_str());
}

TEST(tts_samples) {
    const char* dir = std::getenv("SCACELITH_TTS_SAMPLES");
    if (!dir) SKIP("SCACELITH_TTS_SAMPLES not set");
    tts::Synthesizer* s = model();
    if (!s) SKIP("no model files in " + modelDir());
    // Texts from a file instead of the built-in lines: "lang<TAB>name<TAB>text" per line, where
    // lang may carry a voice ("en:M3"; default voice otherwise). The noise seed is the game's
    // default (derived from the text), so the files sound as the game will play them.
    if (const char* list = std::getenv("SCACELITH_TTS_SAMPLE_LIST")) {
        std::string content;
        CHECK(net::sys::readFile(list, content, 1 << 20));
        size_t pos = 0;
        while (pos < content.size()) {
            size_t e = content.find('\n', pos);
            if (e == std::string::npos) e = content.size();
            std::string line = content.substr(pos, e - pos);
            pos = e + 1;
            size_t t1 = line.find('\t'), t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
            if (t2 == std::string::npos) continue;
            std::string lang = line.substr(0, t1), voice;
            if (size_t c = lang.find(':'); c != std::string::npos) {
                voice = lang.substr(c + 1);
                lang.resize(c);
            }
            tts::Options o;
            for (int v = 0; v < s->voiceCount(); ++v)
                if (s->voiceName(v) == voice) o.voice = v;
            CHECK(voice.empty() || o.voice >= 0);
            std::vector<float> pcm = s->synthesize(line.substr(t2 + 1), lang, o);
            CHECK(!pcm.empty());
            std::string path = std::string(dir) + "/" + line.substr(t1 + 1, t2 - t1 - 1) + ".wav";
            CHECK(audio::writeWav16(path.c_str(), pcm.data(), pcm.size(), 1, 44100));
        }
        return;
    }
    struct Line {
        const char* lang;
        const char* text;
    };
    const Line voiceLines[] = {
        {"en", "Good move! Your knight is well placed now. Let's see how you continue."},
        {"fr", "Bien joué ! Ton cavalier est maintenant bien placé. Voyons la suite."},
        {"ja", "いい手ですね。ナイトがよい位置にあります。"},
        {"en", "Careful: after this move, your queen is attacked by the bishop. Would you like to take it back "
               "and try again?"},
        {"fr", "Attention : après ce coup, ta dame est attaquée par le fou. Veux-tu reprendre ton coup et "
               "chercher autre chose ?"},
        {"ja", "気をつけてください。この手のあと、クイーンがビショップに狙われます。もう一度考えてみましょうか。"},
    };
    for (int voice : {6, 7, 9})
        for (size_t i = 0; i < sizeof voiceLines / sizeof voiceLines[0]; ++i) {
            const Line& l = voiceLines[i];
            tts::Options o;
            o.voice = voice;
            o.seed = 42;
            std::vector<float> pcm = s->synthesize(l.text, l.lang, o);
            std::string path = std::string(dir) + "/voice_" + s->voiceName(voice) + "_" + l.lang + "_" +
                               std::to_string(i / 3 + 1) + ".wav";
            CHECK(audio::writeWav16(path.c_str(), pcm.data(), pcm.size(), 1, 44100));
        }
    // Chess notation probes: each written form, one file per language and form.
    const char* langs[] = {"en", "fr", "de", "es", "ru", "uk", "ar", "ja"};
    const char* probes[] = {"e4", "Nf3", "O-O", "h7", "1-0", "+1.5"};
    for (const char* lang : langs)
        for (int i = 0; i < 6; ++i) {
            tts::Options o;
            o.seed = 42;
            std::vector<float> pcm = s->synthesize(probes[i], lang, o);
            std::string path = std::string(dir) + "/notation_" + lang + "_" + std::to_string(i) + ".wav";
            CHECK(audio::writeWav16(path.c_str(), pcm.data(), pcm.size(), 1, 44100));
        }
}

TEST(tts_node_diff) {
    const char* dumpPath = std::getenv("SCACELITH_TTS_NODE_DUMP");
    const char* stage = std::getenv("SCACELITH_TTS_NODE_STAGE");
    if (!dumpPath || !stage) SKIP("SCACELITH_TTS_NODE_DUMP or SCACELITH_TTS_NODE_STAGE not set");
    StageFixture f;
    if (std::string missing = f.init(); !missing.empty()) SKIP(missing);
    std::string bytes;
    Dump nodes;
    CHECK(net::sys::readFile(dumpPath, bytes, 1u << 31) && nodes.parse(bytes));
    const tts::Engine& e = *f.s->engine();
    std::string st = stage;
    tts::ModelFile file = st == "dp" ? tts::kFileDuration
                          : st == "te" ? tts::kFileTextEncoder
                          : st == "ve" ? tts::kFileVectorEstimator
                                       : tts::kFileVocoder;
    const tts::Graph& g = e.graph(file);
    tts::Session s(g);
    s.setKeepAll(true);
    auto set = [&](const char* name, Tensor t) {
        int i = g.inputIndex(name);
        if (i >= 0) s.setInput(i, std::move(t));
    };
    auto ones = [](int64_t n) {
        Tensor t = Tensor::alloc(DType::F32, {1, 1, n});
        for (int64_t i = 0; i < n; ++i) t.mut<float>()[i] = 1.0f;
        return t;
    };
    set("text_ids", f.ref["text_ids"]);
    set("style_dp", e.styleDp(f.voice));
    set("style_ttl", e.styleTtl(f.voice));
    set("text_mask", ones(f.ref["text_ids"].dims[1]));
    set("text_emb", f.ref["text_emb"]);
    set("noisy_latent", f.ref["noise"]);
    set("latent_mask", ones(f.ref["noise"].dims[2]));
    set("current_step", F({1}, {0.0f}));
    set("total_step", F({1}, {float(f.steps)}));
    set("latent", f.ref["latent" + std::to_string(f.steps)]);
    std::string err;
    CHECK(s.run(f.ctx, &err));
    if (!err.empty()) std::fprintf(stderr, "  %s\n", err.c_str());
    int shown = 0, compared = 0;
    for (const tts::Node& n : g.nodes())
        for (int v : n.out) {
            const std::string& name = g.values()[size_t(v)].name;
            if (!nodes.has(name)) continue;
            const Tensor& mine = s.value(v);
            const Tensor& ref = nodes[name];
            ++compared;
            if (mine.type != ref.type || mine.count() != ref.count()) {
                if (shown++ < 20)
                    std::fprintf(stderr, "  %s %s: type/size %s/%lld vs %s/%lld\n", tts::opName(n.op), name.c_str(),
                                 tts::dtypeName(mine.type), (long long)mine.count(), tts::dtypeName(ref.type),
                                 (long long)ref.count());
                continue;
            }
            if (mine.type == DType::F32) {
                Diff d = diff(mine, ref);
                if ((d.cosine < 0.99999 || d.maxAbs > 1e-3 * std::max(1.0, d.refMaxAbs)) && shown++ < 40)
                    std::fprintf(stderr, "  %-20s %-40s max|d| %.3g (ref %.3g) cos %.7f\n", tts::opName(n.op),
                                 name.c_str(), d.maxAbs, d.refMaxAbs, d.cosine);
            } else if (std::memcmp(mine.data, ref.data, mine.bytes()) != 0) {
                size_t bad = 0;
                for (size_t i = 0; i < mine.bytes(); ++i)
                    bad += mine.as<uint8_t>()[i] != ref.as<uint8_t>()[i];
                if (shown++ < 40)
                    std::fprintf(stderr, "  %-20s %-40s %zu of %zu bytes differ\n", tts::opName(n.op), name.c_str(), bad,
                                 mine.bytes());
            }
        }
    std::fprintf(stderr, "  %d values compared, %d reported\n", compared, shown);
    for (size_t i = 0; i < g.outputCount(); ++i) {
        const std::string& name = g.values()[size_t(g.outputs()[i])].name;
        if (nodes.has(name) && s.output(int(i)).type == DType::F32) report(("output " + name).c_str(), diff(s.output(int(i)), nodes[name]));
    }
}
