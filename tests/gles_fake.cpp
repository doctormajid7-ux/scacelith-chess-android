// The fake libGLESv2 of tests/gles_fake.h: the classic ES 3.2 entry points the GL ES compatibility
// layer (src/gl/gl46_gles.cpp) resolves, over a state that behaves like a driver's.
//
// Every entry point below is called by the layer, never by the tests: the tests call the layer's
// emulations through the gl46 globals, exactly as the game does, and read what the layer did to the
// driver through the fakegles accessors at the bottom.
//
// The internals never call their own exported names: the exported functions are thin wrappers over
// the anonymous-namespace helpers, so nothing here can be interposed.
#include "gles_fake.h"

#include <cstring>
#include <set>
#include <unordered_map>

namespace {

constexpr int kMaxCalls = 8192;

// One pool of object names, as a driver keeps it: numbers from 1 up, and a deleted number is the
// next one handed out again (smallest first, so the tests are deterministic). That recycling is
// what makes the layer's texture-target table -- which has to forget the deleted names -- testable.
struct Names {
    GLuint next = 1;
    std::set<GLuint> freed;
    std::set<GLuint> live;

    GLuint alloc() {
        GLuint n;
        if (!freed.empty()) {
            auto it = freed.begin();
            n = *it;
            freed.erase(it);
        } else {
            n = next++;
        }
        live.insert(n);
        return n;
    }
    void release(GLuint n) {
        if (live.erase(n)) freed.insert(n);
    }
    bool isLive(GLuint n) const { return live.find(n) != live.end(); }
};

struct State {
    GLuint activeUnit = 0;
    std::unordered_map<unsigned long long, GLuint> texBinding;   // (unit << 32) | target
    std::unordered_map<GLenum, GLuint> bufferBinding;            // by binding target
    std::unordered_map<GLuint, GLuint> elementBuffer;            // by vertex array
    GLuint drawFb = 0, readFb = 0, renderbuffer = 0, vao = 0;
    std::unordered_map<GLuint, GLint> texWidth;                 // level 0 width, by texture
    Names tex, buffer, fbo, rbo, sampler, vaoNames;

    std::vector<fakegles::Call> calls;
    GLuint checkedFbo = 0;
    GLfloat lastClear = 0, lastParam = 0;
    int unknownName = 0;
};

State S;

unsigned long long texKey(GLuint unit, GLenum target) {
    return (static_cast<unsigned long long>(unit) << 32) | static_cast<unsigned long long>(target);
}
GLuint boundTex(GLuint unit, GLenum target) {
    auto it = S.texBinding.find(texKey(unit, target));
    return it == S.texBinding.end() ? 0 : it->second;
}
void setBoundTex(GLuint unit, GLenum target, GLuint texture) {
    S.texBinding[texKey(unit, target)] = texture;
}
GLuint boundBuf(GLenum target) {
    auto it = S.bufferBinding.find(target);
    return it == S.bufferBinding.end() ? 0 : it->second;
}

void record(const char* fn, std::initializer_list<long long> args) {
    if (S.calls.size() >= kMaxCalls) return;
    fakegles::Call c;
    c.fn = fn;
    int i = 0;
    for (long long a : args) {
        if (i >= 12) break;
        c.a[i++] = a;
    }
    S.calls.push_back(c);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------------------------
extern "C" void APIENTRY glGenTextures(GLsizei n, GLuint* textures) {
    record("glGenTextures", {n});
    if (!textures || n <= 0) return;
    for (GLsizei i = 0; i < n; ++i) textures[i] = S.tex.alloc();
}

extern "C" void APIENTRY glDeleteTextures(GLsizei n, const GLuint* textures) {
    record("glDeleteTextures", {n});
    if (!textures || n <= 0) return;
    for (GLsizei i = 0; i < n; ++i) {
        // A driver unbinds what it deletes.
        for (auto& entry : S.texBinding)
            if (entry.second == textures[i]) entry.second = 0;
        S.tex.release(textures[i]);
    }
}

extern "C" void APIENTRY glBindTexture(GLenum target, GLuint texture) {
    record("glBindTexture", {target, texture});
    if (texture != 0 && !S.tex.isLive(texture)) ++S.unknownName;
    setBoundTex(S.activeUnit, target, texture);
}

extern "C" void APIENTRY glActiveTexture(GLenum texture) {
    record("glActiveTexture", {texture});
    S.activeUnit = texture >= GL_TEXTURE0 ? texture - GL_TEXTURE0 : 0;
}

extern "C" void APIENTRY glTexStorage2D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width,
                                       GLsizei height) {
    record("glTexStorage2D", {target, levels, internalformat, width, height});
    if (GLuint t = boundTex(S.activeUnit, target)) S.texWidth[t] = width;
}

extern "C" void APIENTRY glTexStorage3D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width,
                                       GLsizei height, GLsizei depth) {
    record("glTexStorage3D", {target, levels, internalformat, width, height, depth});
    if (GLuint t = boundTex(S.activeUnit, target)) S.texWidth[t] = width;
}

