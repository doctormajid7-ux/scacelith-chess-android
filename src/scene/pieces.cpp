// Procedural Staunton chess set. All profiles are authored in millimetres and converted to
// meters at the end. See pieces.h for conventions.
#include "pieces.h"
#include "piece_profile.h"
#include "sdf_mesher.h"
#include "../core/log.h"
#include "../game/layout.h"
#include <chrono>
#include <cmath>
#include <unordered_map>

using namespace m;

namespace {

#ifdef __ANDROID__
// Phones: ~4x fewer triangles. The 32 pieces are drawn every frame in the main pass and in each
// shadow cascade, and at full detail (~2 M triangles) they alone cost a phone GPU more time than
// all the pixels of the frame; at a phone's viewing size the coarser facets do not show.
constexpr int LATHE_SEGMENTS = 48;
constexpr float MAX_SEG = 1.2f;   // profile resampling (mm)
constexpr float SDF_CELL_SCALE = 1.7f;   // grid spacing of the SDF parts (heads, crowns, cross)
#else
constexpr int LATHE_SEGMENTS = 96;
constexpr float MAX_SEG = 0.6f;   // profile resampling (mm)
constexpr float SDF_CELL_SCALE = 1.0f;
#endif
constexpr float FILLET = 0.28f;   // default edge rounding (mm)

float heightMm(int type) { return layout::PIECE_HEIGHT[type] * 1000.0f; }
float radiusMm(int type) { return layout::PIECE_BASE_RADIUS[type] * 1000.0f; }

float sdRoundBox2(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + vec2(r);
    return length(max(q, vec2(0))) + std::min(std::max(q.x, q.y), 0.0f) - r;
}

// ---- turned elements ------------------------------------------------------------------------
// Weighted Staunton foot: felt recess, bottom arris, plinth, large torus, step, cove and a small
// bead. k scales the vertical/horizontal details with the base radius.
void addBase(Profile& p, float R) {
    float k = R / 17.5f;
    p.to(0, 0.95f);
    p.to(R - 2.3f * k, 0.95f, 0.2f);
    p.to(R - 2.0f * k, 0.45f, 0.2f);
    p.to(R - 0.6f, 0.45f, 0.4f);
    p.to(R, 1.2f, 0.35f);
    p.to(R, 2.5f * k, 0.35f);
    p.to(R - 0.9f * k, 2.75f * k, 0.25f);
    p.ellipse(vec2(R - 3.3f * k, 5.15f * k), vec2(2.55f * k, 2.5f * k), -62, 96);
    p.to(R - 4.25f * k, 7.75f * k, 0.2f);
    p.cubic(vec2(R - 5.3f * k, 7.85f * k), vec2(R - 5.75f * k, 8.4f * k), vec2(R - 5.75f * k, 9.15f * k));
    p.ellipse(vec2(R - 5.75f * k, 9.75f * k), vec2(0.62f * k, 0.6f * k), -90, 90);
    p.to(R - 6.1f * k, 10.45f * k, 0.25f);
}

// Stem from the current point to (rTop, yTop): concave flare out of the base, gentle taper.
void addStem(Profile& p, float rTop, float yTop, float flare = 0.5f) {
    vec2 s = p.last();
    float dr = s.x - rTop, dy = yTop - s.y;
    p.cubic(vec2(s.x - dr * flare, s.y + dy * 0.1f), vec2(rTop + dr * 0.06f, s.y + dy * 0.45f), vec2(rTop, yTop));
}

// Collar: small bead, rounded saucer disc, small bead. Starts at the stem top. Returns the y of
// the top of the upper bead (the lathe then closes to the axis there).
float addCollar(Profile& p, float rb1, float rd, float td, float rb2) {
    vec2 s = p.last();
    float y0 = s.y;
    p.ellipse(vec2(s.x, y0 + 0.6f), vec2(rb1 - s.x, 0.6f), -90, 90);
    p.to(rd - td * 0.5f, y0 + 1.35f, 0.3f);
    p.ellipse(vec2(rd - td * 0.5f, y0 + 1.35f + td * 0.5f), vec2(td * 0.5f, td * 0.5f), -90, 90);
    float xi = s.x + 0.35f;
    p.to(xi, y0 + 1.5f + td, 0.3f);
    p.ellipse(vec2(xi, y0 + 2.05f + td), vec2(rb2 - xi, 0.55f), -90, 90);
    return y0 + 2.6f + td;
}

MeshData feltDisc(float R) {
    float k = R / 17.5f;
    float rf = R - 2.45f * k;
    Profile p;
    p.to(0, 0).to(rf, 0, 0.3f).to(rf, 1.0f, 0.1f).to(0, 1.0f);
    return latheProfile(finishProfile(p, 0.2f, 0.6f), 64, 1.0f);
}

// Rotationally symmetric SDF part from a closed (r, y) outline.
struct LatheField {
    sdf::Grid2D g;
    float operator()(vec3 p) const { return g.sample(vec2(std::sqrt(p.x * p.x + p.z * p.z), p.y)); }
};

m::AABB outlineBox(const std::vector<vec2>& poly, float pad) {
    float rmax = 0, y0 = 1e9f, y1 = -1e9f;
    for (auto& q : poly) { rmax = std::max(rmax, q.x); y0 = std::min(y0, q.y); y1 = std::max(y1, q.y); }
    m::AABB b;
    b.add(vec3(-rmax - pad, y0 - pad, -rmax - pad));
    b.add(vec3(rmax + pad, y1 + pad, rmax + pad));
    return b;
}

// Cylindrical uv (u = angle/2pi matching the lathe, v = y/H) with seam vertices duplicated.
void cylindricalUV(MeshData& d, float H) {
    for (auto& v : d.vertices) {
        float a = std::atan2(-v.pos.z, v.pos.x);
        if (a < 0) a += TAU;
        v.uv = vec2(a / TAU, v.pos.y / H);
    }
    std::unordered_map<uint32_t, uint32_t> dup;
    for (size_t i = 0; i + 2 < d.indices.size(); i += 3) {
        float u0 = d.vertices[d.indices[i]].uv.x, u1 = d.vertices[d.indices[i + 1]].uv.x, u2 = d.vertices[d.indices[i + 2]].uv.x;
        float lo = std::min(u0, std::min(u1, u2)), hi = std::max(u0, std::max(u1, u2));
        if (hi - lo < 0.5f) continue;
        for (int k = 0; k < 3; ++k) {
            uint32_t idx = d.indices[i + size_t(k)];
            if (d.vertices[idx].uv.x >= 0.5f) continue;
            auto it = dup.find(idx);
            if (it == dup.end()) {
                Vertex v = d.vertices[idx];
                v.uv.x += 1.0f;
                it = dup.emplace(idx, uint32_t(d.vertices.size())).first;
                d.vertices.push_back(v);
            }
            d.indices[i + size_t(k)] = it->second;
        }
    }
}

double nowSec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

MeshData meshField(const sdf::Field& f, const AABB& box, float cell, float lipschitz, float H, const char* what) {
    sdf::MeshOptions o;
    o.cell = cell * SDF_CELL_SCALE;
    o.lipschitz = lipschitz;
    sdf::MeshStats st;
    double t0 = nowSec();
    MeshData d = sdf::meshSurfaceNets(f, box, o, &st);
    cylindricalUV(d, H);
    LOGD("pieces: %s SDF %d tris, %d evals, %.1f ms", what, st.triangles, st.evaluations, (nowSec() - t0) * 1000.0);
    return d;
}

// ---- pawn -------------------------------------------------------------------------------------
MeshData buildPawn() {
    const float R = radiusMm(1), H = heightMm(1);
    Profile p;
    addBase(p, R);
    addStem(p, 4.6f, 23.6f, 0.55f);
    // Saucer collar.
    p.to(5.1f, 24.6f, 0.35f);
    p.to(8.1f, 24.85f, 0.25f);
    p.ellipse(vec2(8.1f, 25.85f), vec2(1.0f, 1.0f), -90, 90);
    p.to(5.2f, 27.05f, 0.3f);
    // Neck and ball.
    float br = 8.25f, by = H - br;
    float a0 = -58.0f;
    vec2 ballStart(br * std::cos(a0 * DEG), by + br * std::sin(a0 * DEG));
    p.spline({vec2(4.35f, 28.3f), vec2(3.95f, 30.4f), ballStart});
    p.arc(vec2(0, by), br, a0, 90);
    return latheProfile(finishProfile(p, FILLET, MAX_SEG), LATHE_SEGMENTS, H);
}

// ---- rook -------------------------------------------------------------------------------------
MeshData buildRook() {
    const float R = radiusMm(4), H = heightMm(4);
    Profile p;
    addBase(p, R);
    // Tapered, waisted tower body.
    p.cubic(vec2(10.0f, 12.9f), vec2(8.85f, 21.5f), vec2(8.75f, 30.6f));
    // Collar ring under the turret.
    p.to(9.3f, 31.2f, 0.3f);
    p.to(10.6f, 31.4f, 0.25f);
    p.ellipse(vec2(10.6f, 32.4f), vec2(1.0f, 1.0f), -90, 90);
    p.to(9.8f, 33.6f, 0.25f);
    p.to(0.0f, 33.6f);
    MeshData body = latheProfile(finishProfile(p, FILLET, MAX_SEG), LATHE_SEGMENTS, H);

    // Turret (SDF): flared tower, recessed top, five crenels.
    Profile t;
    t.to(0, 33.1f).to(10.15f, 33.1f, 0.0f).to(10.15f, 34.3f, 0.3f);
    t.cubic(vec2(10.25f, 40.0f), vec2(11.7f, 46.2f), vec2(12.25f, 50.8f));
    t.to(12.35f, H, 0.5f);
    t.to(9.7f, H, 0.45f);
    t.to(9.25f, 51.2f, 0.6f);
    t.cubic(vec2(6.2f, 50.6f), vec2(3.0f, 50.45f), vec2(0.0f, 50.45f));
    std::vector<vec2> poly = finishClosed(t, 0.3f, 0.3f);
    LatheField lf;
    lf.g.build(poly, 0.04f, 2.0f);
    const int N = 5;
    const float halfW = 2.05f, yb = 50.7f, rr = 0.45f;
    auto f = [&](vec3 q) {
        float r = std::sqrt(q.x * q.x + q.z * q.z);
        float d = lf.g.sample(vec2(r, q.y));
        if (q.y < yb - 2.0f || d > 2.0f) return d;
        float th = std::atan2(q.z, q.x), sector = TAU / float(N);
        float tl = th - sector * std::floor(th / sector + 0.5f);
        float tt = r * std::sin(tl);
        float slot = sdRoundBox2(vec2(tt, q.y - (yb + 20.0f)), vec2(halfW, 20.0f), rr);
        return sdf::smax(d, -slot, 0.55f);
    };
    MeshData head = meshField(f, outlineBox(poly, 1.0f), 0.36f, 1.3f, H, "rook turret");
    body.append(head);
    return body;
}

// ---- bishop -----------------------------------------------------------------------------------
MeshData buildBishop() {
    const float R = radiusMm(3), H = heightMm(3);
    Profile p;
    addBase(p, R);
    addStem(p, 5.3f, 38.4f, 0.5f);
    float yc = addCollar(p, 6.9f, 9.4f, 2.3f, 6.5f);
    p.to(0.0f, yc);
    MeshData body = latheProfile(finishProfile(p, FILLET, MAX_SEG), LATHE_SEGMENTS, H);

    // Mitre (SDF): egg shape with the diagonal slit, ball finial.
    Profile t;
    float y0 = yc - 0.6f;
    t.to(0, y0).to(5.25f, y0, 0.0f).to(5.25f, yc + 0.6f, 0.3f);
    t.spline({vec2(6.3f, yc + 2.0f), vec2(7.55f, yc + 4.1f), vec2(8.25f, yc + 6.6f), vec2(8.3f, yc + 8.7f), vec2(7.85f, yc + 11.4f),
              vec2(6.8f, yc + 14.2f), vec2(5.35f, yc + 16.7f), vec2(3.7f, yc + 18.8f), vec2(2.25f, yc + 20.1f)});
    float br = 2.45f, by = H - br;
    t.to(1.55f, yc + 20.75f, 0.35f);
    float a0 = -50.0f;
    t.to(br * std::cos(a0 * DEG) * 0.95f, by + br * std::sin(a0 * DEG), 0.3f);
    t.arc(vec2(0, by), br, a0, 90);
    std::vector<vec2> poly = finishClosed(t, 0.3f, 0.3f);
    LatheField lf;
    lf.g.build(poly, 0.04f, 2.0f);
    const float beta = 38.0f * DEG, halfW = 0.72f;
    const vec3 c(0, yc + 10.8f, 0);
    const vec3 n(-std::sin(beta), std::cos(beta), 0);
    auto f = [&](vec3 q) {
        float d = lf(q);
        float slab = std::fabs(dot(q - c, n)) - halfW;
        float cut = std::max(slab, -(q.z + 2.4f));
        return sdf::smax(d, -cut, 0.4f);
    };
    MeshData head = meshField(f, outlineBox(poly, 1.0f), 0.34f, 1.3f, H, "bishop mitre");
    body.append(head);
    return body;
}

// ---- queen ------------------------------------------------------------------------------------
MeshData buildQueen() {
    const float R = radiusMm(5), H = heightMm(5);
    Profile p;
    addBase(p, R);
    addStem(p, 6.0f, 46.8f, 0.5f);
    float yc = addCollar(p, 7.7f, 11.0f, 2.6f, 7.3f);
    p.to(0.0f, yc);
    MeshData body = latheProfile(finishProfile(p, FILLET, MAX_SEG), LATHE_SEGMENTS, H);

    // Coronet (SDF): flared cup with eight points, inner dome, ball finial.
    Profile t;
    float y0 = yc - 0.6f;
    t.to(0, y0).to(5.9f, y0, 0.0f).to(5.9f, yc + 0.7f, 0.3f);
    t.cubic(vec2(6.1f, yc + 5.2f), vec2(9.0f, yc + 9.6f), vec2(10.9f, yc + 14.2f));
    float rimTop = yc + 17.6f;
    t.to(11.15f, rimTop, 0.5f);
    t.to(9.75f, rimTop, 0.45f);
    t.to(9.35f, yc + 15.3f, 0.5f);
    t.cubic(vec2(7.0f, yc + 15.6f), vec2(3.6f, yc + 19.6f), vec2(2.2f, yc + 22.4f));
    float br = 3.5f, by = H - br;
    t.to(1.95f, yc + 23.4f, 0.4f);
    float a0 = -56.0f;
    t.to(br * std::cos(a0 * DEG) * 0.97f, by + br * std::sin(a0 * DEG), 0.3f);
    t.arc(vec2(0, by), br, a0, 90);
    std::vector<vec2> poly = finishClosed(t, 0.3f, 0.3f);
    LatheField lf;
    lf.g.build(poly, 0.04f, 2.0f);
    const int N = 8;
    const float alpha = 57.0f * DEG, rho = 0.45f, yb = yc + 15.0f;
    const vec2 u(std::sin(alpha), std::cos(alpha)), nn(std::cos(alpha), -std::sin(alpha));
    auto f = [&](vec3 q) {
        float r = std::sqrt(q.x * q.x + q.z * q.z);
        float d = lf.g.sample(vec2(r, q.y));
        float th = std::atan2(q.z, q.x), sector = TAU / float(N);
        float tl = th - sector * std::floor(th / sector + 0.5f);
        vec2 v(std::fabs(r * std::sin(tl)), q.y - (yb + rho));
        float dv = dot(v, u) < 0.0f ? length(v) : dot(v, nn);
        float notch = sdf::smax(dv - rho, 8.9f - r, 0.3f);
        return sdf::smax(d, -notch, 0.45f);
    };
    MeshData head = meshField(f, outlineBox(poly, 1.0f), 0.34f, 1.3f, H, "queen coronet");
    body.append(head);
    return body;
}

// ---- king -------------------------------------------------------------------------------------
MeshData buildKing() {
    const float R = radiusMm(6), H = heightMm(6);
    Profile p;
    addBase(p, R);
    addStem(p, 6.5f, 52.8f, 0.5f);
    float yc = addCollar(p, 8.2f, 12.0f, 2.8f, 7.8f);
    // Crown: flared bell, rim bead, domed top, cross socket.
    p.to(6.55f, yc + 1.0f, 0.4f);
    p.cubic(vec2(6.7f, yc + 7.0f), vec2(9.0f, yc + 12.5f), vec2(10.8f, yc + 17.3f));
    p.ellipse(vec2(10.8f, yc + 18.3f), vec2(1.05f, 1.0f), -90, 90);
    p.to(10.3f, yc + 19.5f, 0.3f);
    p.cubic(vec2(8.0f, yc + 21.0f), vec2(5.0f, yc + 22.2f), vec2(3.6f, yc + 22.5f));
    float ySock = yc + 24.2f;
    p.to(3.45f, yc + 22.8f, 0.3f);
    p.to(3.45f, ySock, 0.6f);
    p.to(0.0f, ySock);
    MeshData body = latheProfile(finishProfile(p, FILLET, MAX_SEG), LATHE_SEGMENTS, H);

    // Cross pattee (SDF): arms flaring with concave curves, rounded arrises, faces +/-Z.
    float cy = ySock + (H - ySock) * 0.55f;
    float top = H, bot = ySock - 0.6f, arm = 5.7f, j = 0.95f;
    Profile c;
    c.to(2.5f, top, 0.35f).to(-2.5f, top, 0.35f);
    c.quad(vec2(-1.05f, cy + 3.0f), vec2(-j, cy + j));
    c.quad(vec2(-3.1f, cy + 1.05f), vec2(-arm, cy + 2.4f));
    c.to(-arm, cy - 2.4f, 0.35f);
    c.quad(vec2(-3.1f, cy - 1.05f), vec2(-j, cy - j));
    c.quad(vec2(-1.05f, cy - 3.6f), vec2(-2.6f, bot));
    c.to(2.6f, bot, 0.3f);
    c.quad(vec2(1.05f, cy - 3.6f), vec2(j, cy - j));
    c.quad(vec2(3.1f, cy - 1.05f), vec2(arm, cy - 2.4f));
    c.to(arm, cy + 2.4f, 0.35f);
    c.quad(vec2(3.1f, cy + 1.05f), vec2(j, cy + j));
    c.quad(vec2(1.05f, cy + 3.0f), vec2(2.5f, top));
    std::vector<vec2> poly = finishClosed(c, 0.35f, 0.25f);
    sdf::Grid2D g;
    g.build(poly, 0.03f, 1.5f);
    auto f = [&](vec3 q) { return sdf::extrudeRound(g.sample(vec2(q.x, q.y)), q.z, 2.05f, 0.8f); };
    AABB box;
    box.add(vec3(-arm - 1, bot - 1, -3));
    box.add(vec3(arm + 1, top + 1, 3));
    MeshData cross = meshField(f, box, 0.25f, 1.3f, H, "king cross");
    body.append(cross);
    return body;
}

// ---- knight -----------------------------------------------------------------------------------
struct KnightField {
    sdf::Grid2D side;  // silhouette in (z forward, y up)
    float eyeX = 5.0f, nostrilX = 3.5f;
    static constexpr float CUT = 14.6f;
    static constexpr float EY = 47.9f, EZ = 6.6f;   // eye centre
    static constexpr float NY = 36.7f, NZ = 16.3f;  // nostril centre

