#include "sdf_mesher.h"
#include <algorithm>
#include <cmath>

using namespace m;

namespace sdf {

float sdRoundCone(vec3 p, vec3 a, vec3 b, float r1, float r2) {
    // iq: exact round cone between two spheres.
    vec3 ba = b - a;
    float l2 = dot(ba, ba), rr = r1 - r2, a2 = l2 - rr * rr, il2 = 1.0f / l2;
    vec3 pa = p - a;
    float y = dot(pa, ba), z = y - l2;
    vec3 xv = pa * l2 - ba * y;
    float x2 = dot(xv, xv), y2 = y * y * l2, z2 = z * z * l2;
    float k = (rr >= 0 ? 1.0f : -1.0f) * rr * rr * x2;
    if ((z >= 0 ? 1.0f : -1.0f) * a2 * z2 > k) return std::sqrt(x2 + z2) * il2 - r2;
    if ((y >= 0 ? 1.0f : -1.0f) * a2 * y2 < k) return std::sqrt(x2 + y2) * il2 - r1;
    return (std::sqrt(x2 * a2 * il2) + y * rr) * il2 - r1;
}

float sdSegment2(vec2 p, vec2 a, vec2 b) {
    vec2 pa = p - a, ba = b - a;
    float h = clamp(dot(pa, ba) / std::max(dot(ba, ba), 1e-20f), 0.0f, 1.0f);
    return length(pa - ba * h);
}

// ---------------------------------------------------------------------------------------------
void Grid2D::build(const std::vector<vec2>& poly, float cell, float band) {
    size_t n = poly.size();
    if (n < 3) { w_ = h_ = 0; return; }
    vec2 lo(1e30f), hi(-1e30f);
    for (auto& p : poly) { lo = min(lo, p); hi = max(hi, p); }
    float margin = band + 2.0f * cell;
    cell_ = cell;
    band_ = band;
    origin_ = lo - vec2(margin);
    w_ = int(std::ceil((hi.x - lo.x + 2 * margin) / cell)) + 1;
    h_ = int(std::ceil((hi.y - lo.y + 2 * margin) / cell)) + 1;
    const size_t count = size_t(w_) * size_t(h_);
    const float INF = 1e30f;
    d_.assign(count, INF);
    t_.assign(count, 0.0f);
    std::vector<vec2> cp(count, vec2(INF));  // closest boundary point per cell
    // Exact unsigned distance in a narrow band around each segment...
    const float nb = 2.5f * cell;
    float s = 0.0f;
    for (size_t k = 0; k < n; ++k) {
        vec2 a = poly[k], b = poly[(k + 1) % n];
        vec2 ba = b - a;
        float len2 = dot(ba, ba), len = std::sqrt(len2);
        int i0 = std::max(0, int(std::floor((std::min(a.x, b.x) - nb - origin_.x) / cell)));
        int i1 = std::min(w_ - 1, int(std::ceil((std::max(a.x, b.x) + nb - origin_.x) / cell)));
        int j0 = std::max(0, int(std::floor((std::min(a.y, b.y) - nb - origin_.y) / cell)));
        int j1 = std::min(h_ - 1, int(std::ceil((std::max(a.y, b.y) + nb - origin_.y) / cell)));
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) {
                vec2 p = origin_ + vec2(float(i), float(j)) * cell;
                vec2 pa = p - a;
                float hh = len2 > 1e-20f ? clamp(dot(pa, ba) / len2, 0.0f, 1.0f) : 0.0f;
                vec2 c = a + ba * hh;
                float d = length(p - c);
                size_t idx = size_t(j) * size_t(w_) + size_t(i);
                if (d < d_[idx]) { d_[idx] = d; t_[idx] = s + hh * len; cp[idx] = c; }
            }
        s += len;
    }
    perimeter_ = s;
    // ...then propagated over the whole grid by closest-point dead reckoning (two sweeps). Cell a
    // (centre p) takes neighbour b's closest point when it is nearer. The neighbour order must not
    // change: ties keep the first candidate, which decides d_, t_ and cp.
    auto relax = [&](size_t a, size_t b, vec2 p) {
        if (d_[b] >= INF) return;
        float d = length(p - cp[b]);
        if (d < d_[a]) { d_[a] = d; cp[a] = cp[b]; t_[a] = t_[b]; }
    };
    const size_t W = size_t(w_);
    for (int j = 0; j < h_; ++j) {
        const size_t row = size_t(j) * W;
        for (int i = 0; i < w_; ++i) {  // W, SW, S, SE
            const size_t a = row + size_t(i);
            const vec2 p = origin_ + vec2(float(i), float(j)) * cell;
            if (i > 0) relax(a, a - 1, p);
            if (i > 0 && j > 0) relax(a, a - W - 1, p);
            if (j > 0) relax(a, a - W, p);
            if (i + 1 < w_ && j > 0) relax(a, a - W + 1, p);
        }
        for (int i = w_ - 2; i >= 0; --i) relax(row + size_t(i), row + size_t(i) + 1, origin_ + vec2(float(i), float(j)) * cell);  // E
    }
    for (int j = h_ - 1; j >= 0; --j) {
        const size_t row = size_t(j) * W;
        for (int i = w_ - 1; i >= 0; --i) {  // E, NE, N, NW
            const size_t a = row + size_t(i);
            const vec2 p = origin_ + vec2(float(i), float(j)) * cell;
            if (i + 1 < w_) relax(a, a + 1, p);
            if (i + 1 < w_ && j + 1 < h_) relax(a, a + W + 1, p);
            if (j + 1 < h_) relax(a, a + W, p);
            if (i > 0 && j + 1 < h_) relax(a, a + W - 1, p);
        }
        for (int i = 1; i < w_; ++i) relax(row + size_t(i), row + size_t(i) - 1, origin_ + vec2(float(i), float(j)) * cell);  // W
    }
    // Sign: even-odd scanline fill.
    std::vector<float> xs;
    for (int j = 0; j < h_; ++j) {
        float y = origin_.y + float(j) * cell;
        xs.clear();
        for (size_t k = 0; k < n; ++k) {
            vec2 a = poly[k], b = poly[(k + 1) % n];
            if ((a.y <= y && b.y > y) || (b.y <= y && a.y > y)) xs.push_back(a.x + (y - a.y) / (b.y - a.y) * (b.x - a.x));
        }
        std::sort(xs.begin(), xs.end());
        size_t c = 0;
        for (int i = 0; i < w_; ++i) {
            float x = origin_.x + float(i) * cell;
            while (c < xs.size() && xs[c] < x) ++c;
            if (c & 1) d_[size_t(j) * size_t(w_) + size_t(i)] *= -1.0f;
        }
    }
}

