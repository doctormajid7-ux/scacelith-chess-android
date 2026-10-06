// SDF shrink-wrap mesher: capsule-parametrised base grid projected radially onto the zero set,
// then adaptive refinement: edges are marked where they are too long, bend too much or deviate
// from the surface, a closure pass also marks a triangle's longest edge when its squared length
// exceeds 2.6x the shortest marked one, and triangles with 1, 2 or 3 marked edges are split
// (red/green). Midpoints are Newton-projected back onto the surface, normals come from the SDF
// gradient.
#include "sdf.h"
#include "../core/log.h"
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

using namespace m;

namespace character {
namespace sdf {

float Profile::operator()(float t) const {
    const size_t n = keys.size();
    if (n == 0) return 0.0f;
    if (t <= keys[0].t) return keys[0].v;
    if (t >= keys[n - 1].t) return keys[n - 1].v;
    size_t i = 0;
    while (i + 1 < n && keys[i + 1].t < t) ++i;
    const Key &k0 = keys[i], &k1 = keys[i + 1];
    float h = k1.t - k0.t;
    auto slope = [&](size_t j) {
        if (j == 0 || j + 1 >= n) return 0.0f;  // flat ends
        float dl = (keys[j].v - keys[j - 1].v) / (keys[j].t - keys[j - 1].t);
        float dr = (keys[j + 1].v - keys[j].v) / (keys[j + 1].t - keys[j].t);
        if (dl * dr <= 0.0f) return 0.0f;  // local extremum: keep monotone
        return 2.0f / (1.0f / dl + 1.0f / dr);  // harmonic mean (Fritsch-Butland)
    };
    float m0 = slope(i), m1 = slope(i + 1);
    float s = (t - k0.t) / h, s2 = s * s, s3 = s2 * s;
    return (2 * s3 - 3 * s2 + 1) * k0.v + (s3 - 2 * s2 + s) * h * m0 + (-2 * s3 + 3 * s2) * k1.v + (s3 - s2) * h * m1;
}

vec3 gradient(const Fn& f, vec3 p, float h) {
    // Tetrahedral central differences (4 evaluations).
    const vec3 k0(1, -1, -1), k1(-1, -1, 1), k2(-1, 1, -1), k3(1, 1, 1);
    return k0 * f(p + k0 * h) + k1 * f(p + k1 * h) + k2 * f(p + k2 * h) + k3 * f(p + k3 * h);
}

vec3 project(const Fn& f, vec3 p, float h, int iterations) {
    for (int i = 0; i < iterations; ++i) {
        float v = f(p);
        if (std::fabs(v) < 1e-7f) break;
        vec3 g = gradient(f, p, h) * (1.0f / (4.0f * h));
        float g2 = dot(g, g);
        if (g2 < 1e-12f) break;
        p -= g * (v / g2);
    }
    return p;
}

namespace {

// Distance along dir from org (inside) to the first zero crossing.
float march(const Fn& f, vec3 org, vec3 dir, float maxStep, int& misses) {
    float v = f(org);
    if (v > 0.0f) ++misses;
    float t = 0.0f, prev = 0.0f;
    for (int i = 0; i < 4000; ++i) {
        float step = clamp(-v * 0.6f, 2e-6f, maxStep);
        prev = t;
        t += step;
        v = f(org + dir * t);
        if (v >= 0.0f) {
            float lo = prev, hi = t;
            for (int k = 0; k < 40 && hi - lo > 1e-8f; ++k) {
                float mid = 0.5f * (lo + hi);
                if (f(org + dir * mid) >= 0.0f) hi = mid;
                else lo = mid;
            }
            return 0.5f * (lo + hi);
        }
        if (t > 3.0f) break;
    }
    return t;
}

inline uint64_t edgeKey(uint32_t a, uint32_t b) {
    if (a > b) std::swap(a, b);
    return (uint64_t(a) << 32) | b;
}

}  // namespace

void refine(MeshData& mesh, const Fn& f, const MeshOptions& o) {
    std::vector<vec3> pos, nrm;
    pos.reserve(mesh.vertices.size() * 4);
    nrm.reserve(mesh.vertices.size() * 4);
    for (auto& v : mesh.vertices) {
        pos.push_back(v.pos);
        nrm.push_back(normalize(gradient(f, v.pos, o.gradientStep)));
    }
    std::vector<uint32_t> tri = mesh.indices;
    const float cosMax = std::cos(o.maxAngleDeg * DEG);
    const size_t maxTris = 400000;
    for (int it = 0; it < o.maxIterations; ++it) {
        std::unordered_set<uint64_t> marked;
        auto want = [&](uint32_t a, uint32_t b) {
            float len = length(pos[a] - pos[b]);
            if (len < o.minEdge) return false;
            if (len > o.maxEdge) return true;
            float c = dot(nrm[a], nrm[b]);
            if (c < cosMax) return true;
            float ang = std::acos(clamp(c, -1.0f, 1.0f));
            return len * ang * 0.125f > o.maxDeviation;
        };
        for (size_t t = 0; t < tri.size(); t += 3)
            for (int e = 0; e < 3; ++e) {
                uint32_t a = tri[t + e], b = tri[t + (e + 1) % 3];
                if (want(a, b)) marked.insert(edgeKey(a, b));
            }
        if (std::getenv("SCACELITH_SDF_DEBUG")) {
            int nl = 0, na = 0, nd = 0;
            for (uint64_t k : marked) {
                uint32_t a = uint32_t(k >> 32), b = uint32_t(k & 0xFFFFFFFFu);
                float len = length(pos[a] - pos[b]);
                float c = dot(nrm[a], nrm[b]);
                if (len > o.maxEdge) ++nl;
                else if (c < cosMax) ++na;
                else ++nd;
            }
            LOGI("refine(%s) it %d: tris %d marked %d (len %d angle %d dev %d)", o.name, it, int(tri.size() / 3), int(marked.size()), nl, na, nd);
        }
        if (marked.empty()) break;
        if (tri.size() / 3 > maxTris) {
            LOGW("sdf::refine: triangle budget reached");
            break;
        }
        // Mild quality closure: a triangle with a split edge also splits its longest edge when that
        // edge is much longer than the split one (bounds slivers but keeps anisotropic triangles
        // along directions of low curvature).
        for (bool changed = true; changed;) {
            changed = false;
            for (size_t t = 0; t < tri.size(); t += 3) {
                float minMarked = 1e30f, maxLen = -1.0f;
                int L = 0;
                for (int e = 0; e < 3; ++e) {
                    uint32_t a = tri[t + e], b = tri[t + (e + 1) % 3];
                    float l2 = length2(pos[a] - pos[b]);
                    if (marked.count(edgeKey(a, b))) minMarked = std::min(minMarked, l2);
                    if (l2 > maxLen) { maxLen = l2; L = e; }
                }
                if (minMarked >= 1e30f) continue;
                uint64_t k = edgeKey(tri[t + L], tri[t + (L + 1) % 3]);
                if (maxLen > 2.6f * minMarked && !marked.count(k)) { marked.insert(k); changed = true; }
            }
        }
        std::unordered_map<uint64_t, uint32_t> mid;
        mid.reserve(marked.size() * 2);
        for (uint64_t k : marked) {
            uint32_t a = uint32_t(k >> 32), b = uint32_t(k & 0xFFFFFFFFu);
            vec3 c = (pos[a] + pos[b]) * 0.5f;
            vec3 p = project(f, c, o.gradientStep);
            float len = length(pos[a] - pos[b]);
            vec3 g = gradient(f, p, o.gradientStep);
            vec3 n = length2(g) > 0 ? normalize(g) : normalize(nrm[a] + nrm[b]);
            // An edge bridging two different sheets (across a thin gap or a fold) projects far away
            // or onto a surface facing elsewhere: leave it alone instead of feeding a runaway.
            bool ok = length(p - c) < 0.35f * len + 1e-6f && dot(n, nrm[a] + nrm[b]) > 0.0f && dot(nrm[a], nrm[b]) > -0.5f;
            if (!ok) continue;
            mid[k] = uint32_t(pos.size());
            pos.push_back(p);
            nrm.push_back(n);
        }
        std::vector<uint32_t> out;
        out.reserve(tri.size() * 2);
        auto emit = [&](uint32_t a, uint32_t b, uint32_t c) { out.push_back(a); out.push_back(b); out.push_back(c); };
        for (size_t t = 0; t < tri.size(); t += 3) {
            uint32_t v[3] = {tri[t], tri[t + 1], tri[t + 2]};
            int64_t m[3];
            int count = 0;
            for (int e = 0; e < 3; ++e) {
                auto it2 = mid.find(edgeKey(v[e], v[(e + 1) % 3]));
                m[e] = it2 == mid.end() ? -1 : int64_t(it2->second);
                count += m[e] >= 0;
            }
            if (count == 0) {
                emit(v[0], v[1], v[2]);
            } else if (count == 1) {
                int e = m[0] >= 0 ? 0 : (m[1] >= 0 ? 1 : 2);
                uint32_t a = v[e], b = v[(e + 1) % 3], c = v[(e + 2) % 3], mm = uint32_t(m[e]);
                emit(a, mm, c);
                emit(mm, b, c);
            } else if (count == 2) {
                int u = m[0] < 0 ? 0 : (m[1] < 0 ? 1 : 2);  // the unsplit edge (c -> a)
                uint32_t c = v[u], a = v[(u + 1) % 3], b = v[(u + 2) % 3];
                uint32_t m0 = uint32_t(m[(u + 1) % 3]), m1 = uint32_t(m[(u + 2) % 3]);
                emit(m0, b, m1);
                if (length2(pos[a] - pos[m1]) < length2(pos[m0] - pos[c])) { emit(a, m0, m1); emit(a, m1, c); }
                else { emit(a, m0, c); emit(m0, m1, c); }
            } else {
                uint32_t mab = uint32_t(m[0]), mbc = uint32_t(m[1]), mca = uint32_t(m[2]);
                emit(v[0], mab, mca);
                emit(mab, v[1], mbc);
                emit(mca, mbc, v[2]);
                emit(mab, mbc, mca);
            }
        }
        tri.swap(out);
    }
    mesh.vertices.resize(pos.size());
    for (size_t i = 0; i < pos.size(); ++i) {
        mesh.vertices[i].pos = pos[i];
        mesh.vertices[i].normal = nrm[i];
        mesh.vertices[i].uv = vec2(0, 0);
    }
    mesh.indices.swap(tri);
}

void surfaceFrames(MeshData& mesh, const Fn& f, const MeshOptions& o) {
    for (auto& v : mesh.vertices) {
        vec3 g = gradient(f, v.pos, o.gradientStep);
        v.normal = length2(g) > 1e-20f ? normalize(g) : vec3(0, 1, 0);
        vec3 t = o.tangentAxis - v.normal * dot(v.normal, o.tangentAxis);
        t = length2(t) > 1e-8f ? normalize(t) : orthogonal(v.normal);
        v.tangent = vec4(t, 1.0f);
    }
    // Orient every triangle with the outward SDF normal.
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const Vertex &a = mesh.vertices[mesh.indices[i]], &b = mesh.vertices[mesh.indices[i + 1]], &c = mesh.vertices[mesh.indices[i + 2]];
        vec3 fn = cross(b.pos - a.pos, c.pos - a.pos);
        if (dot(fn, a.normal + b.normal + c.normal) < 0.0f) std::swap(mesh.indices[i + 1], mesh.indices[i + 2]);
    }
}

MeshData meshSegment(const Fn& f, vec3 a, vec3 b, const MeshOptions& oIn, vec3 poleAxis) {
#ifdef __ANDROID__
    // Phones: a coarser robot (fewer triangles to draw every frame in every pass, and a load that
    // does not take half a minute). The tolerances, not the shapes: the joints and the grip keep
    // their geometry within a fraction of a millimetre.
    MeshOptions o = oIn;
    o.maxDeviation *= 3.0f;
    o.maxAngleDeg *= 1.5f;
    o.maxEdge *= 1.5f;
    o.nu = std::max(16, o.nu * 2 / 3);
    o.nv = std::max(16, o.nv * 2 / 3);
#else
    const MeshOptions& o = oIn;
#endif
    vec3 ab = b - a;
    float L = length(ab);
    vec3 w = L > 1e-6f ? ab / L : normalize(poleAxis);
    if (L <= 1e-6f) { L = 0.0f; b = a; }
    vec3 e1 = orthogonal(w), e2 = cross(w, e1);
    int misses = 0;
    vec3 midp = (a + b) * 0.5f;
    float R = 0.0f;
    for (int k = 0; k < 4; ++k) {
        float ang = float(k) * PI * 0.5f;
        R += march(f, midp, e1 * std::cos(ang) + e2 * std::sin(ang), 0.004f, misses);
    }
    R = std::max(R * 0.25f, 1e-4f);
    float maxStep = std::max(R * 0.06f, 0.0002f);
    const int nu = std::max(o.nu, 3), nv = std::max(o.nv, 2);
    float cap = 0.5f * PI * R, total = 2.0f * cap + L;
    MeshData d;
    auto addVertex = [&](vec3 org, vec3 dir) {
        Vertex v;
        v.pos = org + dir * march(f, org, dir, maxStep, misses);
        d.vertices.push_back(v);
    };
    // Cap ray for cap coordinate c (0 = equator, 1 = pole) at end point e, axis direction ax.
    auto capVertex = [&](vec3 e, vec3 ax, vec3 radial, float c) {
        float phi = 0.5f * PI * std::max(0.0f, 1.0f - c);
        addVertex(e, ax * std::cos(phi) + radial * std::sin(phi));
    };
    addVertex(a, -w);  // pole 0
    for (int j = 1; j < nv; ++j) {
        float s = total * float(j) / float(nv);
        for (int i = 0; i < nu; ++i) {
            float th = TAU * (float(i) + (j & 1 ? 0.5f : 0.0f)) / float(nu);
            vec3 radial = e1 * std::cos(th) + e2 * std::sin(th);
            if (s < cap) capVertex(a, -w, radial, 1.0f - s / cap);
            else if (s <= cap + L) addVertex(a + w * (s - cap), radial);
            else capVertex(b, w, radial, 1.0f - (total - s) / cap);
        }
    }
    addVertex(b, w);  // pole 1
    uint32_t pole1 = uint32_t(d.vertices.size() - 1);
    auto row = [&](int j, int i) { return uint32_t(1 + (j - 1) * nu + ((i % nu) + nu) % nu); };
    for (int i = 0; i < nu; ++i) {
        d.indices.insert(d.indices.end(), {0u, row(1, i + 1), row(1, i)});
        d.indices.insert(d.indices.end(), {pole1, row(nv - 1, i), row(nv - 1, i + 1)});
    }
    for (int j = 1; j < nv - 1; ++j)
        for (int i = 0; i < nu; ++i) {
            // Staggered rows: odd rows are offset by half a step, so rows alternate the diagonal.
            uint32_t p0 = row(j, i), p1 = row(j, i + 1);
            uint32_t q0 = row(j + 1, i), q1 = row(j + 1, i + 1);
            if (j & 1) {  // row j offset +0.5: q0 sits under between p(-1)..p0 -> pattern
                d.indices.insert(d.indices.end(), {p0, p1, q1});
                d.indices.insert(d.indices.end(), {p0, q1, q0});
            } else {  // row j+1 offset +0.5: q0 sits between p0 and p1
                d.indices.insert(d.indices.end(), {p0, p1, q0});
                d.indices.insert(d.indices.end(), {p1, q1, q0});
            }
        }
    if (misses > 0)
        LOGW("sdf::meshSegment(%s): %d rays started outside the shape (a %.4f %.4f %.4f  b %.4f %.4f %.4f, f %.5f %.5f %.5f)", o.name,
             misses, a.x, a.y, a.z, b.x, b.y, b.z, f(a), f(midp), f(b));
    refine(d, f, o);
    surfaceFrames(d, f, o);
    return d;
}

}  // namespace sdf
}  // namespace character