    static float halfWidth(float y, float z) {
        float w = lerp(7.3f, 5.75f, smoothstep(15.0f, 32.0f, y));
        w += 0.5f * smoothstep(32.0f, 40.0f, y);
        w -= 1.8f * smoothstep(8.0f, 18.5f, z) * smoothstep(29.0f, 35.0f, y);
        w -= 1.35f * smoothstep(50.0f, 57.0f, y);
        return w;
    }
    // Depth (from the silhouette) over which the cross-section rounds off elliptically; the
    // x semi-axis of that ellipse is the full half width, so the sides stay curved.
    static float roundDepth(float y, float z) {
        float r = lerp(8.5f, 3.9f, smoothstep(28.0f, 38.0f, y));
        return lerp(r, 3.3f, smoothstep(12.0f, 17.0f, z) * smoothstep(29.0f, 35.0f, y));
    }
    // Extruded silhouette + jaw plates: the form the details are carved into.
    float baseForm(vec3 p, float d2) const {
        float ax = std::fabs(p.x);
        float w = halfWidth(p.y, p.z), re = roundDepth(p.y, p.z);
        float sx = re / w;
        float d = sdf::extrudeRound(d2, ax * sx, re, re) / std::max(1.0f, sx);
        if (p.y > 30.0f && p.y < 52.0f && p.z > -9.0f && p.z < 12.0f) {
            float jx = halfWidth(40.8f, 1.6f) - 1.55f;
            d = sdf::smin(d, sdf::sdEllipsoid(vec3(ax, p.y, p.z), vec3(jx, 40.6f, 1.4f), vec3(2.3f, 5.7f, 6.1f)), 1.8f);
        }
        return d;
    }
    float surfaceX(float y, float z) const {
        float d2 = side.sample(vec2(z, y));
        float lo = 0.0f, hi = 12.0f;
        for (int i = 0; i < 32; ++i) {
            float mid = 0.5f * (lo + hi);
            if (baseForm(vec3(mid, y, z), d2) < 0.0f) lo = mid;
            else hi = mid;
        }
        return lo;
    }
    void init() {
        eyeX = surfaceX(EY, EZ);
        nostrilX = surfaceX(NY, NZ);
    }
    float operator()(vec3 p) const {
        const float cut = CUT - p.y;
        float ax = std::fabs(p.x);
        vec2 s(p.z, p.y);
        float d2 = side.sample(s);
        if (d2 > 4.5f) return std::max(d2 - 2.5f, cut);
        float d = baseForm(p, d2);
        if (d > 3.0f && p.y < 50.0f) return std::max(d, cut);
        vec3 q(ax, p.y, p.z);
        // Neck base blends into the collar top with a fillet.
        if (p.y < 20.0f) d = sdf::smin(d, std::max(std::sqrt(p.x * p.x + p.z * p.z) - 11.0f, std::fabs(p.y - 14.4f) - 0.6f), 2.2f);
        // Eye: brow ridge, socket, eyeball.
        if (std::fabs(p.y - EY) < 4.2f && std::fabs(p.z - EZ) < 4.8f) {
            d = sdf::smin(d, sdf::sdEllipsoid(q, vec3(eyeX - 1.05f, EY + 1.4f, EZ - 0.5f), vec3(1.35f, 0.7f, 2.3f)), 0.7f);
            d = sdf::smax(d, -sdf::sdEllipsoid(q, vec3(eyeX + 0.55f, EY, EZ), vec3(1.3f, 1.05f, 1.85f)), 0.35f);
            d = sdf::smin(d, sdf::sdEllipsoid(q, vec3(eyeX - 0.5f, EY - 0.05f, EZ + 0.1f), vec3(1.05f, 0.9f, 1.35f)), 0.2f);
        }
        // Nostrils (flare then hole) and the mouth line.
        if (p.z > 11.0f && p.y < 42.0f) {
            d = sdf::smin(d, sdf::sdEllipsoid(q, vec3(nostrilX - 0.95f, NY + 0.25f, NZ - 0.45f), vec3(1.35f, 1.7f, 1.55f)), 0.5f);
            d = sdf::smax(d, -sdf::sdEllipsoid(q, vec3(nostrilX - 0.05f, NY, NZ + 0.45f), vec3(0.78f, 1.1f, 0.95f)), 0.25f);
            float mouth = sdf::sdSegment2(s, vec2(19.4f, 33.55f), vec2(15.4f, 34.25f)) - 0.2f;
            d = sdf::smax(d, -mouth, 0.2f);
        }
        // Ears (laterally flattened) with a hollow facing forward.
        if (p.y > 50.5f) {
            const float k = 1.2f;
            vec3 e(ax * k, p.y, p.z);
            float ear = 0.83f * sdf::sdRoundCone(e, vec3(2.8f * k, 53.6f, 0.3f), vec3(3.2f * k, 59.6f, -1.5f), 2.15f, 0.55f);
            ear = sdf::smax(ear, -sdf::sdEllipsoid(q, vec3(3.1f, 57.0f, 0.95f), vec3(0.8f, 2.0f, 0.75f)), 0.3f);
            d = sdf::smin(d, ear, 1.0f);
        }
        // Mane: raised crest along the back of the neck with slanted carved grooves.
        if (d < 1.6f && p.z < 3.0f && p.y > 16.0f && p.y < 56.0f) {
            const float e = 0.1f;
            float gz = side.sample(s + vec2(e, 0)) - side.sample(s - vec2(e, 0));
            float gy = side.sample(s + vec2(0, e)) - side.sample(s - vec2(0, e));
            vec2 g = normalize(vec2(gz, gy));
            float back = smoothstep(0.15f, 0.6f, -g.x);
            float band = smoothstep(16.5f, 20.5f, p.y) * (1.0f - smoothstep(53.0f, 55.5f, p.y));
            float strip = 1.0f - smoothstep(2.3f, 3.1f, ax);
            float edge = smoothstep(-2.6f, -0.3f, d2);
            float m8 = back * band * strip * edge;
            if (m8 > 0.0f) {
                float t = side.sampleParam(s);
                float groove = 0.5f - 0.5f * std::cos(TAU * (t + 0.55f * p.x) / 1.9f);
                d -= m8 * (0.85f - 0.6f * groove);
            }
        }
        return std::max(d, cut);
    }
};

MeshData buildKnight() {
    const float R = radiusMm(2), H = heightMm(2);
    Profile p;
    addBase(p, R);
    p.cubic(vec2(11.0f, 11.1f), vec2(11.0f, 11.6f), vec2(11.4f, 12.0f));
    p.to(12.3f, 12.25f, 0.25f);
    p.ellipse(vec2(12.3f, 13.55f), vec2(1.0f, 1.3f), -90, 90);
    p.to(11.6f, 15.1f, 0.25f);
    p.to(0.0f, 15.1f);
    MeshData body = latheProfile(finishProfile(p, FILLET, MAX_SEG), LATHE_SEGMENTS, H);

    // Side silhouette (z forward, y up), counter-clockwise.
    Profile s;
    s.to(-11.2f, 2.0f).to(7.8f, 2.0f, 0.0f).to(7.8f, 13.8f, 0.0f);
    s.spline({vec2(9.6f, 17.5f), vec2(10.8f, 21.5f), vec2(10.8f, 25.2f), vec2(9.8f, 28.4f), vec2(8.4f, 30.7f)});
    s.spline({vec2(9.8f, 31.5f), vec2(12.3f, 31.2f), vec2(14.6f, 31.0f), vec2(16.3f, 31.4f), vec2(17.4f, 32.4f)});
    s.spline({vec2(18.3f, 34.0f), vec2(18.7f, 35.8f), vec2(18.5f, 37.6f), vec2(17.7f, 39.2f)});
    s.spline({vec2(15.6f, 41.9f), vec2(12.9f, 45.2f), vec2(10.2f, 48.4f), vec2(7.6f, 51.4f), vec2(5.0f, 54.0f), vec2(2.7f, 55.6f),
              vec2(0.4f, 56.1f), vec2(-1.8f, 55.5f)});
    s.spline({vec2(-4.2f, 53.6f), vec2(-6.7f, 50.6f), vec2(-8.9f, 46.7f), vec2(-10.6f, 42.2f), vec2(-11.7f, 37.3f), vec2(-12.3f, 31.8f),
              vec2(-12.4f, 26.0f), vec2(-12.1f, 20.2f), vec2(-11.6f, 16.2f), vec2(-11.2f, 13.8f)});
    s.to(-11.2f, 2.0f, 0.0f);
    std::vector<vec2> poly = finishClosed(s, 0.4f, 0.3f);
    KnightField kf;
    kf.side.build(poly, 0.06f, 3.0f);
    kf.init();
    AABB box;
    box.add(vec3(-10.5f, 14.0f, -14.5f));
    box.add(vec3(10.5f, 61.5f, 20.5f));
    MeshData head = meshField([&](vec3 q) { return kf(q); }, box, 0.38f, 1.8f, H, "knight head");
    body.append(head);
    return body;
}


}  // namespace

PieceMeshes buildPiece(int type) {
    double t0 = nowSec();
    PieceMeshes pm;
    if (type < 1 || type > 6) {
        LOGW("buildPiece: invalid piece type %d", type);
        return pm;
    }
    switch (type) {
        case 1: pm.body = buildPawn(); break;
        case 2: pm.body = buildKnight(); break;
        case 3: pm.body = buildBishop(); break;
        case 4: pm.body = buildRook(); break;
        case 5: pm.body = buildQueen(); break;
        default: pm.body = buildKing(); break;
    }
    pm.felt = feltDisc(radiusMm(type));
    const mat4 toMeters = scale(vec3(0.001f));
    pm.body.transform(toMeters);
    pm.felt.transform(toMeters);
    setAxialTangents(pm.body);
    setAxialTangents(pm.felt);
    LOGI("pieces: type %d built: %d tris body, %d tris felt, %.1f ms", type, int(pm.body.indices.size() / 3), int(pm.felt.indices.size() / 3),
         (nowSec() - t0) * 1000.0);
    return pm;
}