float Grid2D::sample(vec2 p) const {
    if (w_ == 0) return 1e9f;
    float fx = (p.x - origin_.x) / cell_, fy = (p.y - origin_.y) / cell_;
    float maxX = float(w_ - 1), maxY = float(h_ - 1);
    if (fx < 0 || fy < 0 || fx >= maxX || fy >= maxY) {
        float dx = std::max(std::max(-fx, fx - maxX), 0.0f), dy = std::max(std::max(-fy, fy - maxY), 0.0f);
        return band_ + std::sqrt(dx * dx + dy * dy) * cell_;
    }
    int i = int(fx), j = int(fy);
    float tx = fx - float(i), ty = fy - float(j);
    const float* r0 = &d_[size_t(j) * size_t(w_) + size_t(i)];
    const float* r1 = r0 + w_;
    return lerp(lerp(r0[0], r0[1], tx), lerp(r1[0], r1[1], tx), ty);
}

float Grid2D::sampleParam(vec2 p) const {
    if (w_ == 0) return 0.0f;
    // Bilinear, unwrapping the neighbours across the start of the outline so the parameter
    // (and details laid out along it) stays smooth; a stepped parameter would show as noisy
    // gradient normals.
    float fx = std::max(0.0f, std::min(float(w_ - 1) - 1e-3f, (p.x - origin_.x) / cell_));
    float fy = std::max(0.0f, std::min(float(h_ - 1) - 1e-3f, (p.y - origin_.y) / cell_));
    int i = int(fx), j = int(fy);
    float tx = fx - float(i), ty = fy - float(j);
    const float* r0 = &t_[size_t(j) * size_t(w_) + size_t(i)];
    const float* r1 = r0 + w_;
    float ref = r0[0], half = 0.5f * perimeter_;
    auto unwrap = [&](float v) { return v - ref > half ? v - perimeter_ : (ref - v > half ? v + perimeter_ : v); };
    return lerp(lerp(ref, unwrap(r0[1]), tx), lerp(unwrap(r1[0]), unwrap(r1[1]), tx), ty);
}

