// General SDF mesher: surface-following Surface Nets + quadric-error (QEM) decimation.
//
//  1. From seed points inside the shape, rays find surface crossings; the grid cells there seed a
//     flood fill that only visits cells the surface passes through (cost ~ surface area / cell^2,
//     never the volume). Corner values are cached in a hash map.
//  2. One vertex per surface cell (mass point of the edge crossings), Newton-projected onto the
//     zero set; one quad per sign-changing grid edge.
//  3. Garland-Heckbert edge collapses in order of quadric error. Every collapsed vertex is
//     re-projected onto the SDF, so the decimated mesh stays exactly on the implicit surface.
//     Collapses that flip a face, break the link condition or create over-long edges are refused.
//  4. Normals from the SDF gradient (perfectly smooth shading), tangents along o.tangentAxis.
#include "sdf.h"
#include "../core/log.h"
#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <unordered_map>

using namespace m;

namespace character {
namespace sdf {
namespace {

inline uint64_t key3(int i, int j, int k) {
    return (uint64_t(uint32_t(i + (1 << 20)) & 0x1FFFFF) << 42) | (uint64_t(uint32_t(j + (1 << 20)) & 0x1FFFFF) << 21) |
           uint64_t(uint32_t(k + (1 << 20)) & 0x1FFFFF);
}

struct Grid {
    const Fn& f;
    float h;
    vec3 origin;
    std::unordered_map<uint64_t, float> corner;
    Grid(const Fn& fn, float cell) : f(fn), h(cell), origin(cell * 0.371f, cell * 0.213f, cell * 0.529f) {}
    vec3 pos(int i, int j, int k) const { return origin + vec3(float(i), float(j), float(k)) * h; }
    float value(int i, int j, int k) {
        auto [it, inserted] = corner.try_emplace(key3(i, j, k), 0.0f);  // one hash lookup
        if (inserted) {
            float v = f(pos(i, j, k));
            if (v == 0.0f) v = 1e-9f;  // no exact zeros: every corner is strictly inside or outside
            it->second = v;
        }
        return it->second;
    }
};

// ---- Quadric error decimation ------------------------------------------------------------------
// Quadrics are built in a local frame (centre + scale) so the 3x3 solve is well conditioned.
struct Quadric {
    double q[10] = {};  // a2 ab ac ad b2 bc bd c2 cd d2
    double w = 0.0;     // accumulated area weight
    void addPlane(double a, double b, double c, double d, double wt) {
        q[0] += wt * a * a; q[1] += wt * a * b; q[2] += wt * a * c; q[3] += wt * a * d;
        q[4] += wt * b * b; q[5] += wt * b * c; q[6] += wt * b * d;
        q[7] += wt * c * c; q[8] += wt * c * d; q[9] += wt * d * d;
        w += wt;
    }
    void add(const Quadric& o) {
        for (int i = 0; i < 10; ++i) q[i] += o.q[i];
        w += o.w;
    }
    double eval(double x, double y, double z) const {
        return q[0] * x * x + 2 * q[1] * x * y + 2 * q[2] * x * z + 2 * q[3] * x + q[4] * y * y + 2 * q[5] * y * z + 2 * q[6] * y +
               q[7] * z * z + 2 * q[8] * z + q[9];
    }
    bool optimum(double& x, double& y, double& z) const {
        if (w <= 0.0) return false;
        double a = q[0] / w, b = q[1] / w, c = q[2] / w, d = q[4] / w, e = q[5] / w, f = q[7] / w;
        double r0 = -q[3] / w, r1 = -q[6] / w, r2 = -q[8] / w;
        double det = a * (d * f - e * e) - b * (b * f - c * e) + c * (b * e - c * d);
        if (std::fabs(det) < 1e-4) return false;  // (near) flat or cylindrical neighbourhood
        x = (r0 * (d * f - e * e) - b * (r1 * f - e * r2) + c * (r1 * e - d * r2)) / det;
        y = (a * (r1 * f - e * r2) - r0 * (b * f - c * e) + c * (b * r2 - r1 * c)) / det;
        z = (a * (d * r2 - r1 * e) - b * (b * r2 - r1 * c) + r0 * (b * e - c * d)) / det;
        return true;
    }
};

struct Decimator {
    const Fn& f;
    const VolumeOptions& o;
    vec3 centre;
    float scale = 1.0f;  // local units = (p - centre) * scale
    std::vector<vec3> pos, nrm;
    std::vector<uint32_t> tri;  // 3 per face
    std::vector<char> faceAlive;
    std::vector<std::vector<uint32_t>> vfaces;
    std::vector<Quadric> Q;
    std::vector<uint32_t> stamp;
    std::vector<char> vAlive;
    std::vector<uint32_t> na, nb, nbrs;
    size_t stat[8] = {};  // collapsed, rejected: link x2, valence, length, projection, faces; pushes
    struct Cand {
        float cost;
        uint32_t a, b, sa, sb;
        vec3 p;
        bool operator<(const Cand& o2) const { return cost > o2.cost; }  // min-heap
    };
    std::vector<Cand> heap;  // binary min-heap (std::*_heap with Cand::operator<)
    bool bulk = false;

