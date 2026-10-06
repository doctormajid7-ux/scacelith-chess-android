// Thin helpers over OpenGL 4.6 DSA objects. Everything uses direct state access
// (glCreate*/glNamed*/glTexture*); never bind-to-edit.
#pragma once
#include <string>
#include "../gl/gl46.h"
#include "../math/math.h"
#include <cstdint>
#include <vector>

namespace gpu {

struct Texture {
    GLuint id = 0;
    GLenum target = GL_TEXTURE_2D;
    GLenum format = GL_RGBA8;
    int width = 0, height = 0, depth = 1, levels = 1;
    void destroy() { if (id) glDeleteTextures(1, &id); id = 0; }
};

int mipCount(int w, int h, int d = 1);
// Allocates immutable storage. levels = 0 means a full mip chain.
Texture createTexture2D(int w, int h, GLenum internalFormat, int levels = 1);
Texture createTexture2DArray(int w, int h, int layers, GLenum internalFormat, int levels = 1);
Texture createTexture3D(int w, int h, int d, GLenum internalFormat, int levels = 1);
Texture createCubemapArray(int size, int cubes, GLenum internalFormat, int levels = 1);
// Sampler state stored on the texture object itself.
void setFilter(const Texture& t, GLenum minFilter, GLenum magFilter);
void setWrap(const Texture& t, GLenum wrap);
void setAnisotropy(const Texture& t, float amount);

struct Framebuffer {
    GLuint id = 0;
    void destroy() { if (id) glDeleteFramebuffers(1, &id); id = 0; }
};
Framebuffer createFramebuffer(std::initializer_list<const Texture*> colors, const Texture* depth, int level = 0);
Framebuffer createFramebufferLayer(const Texture* color, int colorLayer, const Texture* depth, int depthLayer, int level = 0);
bool checkFramebuffer(const Framebuffer& fb, const char* name);

struct Buffer {
    GLuint id = 0;
    size_t size = 0;
    void destroy() { if (id) glDeleteBuffers(1, &id); id = 0; size = 0; }
};
// Dynamic buffer updated with glNamedBufferSubData; grows when needed.
void ensureBuffer(Buffer& b, size_t size, GLbitfield flags = GL_DYNAMIC_STORAGE_BIT);
Buffer createBuffer(size_t size, const void* data, GLbitfield flags = 0);

// Fullscreen triangle helper: binds an empty VAO and draws 3 vertices (use with
// shaders/passes/fullscreen.vert).
void drawFullscreenTriangle();
// Compute helper: dispatch enough groups to cover (w,h,d) with the given local size.
void dispatch2D(int w, int h, int localX = 8, int localY = 8);

// Debug labels for RenderDoc / Nsight.
// With SCACELITH_GPU_PROFILE_GROUPS=1 next to SCACELITH_GPU_PROFILE, every debug group is also a
// profiler scope (named "[group]"): the finer split of a pass, the post chain's included.
struct DebugGroup {
    explicit DebugGroup(const char* name);
    ~DebugGroup();
    const char* name_ = nullptr;
    double start_ = 0;
};

// Frustum planes (xyz normal pointing inside, w distance) of a clip-space [0,1]-depth projection
// (reverse-Z or standard). Degenerate planes (infinite far plane) are dropped. Returns the count.
int frustumPlanes(const m::mat4& viewProj, m::vec4 out[6], bool sidesOnly = false);
// Sphere vs planes: false when the sphere is fully outside one plane.
inline bool sphereVisible(const m::vec4* planes, int n, m::vec3 c, float r) {
    for (int i = 0; i < n; ++i)
        if (planes[i].x * c.x + planes[i].y * c.y + planes[i].z * c.z + planes[i].w < -r) return false;
    return true;
}

// Opt-in pass profiler (SCACELITH_GPU_PROFILE=1): each scope is bracketed by glFinish and timed on
// the CPU, which measures real execution time on any driver (including llvmpipe). Zero cost when
// disabled. Results are logged by profileEndFrame() every 'every' frames.
struct ProfileScope {
    explicit ProfileScope(const char* name);
    ~ProfileScope();
    const char* name_;
    double start_ = 0;
};
void profileEndFrame();
// The finer profile (SCACELITH_GPU_PROFILE_GROUPS): whether it is on, and a measured scope added
// under 'name' (Renderer::drawScene times the main pass per material with it).
bool profileGroupsEnabled();
void profileAdd(const std::string& name, double ms);
double profileNowMs();

}  // namespace gpu