// ---------------------------------------------------------------------------------------------
MeshData meshSurfaceNets(const Field& fIn, const AABB& box, const MeshOptions& o, MeshStats* stats) {
    MeshData out;
    int evals = 0;
    auto f = [&](vec3 p) { ++evals; return fIn(p); };
    const float h = o.cell;
    vec3 size = box.hi - box.lo;
    const int nx = std::max(2, int(std::ceil(size.x / h)) + 1);
    const int ny = std::max(2, int(std::ceil(size.y / h)) + 1);
    const int nz = std::max(2, int(std::ceil(size.z / h)) + 1);
    const int B = std::max(2, o.block);
    const int bx = (nx - 1 + B - 1) / B, by = (ny - 1 + B - 1) / B, bz = (nz - 1 + B - 1) / B;
    auto pidx = [&](int i, int j, int k) { return (size_t(k) * size_t(ny) + size_t(j)) * size_t(nx) + size_t(i); };
    auto cidx = [&](int i, int j, int k) { return (size_t(k) * size_t(ny - 1) + size_t(j)) * size_t(nx - 1) + size_t(i); };
    auto P = [&](int i, int j, int k) { return box.lo + vec3(float(i), float(j), float(k)) * h; };

    std::vector<float> val(size_t(nx) * size_t(ny) * size_t(nz), 0.0f);  // valid where done[] is set
    std::vector<uint8_t> done(val.size(), 0);
    std::vector<uint8_t> active(size_t(bx) * size_t(by) * size_t(bz), 0);
    const float halfDiag = 0.5f * float(B) * h * std::sqrt(3.0f);
    for (int k = 0; k < bz; ++k)
        for (int j = 0; j < by; ++j)
            for (int i = 0; i < bx; ++i) {
                vec3 c = box.lo + (vec3(float(i), float(j), float(k)) + vec3(0.5f)) * (float(B) * h);
                float v = f(c);
                size_t bi = (size_t(k) * size_t(by) + size_t(j)) * size_t(bx) + size_t(i);
                active[bi] = std::fabs(v) <= halfDiag * o.lipschitz + h;
            }
    for (int k = 0; k < bz; ++k)
        for (int j = 0; j < by; ++j)
            for (int i = 0; i < bx; ++i) {
                if (!active[(size_t(k) * size_t(by) + size_t(j)) * size_t(bx) + size_t(i)]) continue;
                for (int z = k * B; z <= std::min(k * B + B, nz - 1); ++z)
                    for (int y = j * B; y <= std::min(j * B + B, ny - 1); ++y)
                        for (int x = i * B; x <= std::min(i * B + B, nx - 1); ++x) {
                            size_t id = pidx(x, y, z);
                            if (done[id]) continue;
                            val[id] = f(P(x, y, z));
                            done[id] = 1;
                        }
            }
    // Only cells of active blocks are meshed below, and every corner they read is evaluated.

    // Gradient (tetrahedral differences), unnormalised scale 4e.
    const float e = h * 0.12f;
    auto grad = [&](vec3 p) {
        const vec3 k0(1, -1, -1), k1(-1, -1, 1), k2(-1, 1, -1), k3(1, 1, 1);
        vec3 g = k0 * f(p + k0 * e) + k1 * f(p + k1 * e) + k2 * f(p + k2 * e) + k3 * f(p + k3 * e);
        return g / (4.0f * e);
    };

    // Vertices: one per cell with a sign change.
    std::vector<int> cellVert(size_t(nx - 1) * size_t(ny - 1) * size_t(nz - 1), -1);
    static const int corner[8][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}};
    static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    std::vector<int> cellList;
    for (int k = 0; k < bz; ++k)
        for (int j = 0; j < by; ++j)
            for (int i = 0; i < bx; ++i) {
                if (!active[(size_t(k) * size_t(by) + size_t(j)) * size_t(bx) + size_t(i)]) continue;
                for (int z = k * B; z < std::min(k * B + B, nz - 1); ++z)
                    for (int y = j * B; y < std::min(j * B + B, ny - 1); ++y)
                        for (int x = i * B; x < std::min(i * B + B, nx - 1); ++x) {
                            float cv[8];
                            int mask = 0;
                            for (int c = 0; c < 8; ++c) {
                                cv[c] = val[pidx(x + corner[c][0], y + corner[c][1], z + corner[c][2])];
                                if (cv[c] < 0.0f) mask |= 1 << c;
                            }
                            if (mask == 0 || mask == 255) continue;
                            vec3 base = P(x, y, z);
                            vec3 acc(0);
                            int cnt = 0;
                            for (auto& ed : edges) {
                                float a = cv[ed[0]], b = cv[ed[1]];
                                if ((a < 0.0f) == (b < 0.0f)) continue;
                                float t = a / (a - b);
                                // Braces, not parentheses: clang reads `vec3 pa(float(...), ...)` as a
                                // declaration of a function pa with parameters named after the
                                // expressions ("redefinition of parameter 'corner'"), GCC as the
                                // variable the line means. Braces are unambiguous for both.
                                vec3 pa{float(corner[ed[0]][0]), float(corner[ed[0]][1]), float(corner[ed[0]][2])};
                                vec3 pb{float(corner[ed[1]][0]), float(corner[ed[1]][1]), float(corner[ed[1]][2])};
                                acc += lerp(pa, pb, t);
                                ++cnt;
                            }
                            vec3 p = base + acc / float(cnt) * h;
                            vec3 n(0, 1, 0);
                            vec3 lo = base - vec3(h * 0.5f), hi = base + vec3(h * 1.5f);
                            for (int it = 0; it < o.projectIterations; ++it) {
                                float d = f(p);
                                vec3 g = grad(p);
                                float g2 = dot(g, g);
                                if (g2 < 1e-12f) break;
                                n = g;
                                p = p - g * (d / g2);
                                p = vec3(clamp(p.x, lo.x, hi.x), clamp(p.y, lo.y, hi.y), clamp(p.z, lo.z, hi.z));
                            }
                            if (o.projectIterations == 0) n = grad(p);
                            Vertex v;
                            v.pos = p;
                            v.normal = normalize(n);
                            cellVert[cidx(x, y, z)] = int(out.vertices.size());
                            out.vertices.push_back(v);
                            cellList.push_back(int(cidx(x, y, z)));
                        }
            }

    auto quad = [&](int a, int b, int c, int d, bool flip) {
        if (a < 0 || b < 0 || c < 0 || d < 0) return;
        if (flip) std::swap(b, d);
        const vec3 &pa = out.vertices[size_t(a)].pos, &pb = out.vertices[size_t(b)].pos, &pc = out.vertices[size_t(c)].pos,
                   &pd = out.vertices[size_t(d)].pos;
        if (length2(pa - pc) <= length2(pb - pd)) {
            for (int i : {a, b, c, a, c, d}) out.indices.push_back(uint32_t(i));
        } else {
            for (int i : {a, b, d, b, c, d}) out.indices.push_back(uint32_t(i));
        }
    };
    for (int ci : cellList) {
        int x = ci % (nx - 1), y = (ci / (nx - 1)) % (ny - 1), z = ci / ((nx - 1) * (ny - 1));
        float v0 = val[pidx(x, y, z)];
        bool in0 = v0 < 0.0f;
        // +X edge
        if (y > 0 && z > 0 && (val[pidx(x + 1, y, z)] < 0.0f) != in0)
            quad(cellVert[cidx(x, y - 1, z - 1)], cellVert[cidx(x, y, z - 1)], cellVert[cidx(x, y, z)], cellVert[cidx(x, y - 1, z)], !in0);
        // +Y edge
        if (x > 0 && z > 0 && (val[pidx(x, y + 1, z)] < 0.0f) != in0)
            quad(cellVert[cidx(x - 1, y, z - 1)], cellVert[cidx(x - 1, y, z)], cellVert[cidx(x, y, z)], cellVert[cidx(x, y, z - 1)], !in0);
        // +Z edge
        if (x > 0 && y > 0 && (val[pidx(x, y, z + 1)] < 0.0f) != in0)
            quad(cellVert[cidx(x - 1, y - 1, z)], cellVert[cidx(x, y - 1, z)], cellVert[cidx(x, y, z)], cellVert[cidx(x - 1, y, z)], !in0);
    }
    if (stats) {
        stats->evaluations = evals;
        stats->vertices = int(out.vertices.size());
        stats->triangles = int(out.indices.size() / 3);
    }
    return out;
}

}  // namespace sdf