extern "C" void APIENTRY glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                                        GLsizei height, GLenum format, GLenum type, const void* pixels) {
    record("glTexSubImage2D", {target, level, xoffset, yoffset, width, height, format, type});
}

extern "C" void APIENTRY glTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                                        GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type,
                                        const void* pixels) {
    record("glTexSubImage3D", {target, level, xoffset, yoffset, zoffset, width, height, depth, format, type});
}

extern "C" void APIENTRY glTexParameteri(GLenum target, GLenum pname, GLint param) {
    record("glTexParameteri", {target, pname, param});
}

extern "C" void APIENTRY glTexParameterf(GLenum target, GLenum pname, GLfloat param) {
    record("glTexParameterf", {target, pname, static_cast<long long>(param)});
    S.lastParam = param;
}

extern "C" void APIENTRY glGenerateMipmap(GLenum target) { record("glGenerateMipmap", {target}); }

extern "C" void APIENTRY glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint* params) {
    record("glGetTexLevelParameteriv", {target, level, pname});
    if (!params) return;
    // Only the width, which glTexStorage2D/3D recorded: a test that asks for it reads back the
    // storage of the texture that was bound, which is exactly what binding it first is for.
    params[0] = 0;
    if (pname == GL_TEXTURE_WIDTH) {
        if (GLuint t = boundTex(S.activeUnit, target)) {
            auto it = S.texWidth.find(t);
            if (it != S.texWidth.end()) params[0] = it->second;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Buffers
// ---------------------------------------------------------------------------------------------
extern "C" void APIENTRY glGenBuffers(GLsizei n, GLuint* buffers) {
    record("glGenBuffers", {n});
    if (!buffers || n <= 0) return;
    for (GLsizei i = 0; i < n; ++i) buffers[i] = S.buffer.alloc();
}

extern "C" void APIENTRY glBindBuffer(GLenum target, GLuint buffer) {
    record("glBindBuffer", {target, buffer});
    if (buffer != 0 && !S.buffer.isLive(buffer)) ++S.unknownName;
    if (target == GL_ELEMENT_ARRAY_BUFFER && S.vao != 0) {
        S.elementBuffer[S.vao] = buffer;   // part of the vertex array
        return;
    }
    S.bufferBinding[target] = buffer;
}

extern "C" void APIENTRY glBufferStorage(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags) {
    record("glBufferStorage", {target, size, flags});
}

extern "C" void APIENTRY glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
    record("glBufferSubData", {target, offset, size});
}

extern "C" void APIENTRY glCopyBufferSubData(GLenum readTarget, GLenum writeTarget, GLintptr readOffset,
                                            GLintptr writeOffset, GLsizeiptr size) {
    record("glCopyBufferSubData", {readTarget, writeTarget, readOffset, writeOffset, size});
}

extern "C" void APIENTRY glGetBufferParameteriv(GLenum target, GLenum pname, GLint* params) {
    record("glGetBufferParameteriv", {target, pname});
    if (params) params[0] = 0;
}

// ---------------------------------------------------------------------------------------------
// Framebuffers and renderbuffers
// ---------------------------------------------------------------------------------------------
extern "C" void APIENTRY glGenFramebuffers(GLsizei n, GLuint* framebuffers) {
    record("glGenFramebuffers", {n});
    if (!framebuffers || n <= 0) return;
    for (GLsizei i = 0; i < n; ++i) framebuffers[i] = S.fbo.alloc();
}

extern "C" void APIENTRY glBindFramebuffer(GLenum target, GLuint framebuffer) {
    record("glBindFramebuffer", {target, framebuffer});
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) S.drawFb = framebuffer;
    if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) S.readFb = framebuffer;
}

