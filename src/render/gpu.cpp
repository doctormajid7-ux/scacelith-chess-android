#include "gpu.h"
#include "../core/log.h"
#ifdef SCACELITH_GLES
#include "../gl/gl46_gles.h"
#endif
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace gpu {

int mipCount(int w, int h, int d) {
    int m = std::max(w, std::max(h, d));
    int n = 1;
    while (m > 1) { m >>= 1; ++n; }
    return n;
}

static void defaultSampling(const Texture& t) {
    bool mips = t.levels > 1;
    bool isInt = t.format == GL_R32UI || t.format == GL_RG32UI || t.format == GL_RGBA32UI || t.format == GL_R8UI ||
                 t.format == GL_R32I || t.format == GL_R16UI;
#ifdef SCACELITH_GLES
    // GL ES never filters a depth texture read without a compare mode: with a LINEAR filter it is
    // incomplete, and every texelFetch of the scene depth (depth_prep, half_prep, SSR) returns 0,
    // i.e. "sky" everywhere. Nearest by default; the shadow lookups bring their own (compare)
    // sampler objects (Renderer::bindGlobalTextures, PostFX's shadowSampler).
    isInt = isInt || t.format == GL_DEPTH_COMPONENT16 || t.format == GL_DEPTH_COMPONENT24 ||
            t.format == GL_DEPTH_COMPONENT32F || t.format == GL_DEPTH24_STENCIL8 || t.format == GL_DEPTH32F_STENCIL8;
    // 32-bit float textures filter only with OES_texture_float_linear (Adreno has it, not every
    // Mali does); without it a LINEAR one is incomplete and reads as 0 as well.
    static const bool floatLinear = gl46::glesHasExtension("GL_OES_texture_float_linear");
    if (!floatLinear) isInt = isInt || t.format == GL_R32F || t.format == GL_RG32F || t.format == GL_RGBA32F;
#endif
    GLenum minF = isInt ? GL_NEAREST : (mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTextureParameteri(t.id, GL_TEXTURE_MIN_FILTER, minF);
    glTextureParameteri(t.id, GL_TEXTURE_MAG_FILTER, isInt ? GL_NEAREST : GL_LINEAR);
    glTextureParameteri(t.id, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(t.id, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTextureParameteri(t.id, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
}

Texture createTexture2D(int w, int h, GLenum fmt, int levels) {
    Texture t;
    t.target = GL_TEXTURE_2D;
    t.format = fmt;
    t.width = w;
    t.height = h;
    t.levels = levels <= 0 ? mipCount(w, h) : levels;
    glCreateTextures(GL_TEXTURE_2D, 1, &t.id);
    glTextureStorage2D(t.id, t.levels, fmt, w, h);
    defaultSampling(t);
    return t;
}

Texture createTexture2DArray(int w, int h, int layers, GLenum fmt, int levels) {
    Texture t;
    t.target = GL_TEXTURE_2D_ARRAY;
    t.format = fmt;
    t.width = w;
    t.height = h;
    t.depth = layers;
    t.levels = levels <= 0 ? mipCount(w, h) : levels;
    glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &t.id);
    glTextureStorage3D(t.id, t.levels, fmt, w, h, layers);
    defaultSampling(t);
    return t;
}

Texture createTexture3D(int w, int h, int d, GLenum fmt, int levels) {
    Texture t;
    t.target = GL_TEXTURE_3D;
    t.format = fmt;
    t.width = w;
    t.height = h;
    t.depth = d;
    t.levels = levels <= 0 ? mipCount(w, h, d) : levels;
    glCreateTextures(GL_TEXTURE_3D, 1, &t.id);
    glTextureStorage3D(t.id, t.levels, fmt, w, h, d);
    defaultSampling(t);
    return t;
}

Texture createCubemapArray(int size, int cubes, GLenum fmt, int levels) {
    Texture t;
    t.target = GL_TEXTURE_CUBE_MAP_ARRAY;
    t.format = fmt;
    t.width = t.height = size;
    t.depth = cubes * 6;
    t.levels = levels <= 0 ? mipCount(size, size) : levels;
    glCreateTextures(GL_TEXTURE_CUBE_MAP_ARRAY, 1, &t.id);
    glTextureStorage3D(t.id, t.levels, fmt, size, size, cubes * 6);
    defaultSampling(t);
    return t;
}

void setFilter(const Texture& t, GLenum minF, GLenum magF) {
    glTextureParameteri(t.id, GL_TEXTURE_MIN_FILTER, minF);
    glTextureParameteri(t.id, GL_TEXTURE_MAG_FILTER, magF);
}
void setWrap(const Texture& t, GLenum wrap) {
    glTextureParameteri(t.id, GL_TEXTURE_WRAP_S, wrap);
    glTextureParameteri(t.id, GL_TEXTURE_WRAP_T, wrap);
    glTextureParameteri(t.id, GL_TEXTURE_WRAP_R, wrap);
}
void setAnisotropy(const Texture& t, float a) {
    float maxA = 1.0f;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxA);
    glTextureParameterf(t.id, GL_TEXTURE_MAX_ANISOTROPY, std::min(a, maxA));
}

Framebuffer createFramebuffer(std::initializer_list<const Texture*> colors, const Texture* depth, int level) {
    Framebuffer fb;
    glCreateFramebuffers(1, &fb.id);
    GLenum bufs[8];
    int n = 0;
    for (const Texture* c : colors) {
        glNamedFramebufferTexture(fb.id, GL_COLOR_ATTACHMENT0 + n, c->id, level);
        bufs[n] = GL_COLOR_ATTACHMENT0 + n;
        ++n;
    }
    if (depth) {
        bool stencil = depth->format == GL_DEPTH24_STENCIL8 || depth->format == GL_DEPTH32F_STENCIL8;
        glNamedFramebufferTexture(fb.id, stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT, depth->id, level);
    }
    if (n) glNamedFramebufferDrawBuffers(fb.id, n, bufs);
    else glNamedFramebufferDrawBuffer(fb.id, GL_NONE);
    return fb;
}

Framebuffer createFramebufferLayer(const Texture* color, int colorLayer, const Texture* depth, int depthLayer, int level) {
    Framebuffer fb;
    glCreateFramebuffers(1, &fb.id);
    if (color) {
        glNamedFramebufferTextureLayer(fb.id, GL_COLOR_ATTACHMENT0, color->id, level, colorLayer);
        glNamedFramebufferDrawBuffer(fb.id, GL_COLOR_ATTACHMENT0);
    } else {
        glNamedFramebufferDrawBuffer(fb.id, GL_NONE);
    }
    if (depth) glNamedFramebufferTextureLayer(fb.id, GL_DEPTH_ATTACHMENT, depth->id, level, depthLayer);
    return fb;
}

bool checkFramebuffer(const Framebuffer& fb, const char* name) {
    GLenum s = glCheckNamedFramebufferStatus(fb.id, GL_FRAMEBUFFER);
    if (s != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("framebuffer '%s' incomplete: 0x%x", name, s);
        return false;
    }
    return true;
}

void ensureBuffer(Buffer& b, size_t size, GLbitfield flags) {
    if (b.id && b.size >= size) return;
    b.destroy();
    size_t cap = std::max<size_t>(size, 256);
    cap = size_t(std::pow(2.0, std::ceil(std::log2(double(cap)))));
    glCreateBuffers(1, &b.id);
    glNamedBufferStorage(b.id, GLsizeiptr(cap), nullptr, flags);
    b.size = cap;
}

Buffer createBuffer(size_t size, const void* data, GLbitfield flags) {
    Buffer b;
    glCreateBuffers(1, &b.id);
    glNamedBufferStorage(b.id, GLsizeiptr(size), data, flags);
    b.size = size;
    return b;
}

void drawFullscreenTriangle() {
    static GLuint vao = 0;
    if (!vao) glCreateVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void dispatch2D(int w, int h, int lx, int ly) {
    glDispatchCompute(GLuint((w + lx - 1) / lx), GLuint((h + ly - 1) / ly), 1);
}


int frustumPlanes(const m::mat4& vp, m::vec4 out[6], bool sidesOnly) {
    auto row = [&](int i) { return m::vec4(vp.c[0][i], vp.c[1][i], vp.c[2][i], vp.c[3][i]); };
    m::vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
    m::vec4 cand[6] = {r3 + r0, r3 - r0, r3 + r1, r3 - r1, r2, r3 - r2};
    int n = 0;
    for (int i = 0; i < (sidesOnly ? 4 : 6); ++i) {
        float l = std::sqrt(cand[i].x * cand[i].x + cand[i].y * cand[i].y + cand[i].z * cand[i].z);
        if (l < 1e-6f) continue;
        out[n++] = cand[i] / l;
    }
    return n;
}

namespace {
struct ProfEntry { std::string name; double ms; };
std::vector<ProfEntry> g_prof;
int g_profFrame = 0;
int profileMode() {
    static int mode = [] {
        const char* e = std::getenv("SCACELITH_GPU_PROFILE");
        return e ? std::max(0, std::atoi(e)) : 0;
    }();
    return mode;
}
bool profilingEnabled() { return profileMode() > 0; }
double nowMs() {
    using namespace std::chrono;
    return double(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count()) * 1e-3;
}
}  // namespace

ProfileScope::ProfileScope(const char* name) : name_(name) {
    if (!profilingEnabled()) return;
    glFinish();
    start_ = nowMs();
}
ProfileScope::~ProfileScope() {
    if (!profilingEnabled()) return;
    glFinish();
    double ms = nowMs() - start_;
    for (auto& e : g_prof)
        if (e.name == name_) { e.ms += ms; return; }
    g_prof.push_back({name_, ms});
}

static bool profileGroups() {
    static const bool on = profilingEnabled() && std::getenv("SCACELITH_GPU_PROFILE_GROUPS");
    return on;
}

DebugGroup::DebugGroup(const char* name) {
    glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, -1, name);
    if (!profileGroups()) return;
    name_ = name;
    glFinish();
    start_ = nowMs();
}
DebugGroup::~DebugGroup() {
    glPopDebugGroup();
    if (!name_) return;
    glFinish();
    const double ms = nowMs() - start_;
    const std::string key = std::string("[") + name_ + "]";
    for (auto& e : g_prof)
        if (e.name == key) { e.ms += ms; return; }
    g_prof.push_back({key, ms});
}

bool profileGroupsEnabled() { return profileGroups(); }
double profileNowMs() { return nowMs(); }
void profileAdd(const std::string& name, double ms) {
    for (auto& e : g_prof)
        if (e.name == name) { e.ms += ms; return; }
    g_prof.push_back({name, ms});
}

void profileEndFrame() {
    if (!profilingEnabled()) return;
    ++g_profFrame;
    int every = profileMode() == 1 ? 1 : profileMode();
    if (g_profFrame % every != 0) return;
    std::string line;
    double total = 0;
    for (auto& e : g_prof) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), " %s=%.2f", e.name.c_str(), e.ms / every);
        line += buf;
        total += e.ms / every;
    }
    LOGI("profile frame %d (ms, CPU+glFinish):%s | sum=%.2f", g_profFrame, line.c_str(), total);
    g_prof.clear();
}

}  // namespace gpu