    Decimator(const Fn& fn, const VolumeOptions& opt) : f(fn), o(opt) {}

    vec3 faceNormal(uint32_t fi) const {
        const vec3 &a = pos[tri[fi * 3]], &b = pos[tri[fi * 3 + 1]], &c = pos[tri[fi * 3 + 2]];
        return cross(b - a, c - a);
    }
    void init() {
        size_t nf = tri.size() / 3;
        vec3 lo(1e9f), hi(-1e9f);
        for (const vec3& p : pos) { lo = min(lo, p); hi = max(hi, p); }
        centre = (lo + hi) * 0.5f;
        scale = 1.0f / std::max(o.cell * 8.0f, 1e-6f);
        faceAlive.assign(nf, 1);
        vfaces.assign(pos.size(), {});
        // Room for every face at its corners plus a few collapse appends (no regrowth).
        std::vector<uint32_t> faceCount(pos.size(), 0);
        for (uint32_t v : tri) ++faceCount[v];
        for (size_t i = 0; i < pos.size(); ++i) vfaces[i].reserve(faceCount[i] + 4);
        Q.assign(pos.size(), Quadric());
        stamp.assign(pos.size(), 0);
        vAlive.assign(pos.size(), 1);
        nrm.resize(pos.size());
        for (size_t i = 0; i < pos.size(); ++i) nrm[i] = normalize(gradient(f, pos[i], o.gradientStep));
        for (uint32_t fi = 0; fi < nf; ++fi) {
            // A zero-area face has no plane to add, but it stays listed at its corners: collapses
            // re-index, kill and check only the faces they find there.
            for (int k = 0; k < 3; ++k) vfaces[tri[fi * 3 + k]].push_back(fi);
            vec3 n = faceNormal(fi);
            float len = length(n);
            if (len < 1e-20f) continue;
            n = n / len;
            vec3 p0 = (pos[tri[fi * 3]] - centre) * scale;
            double d = -dot(n, p0);
            double area = double(len) * 0.5 * double(scale) * double(scale);
            for (int k = 0; k < 3; ++k) Q[tri[fi * 3 + k]].addPlane(n.x, n.y, n.z, d, area);
        }
        bulk = true;
        heap.reserve(tri.size() * 2);
        for (uint32_t fi = 0; fi < nf; ++fi)
            for (int k = 0; k < 3; ++k) {
                uint32_t a = tri[fi * 3 + k], b = tri[fi * 3 + (k + 1) % 3];
                if (a < b) push(a, b);
            }
        bulk = false;
        std::make_heap(heap.begin(), heap.end());
    }
    // Candidate position (optimum, else the best of the endpoints / midpoint) and its cost as a mean
    // squared distance (m^2) to the merged planes. Projection onto the SDF happens at collapse.
    void push(uint32_t a, uint32_t b) {
        Quadric q = Q[a];
        q.add(Q[b]);
        vec3 la = (pos[a] - centre) * scale, lb = (pos[b] - centre) * scale;
        double x, y, z, best;
        vec3 p;
        vec3 mid = (la + lb) * 0.5f;
        if (q.optimum(x, y, z) && length(vec3(float(x), float(y), float(z)) - mid) < 0.75f * length(la - lb)) {
            p = vec3(float(x), float(y), float(z));
            best = q.eval(x, y, z);
        } else {
            p = mid;
            best = q.eval(mid.x, mid.y, mid.z);
            double ca = q.eval(la.x, la.y, la.z), cb = q.eval(lb.x, lb.y, lb.z);
            if (ca < best) { best = ca; p = la; }
            if (cb < best) { best = cb; p = lb; }
        }
        double cost = std::max(0.0, best) / std::max(q.w, 1e-12) / (double(scale) * double(scale));
        ++stat[7];
        heap.push_back({float(cost), a, b, stamp[a], stamp[b], p / scale + centre});
        if (!bulk) std::push_heap(heap.begin(), heap.end());
    }
    static void uniq(std::vector<uint32_t>& v) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    }
    bool collapse(const Cand& c) {
        uint32_t a = c.a, b = c.b;
        // Link condition: the only common neighbours are the two apexes of the shared faces.
        na.clear();
        nb.clear();
        int shared = 0;
        for (uint32_t fi : vfaces[a]) {
            if (!faceAlive[fi]) continue;
            bool hasB = false;
            for (int k = 0; k < 3; ++k) hasB |= tri[fi * 3 + k] == b;
            shared += hasB;
            for (int k = 0; k < 3; ++k) na.push_back(tri[fi * 3 + k]);
        }
        if (shared != 2) { ++stat[1]; return false; }
        for (uint32_t fi : vfaces[b])
            if (faceAlive[fi])
                for (int k = 0; k < 3; ++k) nb.push_back(tri[fi * 3 + k]);
        uniq(na);
        uniq(nb);
        int common = 0;
        for (uint32_t v : na)
            if (v != a && v != b && std::binary_search(nb.begin(), nb.end(), v)) ++common;
        if (common != 2) { ++stat[2]; return false; }
        if (na.size() + nb.size() > 6 + 11) { ++stat[3]; return false; }  // keep valence <= 11 (no fans)
        // Edge length limit, final position on the implicit surface, no flips, no slivers.
        float maxE2 = o.maxEdge * o.maxEdge;
        for (uint32_t v : na) if (v != a && v != b && length2(pos[v] - c.p) > maxE2) { ++stat[4]; return false; }
        for (uint32_t v : nb) if (v != a && v != b && length2(pos[v] - c.p) > maxE2) { ++stat[4]; return false; }
        vec3 p = project(f, c.p, o.gradientStep, 1);
        if (length(p - c.p) > 4.0f * o.maxError + o.cell * 0.25f) { ++stat[5]; return false; }
        vec3 np = normalize(nrm[a] + nrm[b]);
        auto checkFaces = [&](uint32_t moved) {
            for (uint32_t fi : vfaces[moved]) {
                if (!faceAlive[fi]) continue;
                uint32_t v0 = tri[fi * 3], v1 = tri[fi * 3 + 1], v2 = tri[fi * 3 + 2];
                if ((v0 == a || v1 == a || v2 == a) && (v0 == b || v1 == b || v2 == b)) continue;  // removed
                vec3 q0 = v0 == moved ? p : pos[v0], q1 = v1 == moved ? p : pos[v1], q2 = v2 == moved ? p : pos[v2];
                vec3 after = cross(q1 - q0, q2 - q0);
                float la = length(after);
                if (la < 1e-14f) return false;
                // Stay close to the true surface orientation (catches flips and folds).
                vec3 g = (v0 == moved ? np : nrm[v0]) + (v1 == moved ? np : nrm[v1]) + (v2 == moved ? np : nrm[v2]);
                vec3 before = faceNormal(fi);
                if (dot(after, before) < 0.2f * la * length(before)) return false;
                if (dot(after, g) * dot(before, g) < 0.0f) return false;
                float e0 = length2(q1 - q0), e1 = length2(q2 - q1), e2 = length2(q0 - q2);
                float emax2 = std::max(e0, std::max(e1, e2));
                if (la / emax2 < 0.12f) return false;  // min angle ~7 degrees
            }
            return true;
        };
        if (!checkFaces(a) || !checkFaces(b)) { ++stat[6]; return false; }
        // Apply.
        for (uint32_t fi : vfaces[b]) {
            if (!faceAlive[fi]) continue;
            bool hasA = false;
            for (int k = 0; k < 3; ++k) hasA |= tri[fi * 3 + k] == a;
            if (hasA) {
                faceAlive[fi] = 0;
                continue;
            }
            for (int k = 0; k < 3; ++k)
                if (tri[fi * 3 + k] == b) tri[fi * 3 + k] = a;
            vfaces[a].push_back(fi);
        }
        vAlive[b] = 0;
        vfaces[b].clear();
        pos[a] = p;
        nrm[a] = np;
        Q[a].add(Q[b]);
        std::vector<uint32_t>& fa = vfaces[a];
        fa.erase(std::remove_if(fa.begin(), fa.end(), [&](uint32_t fi) { return !faceAlive[fi]; }), fa.end());
        uniq(fa);
        nbrs.clear();
        for (uint32_t fi : fa)
            for (int k = 0; k < 3; ++k)
                if (tri[fi * 3 + k] != a) nbrs.push_back(tri[fi * 3 + k]);
        uniq(nbrs);
        for (uint32_t v : nbrs) {
            std::vector<uint32_t>& fv = vfaces[v];
            fv.erase(std::remove_if(fv.begin(), fv.end(), [&](uint32_t fi) { return !faceAlive[fi]; }), fv.end());
        }
        ++stamp[a];
        for (uint32_t v : nbrs) push(std::min(a, v), std::max(a, v));
        return true;
    }
    void run() {
        const double limit = double(o.maxError) * double(o.maxError);
        while (!heap.empty()) {
            std::pop_heap(heap.begin(), heap.end());
            Cand c = heap.back();
            heap.pop_back();
            if (c.cost > limit) break;
            if (!vAlive[c.a] || !vAlive[c.b]) continue;
            if (c.sa != stamp[c.a] || c.sb != stamp[c.b]) continue;  // superseded by a newer entry
            stat[0] += collapse(c);
        }
    }
    void output(MeshData& d) {
        std::vector<uint32_t> remap(pos.size(), UINT32_MAX);
        d.vertices.clear();
        d.indices.clear();
        for (size_t fi = 0; fi < faceAlive.size(); ++fi) {
            if (!faceAlive[fi]) continue;
            for (int k = 0; k < 3; ++k) {
                uint32_t v = tri[fi * 3 + k];
                if (remap[v] == UINT32_MAX) {
                    remap[v] = uint32_t(d.vertices.size());
                    Vertex vx;
                    vx.pos = pos[v];
                    d.vertices.push_back(vx);
                }
                d.indices.push_back(remap[v]);
            }
        }
    }
};

}  // namespace

MeshData meshVolume(const Fn& f, const std::vector<vec3>& seeds, const VolumeOptions& oIn) {
#ifdef __ANDROID__
    // Phones: coarser grid and decimation tolerance (see meshSegment in sdf_mesh.cpp).
    VolumeOptions o = oIn;
    o.cell *= 1.4f;
    o.maxError *= 3.0f;
    o.maxEdge *= 1.5f;
#else
    const VolumeOptions& o = oIn;
#endif
    const auto t0 = std::chrono::steady_clock::now();
    const float h = o.cell;
    Grid g(f, h);
    std::unordered_map<uint64_t, uint32_t> cellVertex;
    std::vector<vec3> verts;
    std::vector<int> cellIdx;  // i,j,k triples of surface cells, in vertex order (the flood fill queue)
    std::unordered_map<uint64_t, char> visited;

    static const int cornerOff[8][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}};
    static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    auto cellValues = [&](int i, int j, int k, float v[8]) {
        int mask = 0;
        for (int c = 0; c < 8; ++c) {
            v[c] = g.value(i + cornerOff[c][0], j + cornerOff[c][1], k + cornerOff[c][2]);
            mask |= (v[c] < 0.0f) << c;
        }
        return mask;
    };
    auto enqueue = [&](int i, int j, int k) {
        if (visited.try_emplace(key3(i, j, k), 1).second) {  // no node allocated for a visited cell
            cellIdx.push_back(i);
            cellIdx.push_back(j);
            cellIdx.push_back(k);
        }
    };
    // Seeds: rays from interior points.
    int seedCells = 0;
    for (vec3 s : seeds) {
        if (f(s) >= 0.0f) {
            LOGW("sdf::meshVolume(%s): seed (%.4f %.4f %.4f) is outside the shape", o.name, s.x, s.y, s.z);
            continue;
        }
        for (int dirI = 0; dirI < 26; ++dirI) {
            int dx = dirI % 3 - 1, dy = (dirI / 3) % 3 - 1, dz = dirI / 9 - 1;
            if (dirI >= 13) { dx = (dirI + 1) % 3 - 1; dy = ((dirI + 1) / 3) % 3 - 1; dz = (dirI + 1) / 9 - 1; }
            vec3 dir = normalize(vec3(float(dx), float(dy), float(dz)));
            float t = 0, prev = 0;
            float v = f(s);
            bool hit = false;
            for (int it = 0; it < 2000 && t < 2.0f; ++it) {
                prev = t;
                t += clamp(-v * 0.5f, h * 0.05f, h * 0.5f);
                v = f(s + dir * t);
                if (v >= 0.0f) { hit = true; break; }
            }
            if (!hit) continue;
            vec3 p = s + dir * (0.5f * (prev + t));
            int ci = int(std::floor((p.x - g.origin.x) / h)), cj = int(std::floor((p.y - g.origin.y) / h)),
                ck = int(std::floor((p.z - g.origin.z) / h));
            for (int a = -1; a <= 1; ++a)
                for (int b = -1; b <= 1; ++b)
                    for (int c = -1; c <= 1; ++c) {
                        float vv[8];
                        int mask = cellValues(ci + a, cj + b, ck + c, vv);
                        if (mask != 0 && mask != 255) { enqueue(ci + a, cj + b, ck + c); ++seedCells; }
                    }
        }
    }
    // Flood fill over surface cells through faces crossed by the surface.
    static const int faceCorners[6][4] = {{0, 2, 4, 6}, {1, 3, 5, 7}, {0, 1, 4, 5}, {2, 3, 6, 7}, {0, 1, 2, 3}, {4, 5, 6, 7}};
    static const int faceDir[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
    size_t head = 0;
    std::vector<int> surfaceCells;
    while (head < cellIdx.size() / 3) {
        size_t idx = head++;
        int i = cellIdx[idx * 3], j = cellIdx[idx * 3 + 1], k = cellIdx[idx * 3 + 2];
        float v[8];
        int mask = cellValues(i, j, k, v);
        if (mask == 0 || mask == 255) continue;
        // Vertex: mass point of the edge crossings, then projected on the surface.
        vec3 acc(0);
        int n = 0;
        for (auto& e : edges) {
            float a = v[e[0]], b = v[e[1]];
            if ((a < 0) == (b < 0)) continue;
            float t = a / (a - b);
            vec3 pa = g.pos(i + cornerOff[e[0]][0], j + cornerOff[e[0]][1], k + cornerOff[e[0]][2]);
            vec3 pb = g.pos(i + cornerOff[e[1]][0], j + cornerOff[e[1]][1], k + cornerOff[e[1]][2]);
            acc += lerp(pa, pb, t);
            ++n;
        }
        vec3 p = acc / float(n);
        vec3 pp = project(f, p, o.gradientStep, 1);
        if (length(pp - p) < h * 1.5f) p = pp;
        cellVertex[key3(i, j, k)] = uint32_t(verts.size());
        verts.push_back(p);
        surfaceCells.push_back(i);
        surfaceCells.push_back(j);
        surfaceCells.push_back(k);
        for (int fc = 0; fc < 6; ++fc) {
            int fm = 0;
            for (int c = 0; c < 4; ++c) fm |= (v[faceCorners[fc][c]] < 0.0f) << c;
            if (fm != 0 && fm != 15) enqueue(i + faceDir[fc][0], j + faceDir[fc][1], k + faceDir[fc][2]);
        }
        if (verts.size() > 3000000) {
            LOGE("sdf::meshVolume(%s): runaway surface (cell too small?)", o.name);
            break;
        }
    }
    // Quads around every sign-changing grid edge (each edge handled by its lowest cell).
    Decimator dec(f, o);
    dec.pos = verts;
    auto vtx = [&](int i, int j, int k) -> int64_t {
        auto it = cellVertex.find(key3(i, j, k));
        return it == cellVertex.end() ? -1 : int64_t(it->second);
    };
    for (size_t s = 0; s < surfaceCells.size(); s += 3) {
        int i = surfaceCells[s], j = surfaceCells[s + 1], k = surfaceCells[s + 2];
        // x-edge at corners (i..i+1, j+1, k+1): cells (i, j..j+1, k..k+1)
        const int axes[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        for (int a = 0; a < 3; ++a) {
            int c0[3] = {i + 1, j + 1, k + 1};
            c0[a] = a == 0 ? i : (a == 1 ? j : k);
            int c1[3] = {c0[0] + axes[a][0], c0[1] + axes[a][1], c0[2] + axes[a][2]};
            float f0 = g.value(c0[0], c0[1], c0[2]), f1 = g.value(c1[0], c1[1], c1[2]);
            if ((f0 < 0) == (f1 < 0)) continue;
            // The 4 cells around this edge: vary the two other axes by 0/-1 from c0.
            int u = (a + 1) % 3, w = (a + 2) % 3;
            int64_t q[4];
            bool ok = true;
            const int du[4] = {0, 1, 1, 0}, dw[4] = {0, 0, 1, 1};
            for (int c = 0; c < 4; ++c) {
                int cell[3] = {c0[0], c0[1], c0[2]};
                cell[a] = c0[a];
                cell[u] = c0[u] - 1 + du[c];
                cell[w] = c0[w] - 1 + dw[c];
                q[c] = vtx(cell[0], cell[1], cell[2]);
                ok &= q[c] >= 0;
            }
            if (!ok) continue;
            uint32_t A = uint32_t(q[0]), B = uint32_t(q[1]), C = uint32_t(q[2]), D = uint32_t(q[3]);
            if (f0 > 0) std::swap(B, D);
            if (length2(verts[A] - verts[C]) < length2(verts[B] - verts[D])) {
                dec.tri.insert(dec.tri.end(), {A, B, C, A, C, D});
            } else {
                dec.tri.insert(dec.tri.end(), {A, B, D, B, C, D});
            }
        }
    }
    size_t rawTris = dec.tri.size() / 3;
    const auto t1 = std::chrono::steady_clock::now();
    dec.init();
    dec.run();
    MeshData d;
    dec.output(d);
    MeshOptions mo;
    mo.tangentAxis = o.tangentAxis;
    mo.gradientStep = o.gradientStep;
    const auto t2 = std::chrono::steady_clock::now();
    surfaceFrames(d, f, mo);
    if (std::getenv("SCACELITH_SDF_DEBUG")) {
        auto ms = [](std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        LOGI("meshVolume(%s): %d evals, %d cells, %d -> %d tris (seed cells %d) nets %.0f ms, decimate %.0f ms, frames %.0f ms", o.name,
             int(g.corner.size()), int(verts.size()), int(rawTris), int(d.indices.size() / 3), int(seedCells), ms(t0, t1), ms(t1, t2),
             ms(t2, std::chrono::steady_clock::now()));
        LOGI("  collapses %d, rejected: shared %d link %d valence %d length %d proj %d faces %d; pushes %d", int(dec.stat[0]),
             int(dec.stat[1]), int(dec.stat[2]), int(dec.stat[3]), int(dec.stat[4]), int(dec.stat[5]), int(dec.stat[6]),
             int(dec.stat[7]));
    }
    return d;
}

}  // namespace sdf
}  // namespace character