extern "C" void APIENTRY glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture,
                                               GLint level) {
    record("glFramebufferTexture2D", {target, attachment, textarget, texture, level});
}

extern "C" void APIENTRY glFramebufferTextureLayer(GLenum target, GLenum attachment, GLuint texture, GLint level,
                                                  GLint layer) {
    record("glFramebufferTextureLayer", {target, attachment, texture, level, layer});
}

extern "C" void APIENTRY glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget,
                                                  GLuint renderbuffer) {
    record("glFramebufferRenderbuffer", {target, attachment, renderbuffertarget, renderbuffer});
}

extern "C" void APIENTRY glDrawBuffers(GLsizei n, const GLenum* bufs) {
    record("glDrawBuffers", {n, bufs && n > 0 ? static_cast<long long>(bufs[0]) : 0});
}

extern "C" void APIENTRY glClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat* value) {
    record("glClearBufferfv", {buffer, drawbuffer});
    if (value) S.lastClear = value[0];
}

extern "C" GLenum APIENTRY glCheckFramebufferStatus(GLenum target) {
    record("glCheckFramebufferStatus", {target});
    S.checkedFbo = S.drawFb;
    return S.drawFb != 0 ? GL_FRAMEBUFFER_COMPLETE : GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
}

extern "C" void APIENTRY glGenRenderbuffers(GLsizei n, GLuint* renderbuffers) {
    record("glGenRenderbuffers", {n});
    if (!renderbuffers || n <= 0) return;
    for (GLsizei i = 0; i < n; ++i) renderbuffers[i] = S.rbo.alloc();
}

extern "C" void APIENTRY glBindRenderbuffer(GLenum target, GLuint renderbuffer) {
    record("glBindRenderbuffer", {target, renderbuffer});
    S.renderbuffer = renderbuffer;
}

extern "C" void APIENTRY glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height) {
    record("glRenderbufferStorage", {target, internalformat, width, height});
}

// ---------------------------------------------------------------------------------------------
// Samplers and vertex arrays
// ---------------------------------------------------------------------------------------------
extern "C" void APIENTRY glGenSamplers(GLsizei n, GLuint* samplers) {
    record("glGenSamplers", {n});
    if (!samplers || n <= 0) return;
    for (GLsizei i = 0; i < n; ++i) samplers[i] = S.sampler.alloc();
}

extern "C" void APIENTRY glGenVertexArrays(GLsizei n, GLuint* arrays) {
    record("glGenVertexArrays", {n});
    if (!arrays || n <= 0) return;
    for (GLsizei i = 0; i < n; ++i) arrays[i] = S.vaoNames.alloc();
}

extern "C" void APIENTRY glBindVertexArray(GLuint array) {
    record("glBindVertexArray", {array});
    S.vao = array;
}

extern "C" void APIENTRY glBindVertexBuffer(GLuint bindingindex, GLuint buffer, GLintptr offset, GLsizei stride) {
    record("glBindVertexBuffer", {bindingindex, buffer, offset, stride});
}

extern "C" void APIENTRY glVertexAttribFormat(GLuint attribindex, GLint size, GLenum type, GLboolean normalized,
                                             GLuint relativeoffset) {
    record("glVertexAttribFormat", {attribindex, size, type, normalized, relativeoffset});
}

