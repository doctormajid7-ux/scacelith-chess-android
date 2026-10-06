#include "ui_draw.h"
#include "text_shape.h"
#include "../core/log.h"
#include "../i18n/i18n.h"
#include "../i18n/unicode.h"
#include "../render/gpu.h"
#include "../render/shader.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace ui {
namespace gfx {
namespace {

struct Vertex {
    float x, y;      // physical pixels, origin top-left
    float u, v;      // local pixel coordinates (shapes) or atlas uv (text)
    uint32_t color;  // RGBA8, straight alpha
    float p0[4];
    float p1[4];
};
static_assert(sizeof(Vertex) == 52, "vertex layout");

enum Mode { MODE_BOX = 0, MODE_SHADOW = 1, MODE_TEXT = 2, MODE_RADIAL = 3 };

struct IRect { int x = 0, y = 0, w = 0, h = 0; bool on = false; };
bool operator==(const IRect& a, const IRect& b) {
    return a.on == b.on && (!a.on || (a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h));
}

struct Cmd { uint32_t firstQuad, quadCount; IRect clip; };
struct LayerData {
    std::vector<Vertex> verts;
    std::vector<Cmd> cmds;
};

struct State {
    bool ready = false;
    int fbW = 1, fbH = 1;
    float s = 1.0f;
    LayerData layers[LAYER_COUNT];
    Layer cur = LAYER_MAIN;
    std::vector<Rect> clips;
    std::vector<float> alphas;
    float alpha = 1.0f;
    GLuint vao = 0;
    gpu::Buffer vbo, ibo;
    size_t iboQuads = 0;
    const ShaderProgram* prog = nullptr;
    std::vector<Vertex> merged;
};
State g;

uint32_t pack(vec4 c) {
    auto q = [](float v) { return uint32_t(std::lround(m::saturate(v) * 255.0f)); };
    return q(c.x) | (q(c.y) << 8) | (q(c.z) << 16) | (q(c.w * g.alpha) << 24);
}

IRect currentClip() {
    IRect r;
    if (g.clips.empty()) return r;
    const Rect& c = g.clips.back();
    float x0 = std::floor(c.x * g.s), y0 = std::floor(c.y * g.s);
    float x1 = std::ceil(c.r() * g.s), y1 = std::ceil(c.b() * g.s);
    r.on = true;
    r.x = int(x0);
    r.w = std::max(0, int(x1 - x0));
    r.h = std::max(0, int(y1 - y0));
    r.y = g.fbH - int(y0) - r.h;  // GL scissor origin is bottom-left
    return r;
}

Vertex* allocQuad() {
    LayerData& L = g.layers[g.cur];
    IRect clip = currentClip();
    uint32_t quad = uint32_t(L.verts.size() / 4);
    if (L.cmds.empty() || !(L.cmds.back().clip == clip)) L.cmds.push_back({quad, 0, clip});
    L.cmds.back().quadCount++;
    L.verts.resize(L.verts.size() + 4);
    return &L.verts[L.verts.size() - 4];
}

// Generic quad with per-corner positions/uv/colours (order: tl, tr, bl, br).
void emit(const vec2 pos[4], const vec2 uv[4], const uint32_t col[4], const float p0[4], const float p1[4]) {
    Vertex* v = allocQuad();
    for (int i = 0; i < 4; ++i) {
        v[i].x = pos[i].x;
        v[i].y = pos[i].y;
        v[i].u = uv[i].x;
        v[i].v = uv[i].y;
        v[i].color = col[i];
        std::memcpy(v[i].p0, p0, sizeof(float) * 4);
        std::memcpy(v[i].p1, p1, sizeof(float) * 4);
    }
}

// Axis-aligned box in physical pixels. margin = extra quad extent for AA / blur.
void boxPx(float x0, float y0, float x1, float y1, float radius, float strokePx, float feather, float margin,
           const vec4 col[4], int mode) {
    if (x1 <= x0 || y1 <= y0) return;
    float hx = (x1 - x0) * 0.5f, hy = (y1 - y0) * 0.5f;
    float cx = x0 + hx, cy = y0 + hy;
    float ex = hx + margin, ey = hy + margin;
    vec2 pos[4] = {{cx - ex, cy - ey}, {cx + ex, cy - ey}, {cx - ex, cy + ey}, {cx + ex, cy + ey}};
    vec2 uv[4] = {{-ex, -ey}, {ex, -ey}, {-ex, ey}, {ex, ey}};
    uint32_t c[4] = {pack(col[0]), pack(col[1]), pack(col[2]), pack(col[3])};
    float p0[4] = {hx, hy, std::min(radius, std::min(hx, hy)), strokePx};
    float p1[4] = {float(mode), feather, 0.0f, 0.0f};
    emit(pos, uv, c, p0, p1);
}

void boxRef(const Rect& r, float radius, float strokeRef, const vec4 col[4]) {
    float s = g.s;
    float strokePx = strokeRef > 0.0f ? std::max(1.0f, strokeRef * s) : 0.0f;
    boxPx(r.x * s, r.y * s, r.r() * s, r.b() * s, radius * s, strokePx, 1.0f, 1.0f, col, MODE_BOX);
}

// Rotated box: centre, half extents (physical px), unit axis for local +x.
void rotBoxPx(vec2 c, vec2 half, vec2 axis, float radius, float strokePx, vec4 col) {
    vec2 n(-axis.y, axis.x);
    float m = 1.0f;
    vec2 e(half.x + m, half.y + m);
    vec2 loc[4] = {{-e.x, -e.y}, {e.x, -e.y}, {-e.x, e.y}, {e.x, e.y}};
    vec2 pos[4];
    for (int i = 0; i < 4; ++i) pos[i] = c + axis * loc[i].x + n * loc[i].y;
    uint32_t cc = pack(col);
    uint32_t cols[4] = {cc, cc, cc, cc};
    float p0[4] = {half.x, half.y, std::min(radius, std::min(half.x, half.y)), strokePx};
    float p1[4] = {float(MODE_BOX), 1.0f, 0.0f, 0.0f};
    emit(pos, loc, cols, p0, p1);
}

}  // namespace

Rect intersect(const Rect& a, const Rect& b) {
    float x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
    float x1 = std::min(a.r(), b.r()), y1 = std::min(a.b(), b.b());
    return {x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
}

bool init() {
    if (g.ready) return true;
    ProgramDesc d;
    d.vs = "shaders/ui/ui.vert";
    d.fs = "shaders/ui/ui.frag";
    g.prog = &shaders::get(d);
    if (!g.prog->valid()) LOGE("ui: shader compilation failed");
    glCreateVertexArrays(1, &g.vao);
    glEnableVertexArrayAttrib(g.vao, 0);
    glVertexArrayAttribFormat(g.vao, 0, 2, GL_FLOAT, GL_FALSE, offsetof(Vertex, x));
    glEnableVertexArrayAttrib(g.vao, 1);
    glVertexArrayAttribFormat(g.vao, 1, 2, GL_FLOAT, GL_FALSE, offsetof(Vertex, u));
    glEnableVertexArrayAttrib(g.vao, 2);
    glVertexArrayAttribFormat(g.vao, 2, 4, GL_UNSIGNED_BYTE, GL_TRUE, offsetof(Vertex, color));
    glEnableVertexArrayAttrib(g.vao, 3);
    glVertexArrayAttribFormat(g.vao, 3, 4, GL_FLOAT, GL_FALSE, offsetof(Vertex, p0));
    glEnableVertexArrayAttrib(g.vao, 4);
    glVertexArrayAttribFormat(g.vao, 4, 4, GL_FLOAT, GL_FALSE, offsetof(Vertex, p1));
    for (GLuint i = 0; i < 5; ++i) glVertexArrayAttribBinding(g.vao, i, 0);
    g.ready = true;
    return g.prog->valid();
}

void shutdown() {
    if (g.vao) glDeleteVertexArrays(1, &g.vao);
    g.vbo.destroy();
    g.ibo.destroy();
    g = State();
}

void beginFrame(int w, int h) {
    font::beginFrame();
    g.fbW = std::max(1, w);
    g.fbH = std::max(1, h);
    g.s = float(g.fbH) / 1080.0f;
    for (auto& L : g.layers) {
        L.verts.clear();
        L.cmds.clear();
    }
    g.cur = LAYER_MAIN;
    g.clips.clear();
    g.alphas.clear();
    g.alpha = 1.0f;
}

void endFrame() {
    if (!g.ready || !g.prog || !g.prog->valid()) return;
    font::flushUploads();
    size_t total = 0;
    for (auto& L : g.layers) total += L.verts.size();
    if (total == 0) return;
    g.merged.clear();
    g.merged.reserve(total);
    struct Draw { uint32_t first, count; IRect clip; };
    std::vector<Draw> draws;
    for (auto& L : g.layers) {
        uint32_t base = uint32_t(g.merged.size() / 4);
        g.merged.insert(g.merged.end(), L.verts.begin(), L.verts.end());
        for (auto& c : L.cmds)
            if (c.quadCount) draws.push_back({base + c.firstQuad, c.quadCount, c.clip});
    }
    size_t quads = total / 4;
    size_t bytes = total * sizeof(Vertex);
    gpu::ensureBuffer(g.vbo, bytes);
    // Rebind every frame: a re-created buffer may reuse the old name while the VAO still
    // references the deleted object.
    glVertexArrayVertexBuffer(g.vao, 0, g.vbo.id, 0, sizeof(Vertex));
    glInvalidateBufferData(g.vbo.id);
    glNamedBufferSubData(g.vbo.id, 0, GLsizeiptr(bytes), g.merged.data());
    if (quads > g.iboQuads) {
        size_t n = std::max<size_t>(1024, quads + quads / 2);
        std::vector<uint32_t> idx(n * 6);
        for (size_t q = 0; q < n; ++q) {
            uint32_t b = uint32_t(q * 4);
            uint32_t* p = &idx[q * 6];
            p[0] = b; p[1] = b + 2; p[2] = b + 1;
            p[3] = b + 1; p[4] = b + 2; p[5] = b + 3;
        }
        g.ibo.destroy();
        g.ibo = gpu::createBuffer(idx.size() * sizeof(uint32_t), idx.data(), 0);
        g.iboQuads = n;
        glVertexArrayElementBuffer(g.vao, g.ibo.id);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, g.fbW, g.fbH);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    g.prog->use();
    g.prog->set("uViewport", float(g.fbW), float(g.fbH));
    glBindTextureUnit(0, font::atlasTexture());
    glBindVertexArray(g.vao);
    bool scissor = false;
    for (auto& d : draws) {
        if (d.clip.on) {
            if (d.clip.w <= 0 || d.clip.h <= 0) continue;
            if (!scissor) glEnable(GL_SCISSOR_TEST);
            scissor = true;
            glScissor(d.clip.x, d.clip.y, d.clip.w, d.clip.h);
        } else if (scissor) {
            glDisable(GL_SCISSOR_TEST);
            scissor = false;
        }
        glDrawElements(GL_TRIANGLES, GLsizei(d.count * 6), GL_UNSIGNED_INT,
                       reinterpret_cast<const void*>(uintptr_t(d.first) * 6 * sizeof(uint32_t)));
    }
    if (scissor) glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glBindVertexArray(0);
    glUseProgram(0);
}

float scale() { return g.s; }
float px() { return 1.0f / g.s; }
vec2 viewSize() { return {float(g.fbW) / g.s, 1080.0f}; }
float snap(float v) { return std::round(v * g.s) / g.s; }

void setLayer(Layer l) { g.cur = l; }
Layer layer() { return g.cur; }
void pushClip(const Rect& r) { g.clips.push_back(g.clips.empty() ? r : intersect(g.clips.back(), r)); }
void popClip() { if (!g.clips.empty()) g.clips.pop_back(); }
bool clipContains(vec2 p) { return g.clips.empty() || g.clips.back().contains(p); }
void pushAlpha(float a) {
    g.alphas.push_back(g.alpha);
    g.alpha *= m::saturate(a);
}
void popAlpha() {
    if (g.alphas.empty()) return;
    g.alpha = g.alphas.back();
    g.alphas.pop_back();
}

// ---- Shapes -------------------------------------------------------------------------------------
void fill(const Rect& r, vec4 c, float radius) {
    vec4 col[4] = {c, c, c, c};
    boxRef(r, radius, 0.0f, col);
}
void fillV(const Rect& r, vec4 top, vec4 bottom, float radius) {
    vec4 col[4] = {top, top, bottom, bottom};
    boxRef(r, radius, 0.0f, col);
}
void fillH(const Rect& r, vec4 left, vec4 right, float radius) {
    vec4 col[4] = {left, right, left, right};
    boxRef(r, radius, 0.0f, col);
}
void stroke(const Rect& r, vec4 c, float thickness, float radius) {
    vec4 col[4] = {c, c, c, c};
    float s = g.s;
    // Snap the outline to the pixel grid so hairlines stay crisp.
    float x0 = std::round(r.x * s), y0 = std::round(r.y * s), x1 = std::round(r.r() * s), y1 = std::round(r.b() * s);
    float t = thickness > 0.0f ? std::max(1.0f, std::round(thickness * s)) : 1.0f;
    boxPx(x0, y0, x1, y1, radius * s, t, 1.0f, 1.0f, col, MODE_BOX);
}
void shadow(const Rect& r, float radius, float blur, vec4 c) {
    vec4 col[4] = {c, c, c, c};
    float s = g.s;
    float sigma = std::max(0.5f, blur * s * 0.5f);
    boxPx(r.x * s, r.y * s, r.r() * s, r.b() * s, radius * s, 0.0f, sigma, sigma * 3.0f, col, MODE_SHADOW);
}
void hline(float x0, float x1, float y, vec4 c, float thickness) {
    float s = g.s;
    float t = thickness > 0.0f ? std::max(1.0f, std::round(thickness * s)) : 1.0f;
    float yp = std::floor(y * s);
    vec4 col[4] = {c, c, c, c};
    boxPx(x0 * s, yp, x1 * s, yp + t, 0.0f, 0.0f, 1.0f, 1.0f, col, MODE_BOX);
}
void vline(float x, float y0, float y1, vec4 c, float thickness) {
    float s = g.s;
    float t = thickness > 0.0f ? std::max(1.0f, std::round(thickness * s)) : 1.0f;
    float xp = std::floor(x * s);
    vec4 col[4] = {c, c, c, c};
    boxPx(xp, y0 * s, xp + t, y1 * s, 0.0f, 0.0f, 1.0f, 1.0f, col, MODE_BOX);
}
void hlineFade(float x0, float x1, float y, vec4 c, float fadeFrac, float thickness) {
    float s = g.s;
    float t = thickness > 0.0f ? std::max(1.0f, std::round(thickness * s)) : 1.0f;
    float yp = std::floor(y * s);
    float a = x0 * s, b = x1 * s;
    float f = (b - a) * m::clamp(fadeFrac, 0.0f, 0.5f);
    vec4 z(c.x, c.y, c.z, 0.0f);
    vec4 l[4] = {z, c, z, c}, mid[4] = {c, c, c, c}, r[4] = {c, z, c, z};
    boxPx(a, yp, a + f, yp + t, 0.0f, 0.0f, 1.0f, 0.0f, l, MODE_BOX);
    if (b - a > 2.0f * f) boxPx(a + f, yp, b - f, yp + t, 0.0f, 0.0f, 1.0f, 0.0f, mid, MODE_BOX);
    boxPx(b - f, yp, b, yp + t, 0.0f, 0.0f, 1.0f, 0.0f, r, MODE_BOX);
}
void line(vec2 a, vec2 b, vec4 c, float thickness) {
    float s = g.s;
    vec2 pa = a * s, pb = b * s;
    vec2 d = pb - pa;
    float len = m::length(d);
    if (len < 1e-4f) return;
    vec2 axis = d / len;
    float t = std::max(1.0f, thickness * s);
    rotBoxPx((pa + pb) * 0.5f, vec2(len * 0.5f, t * 0.5f), axis, 0.0f, 0.0f, c);
}
void diamond(vec2 center, float radius, vec4 c, float strokeWidth) {
    float s = g.s;
    float h = radius * s * 0.70710678f;
    float st = strokeWidth > 0.0f ? std::max(1.0f, strokeWidth * s) : 0.0f;
    rotBoxPx(center * s, vec2(h, h), vec2(0.70710678f, 0.70710678f), 0.0f, st, c);
}
void circle(vec2 center, float radius, vec4 c, float strokeWidth) {
    float s = g.s;
    float r = radius * s;
    vec4 col[4] = {c, c, c, c};
    float st = strokeWidth > 0.0f ? std::max(1.0f, strokeWidth * s) : 0.0f;
    boxPx(center.x * s - r, center.y * s - r, center.x * s + r, center.y * s + r, r, st, 1.0f, 1.0f, col, MODE_BOX);
}
void radial(vec2 center, vec2 radii, vec4 c, float t0, float t1) {
    float s = g.s;
    vec2 cp = center * s, rp = radii * s;
    vec2 e = rp * t1;
    vec2 pos[4] = {{cp.x - e.x, cp.y - e.y}, {cp.x + e.x, cp.y - e.y}, {cp.x - e.x, cp.y + e.y}, {cp.x + e.x, cp.y + e.y}};
    vec2 uv[4] = {{-e.x, -e.y}, {e.x, -e.y}, {-e.x, e.y}, {e.x, e.y}};
    uint32_t cc = pack(c);
    uint32_t col[4] = {cc, cc, cc, cc};
    float p0[4] = {rp.x, rp.y, t0, t1};
    float p1[4] = {float(MODE_RADIAL), 1.0f, 0.0f, 0.0f};
    emit(pos, uv, col, p0, p1);
}

// ---- Text ---------------------------------------------------------------------------------------
namespace {
// Options > Display, text size: every interface text is drawn and measured this much larger. Not
// the wordmark-sized titles (sized to the page) nor the handwriting (sized to the paper).
float g_textScale = 1.0f;
float esz(const TextStyle& st) { return (st.hand >= 0 || st.size > 60.0f) ? st.size : st.size * g_textScale; }
}  // namespace

void setTextScale(float k) { g_textScale = m::clamp(k, 0.8f, 1.6f); }
float textScale() { return g_textScale; }

namespace {
text::Run shape(const std::string& s, const TextStyle& st) {
    int dir = textDirection(s, st);
    if (st.hand >= 0) return text::shapeHandwriting(s, st.hand, st.tracking, dir);
    return text::shapeLine(s, st.face, st.tracking, dir);
}
}  // namespace

float caretOffset(const std::string& s, const TextStyle& st, int index) {
    if (!font::ready()) return 0.0f;
    return text::caretX(shape(s, st), index) * esz(st);
}

int caretAt(const std::string& s, const TextStyle& st, float offset) {
    if (!font::ready() || esz(st) <= 0.0f) return 0;
    return text::caretIndex(shape(s, st), offset / esz(st));
}

int textDirection(const std::string& s, const TextStyle& st) {
    if (st.dir >= 0) return st.dir & 1;
    std::u32string u = uni::decode(s);
    if (i18n::rtl() && uni::containsRtl(u)) return 1;
    return uni::paragraphLevel(u, 0);
}

float textWidth(const std::string& s, const TextStyle& st) {
    if (!font::ready() || s.empty()) return 0.0f;
    return shape(s, st).advance * esz(st);
}

float fitSize(const std::string& s, const TextStyle& st, float maxWidth, float minScale) {
    float w = textWidth(s, st);
    if (w <= maxWidth || w <= 0.0f) return st.size;
    return st.size * std::max(minScale, maxWidth / w);
}

float capHeight(const TextStyle& st) {
    // The cap height of the style's face (the handwriting face for a hand); scripts without
    // capitals use the same value.
    int face = st.face;
    if (st.hand >= 0) face = font::FACE_HAND_CAVEAT + st.hand;
    return font::metrics(face).capHeight * esz(st);
}

float text(const std::string& s, float x, float baseline, const TextStyle& st) {
    if (!font::ready() || s.empty()) return 0.0f;
    text::Run run = shape(s, st);
    float w = run.advance * esz(st);
    if (st.align == HAlign::Center) x -= w * 0.5f;
    else if (st.align == HAlign::Right) x -= w;
    float sc = g.s;
    float sizePx = esz(st) * sc;
    float ox = x * sc;
    float by = std::round(baseline * sc);
    // Slight dilation at small sizes keeps thin serifs visible after blending.
    float dil = st.weight * sc + m::clamp((22.0f - sizePx) * 0.02f, 0.0f, 0.2f);
    float soft = 1.0f + st.softness * sc;
    uint32_t cc = pack(st.color);
    uint32_t col[4] = {cc, cc, cc, cc};
    for (const text::PlacedGlyph& pg : run.glyphs) {
        const font::Glyph& gl = *pg.glyph;
        if (!gl.hasQuad) continue;
        float gs = sizePx * pg.scale;
        float pen = pg.x * sizePx;
        float x0 = ox + pen + gl.x0 * gs, x1 = ox + pen + gl.x1 * gs;
        float y0 = by + gl.y0 * gs, y1 = by + gl.y1 * gs;
        vec2 pos[4] = {{x0, y0}, {x1, y0}, {x0, y1}, {x1, y1}};
        vec2 uv[4] = {{gl.u0, gl.v0}, {gl.u1, gl.v0}, {gl.u0, gl.v1}, {gl.u1, gl.v1}};
        float p0[4] = {soft, dil, gl.pxRange, 0.0f};
        float p1[4] = {float(MODE_TEXT), 0.0f, 0.0f, 0.0f};
        emit(pos, uv, col, p0, p1);
    }
    return w;
}

namespace {
struct Line {
    std::string text;
    int dir = 0;      // base direction of its paragraph
    float width = 0;  // reference pixels
};

// Splits into wrapped lines: at spaces (dropped at the break) and between CJK characters. Widths
// are the sum of the shaped pieces (nothing joins across a break opportunity). Results are cached
// per frame-independent key since paragraphs are drawn every frame; the returned reference stays
// valid until the next wrap() call.
const std::vector<Line>& wrap(const std::string& s, float maxWidth, const TextStyle& st) {
    struct Cache {
        std::unordered_map<std::string, std::vector<Line>> map;
        int lang = -1, gen = -1;
    };
    static Cache cache;
    if (cache.lang != i18n::generation() || cache.gen != font::atlasGeneration() || cache.map.size() > 512) {
        cache.map.clear();
        cache.lang = i18n::generation();
        cache.gen = font::atlasGeneration();
    }
    char keyBuf[96];
    std::snprintf(keyBuf, sizeof(keyBuf), "%d|%d|%d|%.3f|%.3f|%.3f|", st.face, st.hand, st.dir, esz(st), st.tracking, maxWidth);
    std::string key = keyBuf + s;
    auto hit = cache.map.find(key);
    if (hit != cache.map.end()) return hit->second;

    std::vector<Line> lines;
    const float spaceW = textWidth(" ", st);
    size_t start = 0;
    while (start <= s.size()) {
        size_t nl = s.find('\n', start);
        std::string para = s.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        TextStyle ps = st;
        ps.dir = textDirection(para, st);
        // Tokens: [a, b) byte ranges without spaces; spaceBefore = separated from the previous one.
        struct Tok { size_t a, b; bool spaceBefore; float w; float join; };
        std::vector<Tok> toks;
        bool pendingSpace = false;
        uint32_t prev = 0;
        for (size_t i = 0; i < para.size();) {
            size_t at = i;
            uint32_t cp = uni::decodeAt(para, i);
            if (cp == ' ' || cp == 0x3000 || cp == '\t') {
                pendingSpace = true;
                prev = cp;
                continue;
            }
            if (toks.empty() || pendingSpace || uni::breakBetween(char32_t(prev), char32_t(cp)))
                toks.push_back({at, i, pendingSpace && !toks.empty(), 0.0f,
                                st.tracking * esz(st) * uni::trackingScale(char32_t(prev), char32_t(cp))});
            else
                toks.back().b = i;
            pendingSpace = false;
            prev = cp;
        }
        Line cur;
        cur.dir = ps.dir;
        size_t lineA = 0, lineB = 0;
        bool open = false;
        for (Tok& t : toks) {
            t.w = textWidth(para.substr(t.a, t.b - t.a), ps);
            float add = (open ? (t.spaceBefore ? spaceW : t.join) : 0.0f) + t.w;
            if (open && cur.width + add > maxWidth) {
                cur.text = para.substr(lineA, lineB - lineA);
                lines.push_back(cur);
                cur.width = 0.0f;
                open = false;
                add = t.w;
            }
            if (!open) lineA = t.a;
            lineB = t.b;
            cur.width += add;
            open = true;
        }
        cur.text = open ? para.substr(lineA, lineB - lineA) : std::string();
        lines.push_back(cur);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return cache.map.emplace(key, std::move(lines)).first->second;
}
}  // namespace

int textWrapped(const std::string& s, float x, float baseline, float maxWidth, const TextStyle& st, float lineHeight) {
    if (lineHeight <= 0.0f) lineHeight = esz(st) * 1.3f;
    else if (st.size > 0.0f) lineHeight *= esz(st) / st.size;   // the caller's spacing, for the larger text
    const std::vector<Line>& lines = wrap(s, maxWidth, st);  // text() never calls wrap()
    float y = baseline;
    for (auto& l : lines) {
        TextStyle ls = st;
        ls.dir = l.dir;
        text(l.text, x, y, ls);
        y += lineHeight;
    }
    return int(lines.size());
}

int wrapLineCount(const std::string& s, float maxWidth, const TextStyle& st) { return int(wrap(s, maxWidth, st).size()); }

float wrapWidth(const std::string& s, float maxWidth, const TextStyle& st) {
    float w = 0.0f;
    for (const Line& l : wrap(s, maxWidth, st)) w = std::max(w, l.width);
    return w;
}

}  // namespace gfx
}  // namespace ui