extern "C" void APIENTRY glVertexAttribBinding(GLuint attribindex, GLuint bindingindex) {
    record("glVertexAttribBinding", {attribindex, bindingindex});
}

extern "C" void APIENTRY glEnableVertexAttribArray(GLuint index) {
    record("glEnableVertexAttribArray", {index});
}

extern "C" void APIENTRY glDisableVertexAttribArray(GLuint index) {
    record("glDisableVertexAttribArray", {index});
}

// ---------------------------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------------------------
extern "C" void APIENTRY glGetIntegerv(GLenum pname, GLint* data) {
    record("glGetIntegerv", {pname});
    if (!data) return;
    switch (pname) {
        case GL_ARRAY_BUFFER_BINDING: data[0] = boundBuf(GL_ARRAY_BUFFER); break;
        case GL_COPY_READ_BUFFER_BINDING: data[0] = boundBuf(GL_COPY_READ_BUFFER); break;
        case GL_COPY_WRITE_BUFFER_BINDING: data[0] = boundBuf(GL_COPY_WRITE_BUFFER); break;
        case GL_ELEMENT_ARRAY_BUFFER_BINDING: {
            auto it = S.elementBuffer.find(S.vao);
            data[0] = it == S.elementBuffer.end() ? 0 : it->second;
            break;
        }
        // GL_FRAMEBUFFER_BINDING and GL_DRAW_FRAMEBUFFER_BINDING are the same pname (0x8CA6).
        case GL_DRAW_FRAMEBUFFER_BINDING: data[0] = S.drawFb; break;
        case GL_READ_FRAMEBUFFER_BINDING: data[0] = S.readFb; break;
        case GL_RENDERBUFFER_BINDING: data[0] = S.renderbuffer; break;
        case GL_VERTEX_ARRAY_BINDING: data[0] = S.vao; break;
        default: data[0] = 0; break;
    }
}

// ---------------------------------------------------------------------------------------------
// What the tests read
// ---------------------------------------------------------------------------------------------
namespace fakegles {

void reset() { S = State{}; }
void clearLog() { S.calls.clear(); }

const std::vector<Call>& log() { return S.calls; }
int logSize() { return static_cast<int>(S.calls.size()); }

int countOf(const char* fn) {
    int n = 0;
    for (const Call& c : S.calls)
        if (c.fn == fn || std::strcmp(c.fn, fn) == 0) ++n;
    return n;
}

const Call* last(const char* fn) {
    for (auto it = S.calls.rbegin(); it != S.calls.rend(); ++it)
        if (std::strcmp(it->fn, fn) == 0) return &*it;
    return nullptr;
}

bool saw(const char* fn, std::initializer_list<long long> args) {
    for (const Call& c : S.calls) {
        if (std::strcmp(c.fn, fn) != 0) continue;
        size_t i = 0;
        bool same = true;
        for (long long a : args) {
            if (i >= 12 || c.a[i] != a) {
                same = false;
                break;
            }
            ++i;
        }
        if (same) return true;
    }
    return false;
}

GLuint boundTexture(GLuint unit, GLenum target) { return boundTex(unit, target); }
GLuint activeUnit() { return S.activeUnit; }
GLuint boundBuffer(GLenum target) { return boundBuf(target); }
GLuint elementBuffer(GLuint vao) {
    auto it = S.elementBuffer.find(vao);
    return it == S.elementBuffer.end() ? 0 : it->second;
}
GLuint boundFramebuffer(GLenum target) {
    if (target == GL_READ_FRAMEBUFFER) return S.readFb;
    return S.drawFb;
}
GLuint boundRenderbuffer() { return S.renderbuffer; }
GLuint boundVertexArray() { return S.vao; }
GLuint checkedFbo() { return S.checkedFbo; }
GLfloat lastClearValue() { return S.lastClear; }
GLfloat lastTexParameterfValue() { return S.lastParam; }
GLint storedWidth(GLuint texture) {
    auto it = S.texWidth.find(texture);
    return it == S.texWidth.end() ? 0 : it->second;
}
int unknownNameCalls() { return S.unknownName; }

}  // namespace fakegles
