// OpenGL ES 3.2 backend for the OpenGL 4.6 core API the renderer is written against.
//
// The renderer and every module above it use direct state access only (glCreate* / glNamed* /
// glTexture*): nothing in the game ever binds a texture, a buffer or a renderbuffer to edit it.
// That is what makes this file possible: GL ES 3.2 has no DSA, but a DSA call is always exactly
// "bind the object, do the classic call, put the binding back", and the bindings it has to put
// back are only ever the ones this file made itself.
//
// What GL ES 3.2 has (resolved normally by gl46::load, src/gl/gl46.cpp): immutable texture
// storage (glTexStorage*), shader storage buffers, image load/store, compute shaders, indirect
// draws, buffer textures, glDrawElementsBaseVertex, multisample storage, KHR_debug as core,
// glCopyImageSubData (ES 3.2 took it from EXT_copy_image), FBO blits, sampler objects,
// glBufferStorage, seamless cube maps.
//
// What it does not have, and what is emulated here (the 34 entry points the game actually calls):
//   * DSA: glCreateTextures / glTextureStorage* / glTextureSubImage* / glTextureParameter* /
//     glGenerateTextureMipmap / glGetTextureLevelParameteriv, glCreateBuffers /
//     glNamedBufferStorage / glNamedBufferSubData / glCopyNamedBufferSubData /
//     glInvalidateBufferData, glCreateFramebuffers / glNamedFramebufferTexture(Layer) /
//     glNamedFramebufferRenderbuffer / glNamedFramebufferDrawBuffer(s) /
//     glClearNamedFramebufferfv / glCheckNamedFramebufferStatus, glCreateRenderbuffers /
//     glNamedRenderbufferStorage(Multisample), glCreateSamplers, glCreateVertexArrays /
//     glVertexArrayAttribFormat / glVertexArrayAttribBinding / glVertexArrayVertexBuffer /
//     glVertexArrayElementBuffer / glEnableVertexArrayAttrib.
//   * glBindTextureUnit (ES binds by target).
//   * glClipControl: core ES clips z to [-w, w] and maps NDC [-1, 1] to depth [0, 1], not the
//     [0, 1] range the game's reverse-Z projections are built for. GL_EXT_clip_control (Adreno,
//     Mali and PowerVR drivers have it) gives the desktop behaviour; without it the vertex stages
//     remap z themselves (glesRemapDepth(), src/render/shader.cpp).
//
// To call the classic ES entry points, this file loads them from libGLESv2.so itself instead of
// using the gl46 globals: those hold the game's functions, and the names would collide.
//
// Build: compiled instead of nothing on Android (see android/app/src/main/cpp/CMakeLists.txt);
// the desktop builds never compile it.
#if defined(SCACELITH_GLES)

#include "gl46.h"
#include "gl46_gles.h"
#include "../core/log.h"

#include <dlfcn.h>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace {

// ---------------------------------------------------------------------------------------------
// The classic GL ES entry points used by the emulation, loaded from the driver.
// ---------------------------------------------------------------------------------------------
struct Gles {
    PFNGLGENTEXTURESPROC GenTextures;
    PFNGLDELETETEXTURESPROC DeleteTextures;
    PFNGLBINDTEXTUREPROC BindTexture;
    PFNGLACTIVETEXTUREPROC ActiveTexture;
    PFNGLTEXSTORAGE2DPROC TexStorage2D;
    PFNGLTEXSTORAGE3DPROC TexStorage3D;
    PFNGLTEXSUBIMAGE2DPROC TexSubImage2D;
    PFNGLTEXSUBIMAGE3DPROC TexSubImage3D;
    PFNGLTEXPARAMETERIPROC TexParameteri;
    PFNGLTEXPARAMETERFPROC TexParameterf;
    PFNGLGENERATEMIPMAPPROC GenerateMipmap;
    PFNGLGETTEXLEVELPARAMETERIVPROC GetTexLevelParameteriv;

    PFNGLGENBUFFERSPROC GenBuffers;
    PFNGLBINDBUFFERPROC BindBuffer;
    PFNGLBUFFERSTORAGEPROC BufferStorage;
    PFNGLBUFFERDATAPROC BufferData;
    PFNGLBUFFERSUBDATAPROC BufferSubData;
    PFNGLCOPYBUFFERSUBDATAPROC CopyBufferSubData;
    PFNGLGETBUFFERPARAMETERIVPROC GetBufferParameteriv;

    PFNGLGENFRAMEBUFFERSPROC GenFramebuffers;
    PFNGLBINDFRAMEBUFFERPROC BindFramebuffer;
    PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D;
    PFNGLFRAMEBUFFERTEXTURELAYERPROC FramebufferTextureLayer;
    PFNGLFRAMEBUFFERRENDERBUFFERPROC FramebufferRenderbuffer;
    PFNGLDRAWBUFFERSPROC DrawBuffers;
    PFNGLCLEARBUFFERFVPROC ClearBufferfv;
    PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus;

    PFNGLGENRENDERBUFFERSPROC GenRenderbuffers;
    PFNGLBINDRENDERBUFFERPROC BindRenderbuffer;
    PFNGLRENDERBUFFERSTORAGEPROC RenderbufferStorage;
    PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC RenderbufferStorageMultisample;

    PFNGLGENSAMPLERSPROC GenSamplers;

    PFNGLGENVERTEXARRAYSPROC GenVertexArrays;
    PFNGLBINDVERTEXARRAYPROC BindVertexArray;
    PFNGLBINDVERTEXBUFFERPROC BindVertexBuffer;
    PFNGLVERTEXATTRIBFORMATPROC VertexAttribFormat;
    PFNGLVERTEXATTRIBBINDINGPROC VertexAttribBinding;
    PFNGLENABLEVERTEXATTRIBARRAYPROC EnableVertexAttribArray;
    PFNGLDISABLEVERTEXATTRIBARRAYPROC DisableVertexAttribArray;

    PFNGLGETINTEGERVPROC GetIntegerv;
    PFNGLGETSTRINGIPROC GetStringi;
    PFNGLCLIPCONTROLPROC ClipControlEXT;   // GL_EXT_clip_control, null when the driver lacks it
};

Gles e;
bool g_ready = false;
bool g_remapDepth = false;   // no GL_EXT_clip_control: the vertex stages remap z (see emuClipControl)

void* loadSym(void* lib, const char* name) {
    void* p = dlsym(lib, name);
    if (!p) LOGE("GLES: missing entry point %s", name);
    return p;
}

// The emulation is only ever reached with a loaded ES 3.2 context; a driver that lacks one of
// these has bigger problems, so a null stays a null and the call after it fails loudly.
bool loadGles() {
    // The unit tests (tests/gles_tests.cpp) point this at a fake libGLESv2 they build next to
    // themselves: it is the only way to exercise this file on a machine that cannot run a device
    // or an emulator. A driver whose ES entry points live elsewhere can use it too.
    void* lib = nullptr;
    if (const char* overrideName = getenv("SCACELITH_GLES_LIBRARY")) {
        lib = dlopen(overrideName, RTLD_NOW | RTLD_LOCAL);
        if (!lib) {   // an explicit request that cannot be honoured is an error, not a fallback
            LOGE("GLES: cannot open %s (SCACELITH_GLES_LIBRARY): %s", overrideName, dlerror());
            return false;
        }
    }
    if (!lib) lib = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) lib = dlopen("libGLESv3.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        LOGE("GLES: cannot open libGLESv2.so: %s", dlerror());
        return false;
    }
#define LOAD(field, name) e.field = (decltype(e.field))loadSym(lib, name)
    LOAD(GenTextures, "glGenTextures");
    LOAD(DeleteTextures, "glDeleteTextures");
    LOAD(BindTexture, "glBindTexture");
    LOAD(ActiveTexture, "glActiveTexture");
    LOAD(TexStorage2D, "glTexStorage2D");
    LOAD(TexStorage3D, "glTexStorage3D");
    LOAD(TexSubImage2D, "glTexSubImage2D");
    LOAD(TexSubImage3D, "glTexSubImage3D");
    LOAD(TexParameteri, "glTexParameteri");
    LOAD(TexParameterf, "glTexParameterf");
    LOAD(GenerateMipmap, "glGenerateMipmap");
    LOAD(GetTexLevelParameteriv, "glGetTexLevelParameteriv");
    LOAD(GenBuffers, "glGenBuffers");
    LOAD(BindBuffer, "glBindBuffer");
    // glBufferStorage is not core ES (EXT_buffer_storage): optional, glBufferData stands in.
    e.BufferStorage = (PFNGLBUFFERSTORAGEPROC)dlsym(lib, "glBufferStorage");
    LOAD(BufferData, "glBufferData");
    LOAD(BufferSubData, "glBufferSubData");
    LOAD(CopyBufferSubData, "glCopyBufferSubData");
    LOAD(GetBufferParameteriv, "glGetBufferParameteriv");
    LOAD(GenFramebuffers, "glGenFramebuffers");
    LOAD(BindFramebuffer, "glBindFramebuffer");
    LOAD(FramebufferTexture2D, "glFramebufferTexture2D");
    LOAD(FramebufferTextureLayer, "glFramebufferTextureLayer");
    LOAD(FramebufferRenderbuffer, "glFramebufferRenderbuffer");
    LOAD(DrawBuffers, "glDrawBuffers");
    LOAD(ClearBufferfv, "glClearBufferfv");
    LOAD(CheckFramebufferStatus, "glCheckFramebufferStatus");
    LOAD(GenRenderbuffers, "glGenRenderbuffers");
    LOAD(BindRenderbuffer, "glBindRenderbuffer");
    LOAD(RenderbufferStorage, "glRenderbufferStorage");
    LOAD(RenderbufferStorageMultisample, "glRenderbufferStorageMultisample");
    LOAD(GenSamplers, "glGenSamplers");
    LOAD(GenVertexArrays, "glGenVertexArrays");
    LOAD(BindVertexArray, "glBindVertexArray");
    LOAD(BindVertexBuffer, "glBindVertexBuffer");
    LOAD(VertexAttribFormat, "glVertexAttribFormat");
    LOAD(VertexAttribBinding, "glVertexAttribBinding");
    LOAD(EnableVertexAttribArray, "glEnableVertexAttribArray");
    LOAD(DisableVertexAttribArray, "glDisableVertexAttribArray");
    LOAD(GetIntegerv, "glGetIntegerv");
    e.GetStringi = (PFNGLGETSTRINGIPROC)dlsym(lib, "glGetStringi");
#undef LOAD
    return e.GenTextures && e.BindTexture && e.TexStorage2D && e.GenFramebuffers && e.GetIntegerv &&
           e.DisableVertexAttribArray;
}

// ---------------------------------------------------------------------------------------------
// The one piece of state GL ES cannot be asked about: the target of a texture object.
// GL ES has no target-free texture call at all, and the game creates every texture with
// glCreateTextures(target, ...), which is emulated below, so this table is complete. Deletions
// take their entry out (GL recycles names).
// ---------------------------------------------------------------------------------------------
std::unordered_map<GLuint, GLenum> g_texTarget;

GLenum targetOf(GLuint texture) {
    if (texture == 0) return GL_TEXTURE_2D;
    auto it = g_texTarget.find(texture);
    if (it != g_texTarget.end()) return it->second;
    // Never seen: a texture the engine did not create through gpu:: (nothing does today).
    LOGW("GLES: texture %u has no recorded target, assuming GL_TEXTURE_2D", texture);
    return GL_TEXTURE_2D;
}

bool isArrayTarget(GLenum t) { return t == GL_TEXTURE_2D_ARRAY || t == GL_TEXTURE_CUBE_MAP_ARRAY || t == GL_TEXTURE_3D; }

// ---------------------------------------------------------------------------------------------
// Binding slots. The game never calls glBindTexture / glBindBuffer / glRenderbufferStorage /
// glActiveTexture, so every binding these emulations might disturb is one they made themselves:
// the bound texture of each (unit, target) slot is tracked here, and restored exactly.
// ---------------------------------------------------------------------------------------------
constexpr int kUnits = 32;   // GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS is at least 16 in ES 3.2
// The active texture unit: the game never calls glActiveTexture (its binds all go through
// glBindTextureUnit), so this cache stays authoritative and keeps glActiveTexture out of the
// per-draw path.
int g_activeUnit = 0;
void useUnit(int unit) {
    if (unit != g_activeUnit) {
        e.ActiveTexture(GL_TEXTURE0 + GLenum(unit));
        g_activeUnit = unit;
    }
}
// One slot per distinct target a unit has ever bound, appended the first time that unit meets it.
// The game uses a handful of targets, but nothing caps a unit's slots: a fixed table would need an
// overflow branch, and past its size a slot would hold some *other* target's texture and put that
// back on unbind -- silently editing the wrong object, which is the one failure this file exists
// to prevent. Growth happens only when a unit meets a new target, never in the steady-state path.
struct Slot {
    GLenum target;
    GLuint texture;
};
std::vector<Slot> g_slots[kUnits];
size_t slotIndex(int unit, GLenum target) {
    std::vector<Slot>& slots = g_slots[unit];
    for (size_t i = 0; i < slots.size(); ++i)
        if (slots[i].target == target) return i;
    slots.push_back({target, 0});   // first time this unit meets this target
    return slots.size() - 1;
}
void setSlot(int unit, GLenum target, GLuint texture) { g_slots[unit][slotIndex(unit, target)].texture = texture; }
GLuint getSlot(int unit, GLenum target) { return g_slots[unit][slotIndex(unit, target)].texture; }

// The unit every bind-to-edit emulation uses. It is unit 0, which the game itself uses through
// glBindTextureUnit; whatever stood there is put back before the emulation returns. The active
// unit is pinned to GL_TEXTURE0 at install time and the game never changes it, so the classic
// calls below land on unit 0 without glActiveTexture in the hot path.
constexpr int kScratchUnit = 0;

// Binds 'texture' (of target 'target') to the scratch unit and returns the slot to restore.
struct Scratch {
    GLenum target;
    GLuint previous;
    explicit Scratch(GLenum t) : target(t), previous(getSlot(kScratchUnit, t)) { useUnit(kScratchUnit); }
    // A texture's bind-to-edit needs the texture actually bound; the game's own slot is saved by
    // the constructor and restored by the destructor.
    ~Scratch() { e.BindTexture(target, previous); setSlot(kScratchUnit, target, previous); }
    void bind(GLuint texture) { e.BindTexture(target, texture); setSlot(kScratchUnit, target, texture); }
};

// ---------------------------------------------------------------------------------------------
// DSA: textures
// ---------------------------------------------------------------------------------------------
void APIENTRY emuCreateTextures(GLenum target, GLsizei n, GLuint* textures) {
    e.GenTextures(n, textures);
    for (GLsizei i = 0; i < n; ++i) g_texTarget[textures[i]] = target;
}
void APIENTRY emuDeleteTextures(GLsizei n, const GLuint* textures) {
    for (GLsizei i = 0; i < n; ++i) {
        g_texTarget.erase(textures[i]);
        for (int unit = 0; unit < kUnits; ++unit)
            for (Slot& slot : g_slots[unit])
                if (slot.texture == textures[i]) slot.texture = 0;
    }
    e.DeleteTextures(n, textures);
}
void APIENTRY emuBindTextureUnit(GLuint unit, GLuint texture) {
    if (unit >= kUnits) return;
    GLenum target = texture ? targetOf(texture) : GL_TEXTURE_2D;
    // The renderer rebinds every material texture on every draw: skip the driver calls when the
    // slot already holds the texture (the table is exact, see "Binding slots" above).
    Slot& slot = g_slots[unit][slotIndex(int(unit), target)];
    if (slot.texture == texture) return;
    useUnit(int(unit));   // glBindTextureUnit must not disturb the active unit; this one restores it
    e.BindTexture(target, texture);
    slot.texture = texture;
}
void APIENTRY emuTextureStorage2D(GLuint texture, GLsizei levels, GLenum internalformat, GLsizei w, GLsizei h) {
    Scratch s(targetOf(texture));
    s.bind(texture);
    e.TexStorage2D(s.target, levels, internalformat, w, h);
}
void APIENTRY emuTextureStorage3D(GLuint texture, GLsizei levels, GLenum internalformat, GLsizei w, GLsizei h, GLsizei d) {
    Scratch s(targetOf(texture));
    s.bind(texture);
    e.TexStorage3D(s.target, levels, internalformat, w, h, d);
}
void APIENTRY emuTextureSubImage2D(GLuint texture, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format,
                                   GLenum type, const void* pixels) {
    Scratch s(targetOf(texture));
    s.bind(texture);
    e.TexSubImage2D(s.target, level, x, y, w, h, format, type, pixels);
}
void APIENTRY emuTextureSubImage3D(GLuint texture, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d,
                                   GLenum format, GLenum type, const void* pixels) {
    Scratch s(targetOf(texture));
    s.bind(texture);
    e.TexSubImage3D(s.target, level, x, y, z, w, h, d, format, type, pixels);
}
void APIENTRY emuTextureParameteri(GLuint texture, GLenum pname, GLint param) {
    GLenum target = targetOf(texture);
    if (pname == GL_TEXTURE_WRAP_R && !isArrayTarget(target)) return;   // ES: meaningless on 2D
    Scratch s(target);
    s.bind(texture);
    e.TexParameteri(target, pname, param);
}
void APIENTRY emuTextureParameterf(GLuint texture, GLenum pname, GLfloat param) {
    GLenum target = targetOf(texture);
    if (pname == GL_TEXTURE_WRAP_R && !isArrayTarget(target)) return;
    // GL_TEXTURE_MAX_ANISOTROPY has the same value as the EXT name ES drivers know (0x84FE),
    // so the parameter is passed through as it is.
    Scratch s(target);
    s.bind(texture);
    e.TexParameterf(target, pname, param);
}
void APIENTRY emuGenerateTextureMipmap(GLuint texture) {
    Scratch s(targetOf(texture));
    s.bind(texture);
    e.GenerateMipmap(s.target);
}
void APIENTRY emuGetTextureLevelParameteriv(GLuint texture, GLint level, GLenum pname, GLint* params) {
    Scratch s(targetOf(texture));
    s.bind(texture);
    e.GetTexLevelParameteriv(s.target, level, pname, params);
}

// ---------------------------------------------------------------------------------------------
// DSA: buffers
// ---------------------------------------------------------------------------------------------
void APIENTRY emuCreateBuffers(GLsizei n, GLuint* buffers) { e.GenBuffers(n, buffers); }
// When the driver has no glBufferStorage (it is not core ES 3.2), a mutable allocation replaces
// it: glBufferData reserves the same store, and the game only ever passes GL_DYNAMIC_STORAGE_BIT —
// it updates its buffers with (Named)BufferSubData and never maps them or re-specifies immutable
// memory.
void APIENTRY emuNamedBufferStorage(GLuint buffer, GLsizeiptr size, const void* data, GLbitfield flags) {
    GLint prev = 0;
    e.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev);
    e.BindBuffer(GL_ARRAY_BUFFER, buffer);
    if (e.BufferStorage) e.BufferStorage(GL_ARRAY_BUFFER, size, data, flags);
    else if (e.BufferData) e.BufferData(GL_ARRAY_BUFFER, size, data, GL_DYNAMIC_DRAW);
    e.BindBuffer(GL_ARRAY_BUFFER, GLuint(prev));
}
void APIENTRY emuNamedBufferSubData(GLuint buffer, GLintptr offset, GLsizeiptr size, const void* data) {
    GLint prev = 0;
    e.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev);
    e.BindBuffer(GL_ARRAY_BUFFER, buffer);
    e.BufferSubData(GL_ARRAY_BUFFER, offset, size, data);
    e.BindBuffer(GL_ARRAY_BUFFER, GLuint(prev));
}
void APIENTRY emuCopyNamedBufferSubData(GLuint readBuffer, GLuint writeBuffer, GLintptr readOffset, GLintptr writeOffset,
                                        GLsizeiptr size) {
    GLint prevRead = 0, prevWrite = 0;
    e.GetIntegerv(GL_COPY_READ_BUFFER_BINDING, &prevRead);
    e.GetIntegerv(GL_COPY_WRITE_BUFFER_BINDING, &prevWrite);
    e.BindBuffer(GL_COPY_READ_BUFFER, readBuffer);
    e.BindBuffer(GL_COPY_WRITE_BUFFER, writeBuffer);
    e.CopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, readOffset, writeOffset, size);
    e.BindBuffer(GL_COPY_READ_BUFFER, GLuint(prevRead));
    e.BindBuffer(GL_COPY_WRITE_BUFFER, GLuint(prevWrite));
}
// Dropping the contents of a whole buffer is a driver hint on desktop; ES has nothing like it and
// the game never reads the buffer back, so it is exact to do nothing.
void APIENTRY emuInvalidateBufferData(GLuint) {}
void APIENTRY emuInvalidateBufferSubData(GLuint, GLintptr, GLsizeiptr) {}

// ---------------------------------------------------------------------------------------------
// DSA: framebuffers and renderbuffers
// ---------------------------------------------------------------------------------------------
class FboGuard {
public:
    FboGuard() {
        e.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_);
        e.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_);
    }
    ~FboGuard() {
        // The guard is only ever used to touch the draw-side attachments the game set up.
        e.BindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(draw_));
        e.BindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(read_));
    }
    void bind(GLuint fb) { e.BindFramebuffer(GL_DRAW_FRAMEBUFFER, fb); }

private:
    GLint draw_ = 0, read_ = 0;
};

void APIENTRY emuCreateFramebuffers(GLsizei n, GLuint* ids) { e.GenFramebuffers(n, ids); }
void APIENTRY emuNamedFramebufferTexture(GLuint fb, GLenum attachment, GLuint texture, GLint level) {
    GLenum target = texture ? targetOf(texture) : GL_TEXTURE_2D;
    FboGuard g;
    g.bind(fb);
    if (texture == 0) {   // detach: ES detaches through any face target
        e.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, attachment, GL_TEXTURE_2D, 0, 0);
        return;
    }
    if (isArrayTarget(target) || target == GL_TEXTURE_CUBE_MAP) {
        // Desktop attaches a whole array / cube map at 'level'; an unlayered attachment in ES is
        // layer 0 of it, which is what the game's non-layered targets mean here.
        e.FramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, attachment, texture, level, 0);
        return;
    }
    e.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, attachment, target, texture, level);
}
void APIENTRY emuNamedFramebufferTextureLayer(GLuint fb, GLenum attachment, GLuint texture, GLint level, GLint layer) {
    FboGuard g;
    g.bind(fb);
    e.FramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, attachment, texture, level, layer);
}
void APIENTRY emuNamedFramebufferRenderbuffer(GLuint fb, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer) {
    FboGuard g;
    g.bind(fb);
    e.FramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, attachment, renderbuffertarget, renderbuffer);
}
void APIENTRY emuNamedFramebufferDrawBuffer(GLuint fb, GLenum buf) {
    FboGuard g;
    g.bind(fb);
    e.DrawBuffers(1, &buf);
}
void APIENTRY emuNamedFramebufferDrawBuffers(GLuint fb, GLsizei n, const GLenum* bufs) {
    FboGuard g;
    g.bind(fb);
    e.DrawBuffers(n, bufs);
}
void APIENTRY emuClearNamedFramebufferfv(GLuint fb, GLenum buffer, GLint drawbuffer, const GLfloat* value) {
    FboGuard g;
    g.bind(fb);
    e.ClearBufferfv(buffer, drawbuffer, value);
}
GLenum APIENTRY emuCheckNamedFramebufferStatus(GLuint fb, GLenum target) {
    FboGuard g;
    g.bind(fb);
    return e.CheckFramebufferStatus(target);
}
void APIENTRY emuCreateRenderbuffers(GLsizei n, GLuint* ids) { e.GenRenderbuffers(n, ids); }
void APIENTRY emuNamedRenderbufferStorage(GLuint rb, GLenum internalformat, GLsizei w, GLsizei h) {
    GLint prev = 0;
    e.GetIntegerv(GL_RENDERBUFFER_BINDING, &prev);
    e.BindRenderbuffer(GL_RENDERBUFFER, rb);
    e.RenderbufferStorage(GL_RENDERBUFFER, internalformat, w, h);
    e.BindRenderbuffer(GL_RENDERBUFFER, GLuint(prev));
}

void APIENTRY emuNamedRenderbufferStorageMultisample(GLuint rb, GLsizei samples, GLenum internalformat, GLsizei w, GLsizei h) {
    GLint prev = 0;
    e.GetIntegerv(GL_RENDERBUFFER_BINDING, &prev);
    e.BindRenderbuffer(GL_RENDERBUFFER, rb);
    e.RenderbufferStorageMultisample(GL_RENDERBUFFER, samples, internalformat, w, h);
    e.BindRenderbuffer(GL_RENDERBUFFER, GLuint(prev));
}

// ---------------------------------------------------------------------------------------------
// DSA: samplers and vertex arrays
// ---------------------------------------------------------------------------------------------
void APIENTRY emuCreateSamplers(GLsizei n, GLuint* samplers) { e.GenSamplers(n, samplers); }

class VaoGuard {
public:
    VaoGuard() { e.GetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_); }
    ~VaoGuard() { e.BindVertexArray(GLuint(prev_)); }
    void bind(GLuint vao) { e.BindVertexArray(vao); }

private:
    GLint prev_ = 0;
};

void APIENTRY emuCreateVertexArrays(GLsizei n, GLuint* arrays) { e.GenVertexArrays(n, arrays); }
void APIENTRY emuVertexArrayAttribFormat(GLuint vao, GLuint attribindex, GLint size, GLenum type, GLboolean normalized,
                                         GLuint relativeoffset) {
    VaoGuard g;
    g.bind(vao);
    e.VertexAttribFormat(attribindex, size, type, normalized, relativeoffset);
}
void APIENTRY emuVertexArrayAttribBinding(GLuint vao, GLuint attribindex, GLuint bindingindex) {
    VaoGuard g;
    g.bind(vao);
    e.VertexAttribBinding(attribindex, bindingindex);
}
void APIENTRY emuVertexArrayVertexBuffer(GLuint vao, GLuint bindingindex, GLuint buffer, GLintptr offset, GLsizei stride) {
    VaoGuard g;
    g.bind(vao);
    e.BindVertexBuffer(bindingindex, buffer, offset, stride);
}
// GL_ELEMENT_ARRAY_BUFFER binding is part of the vertex array object, so binding it while the
// array is bound needs no restoring.
void APIENTRY emuVertexArrayElementBuffer(GLuint vao, GLuint buffer) {
    VaoGuard g;
    g.bind(vao);
    e.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer);
}
void APIENTRY emuEnableVertexArrayAttrib(GLuint vao, GLuint index) {
    VaoGuard g;
    g.bind(vao);
    e.EnableVertexAttribArray(index);
}
void APIENTRY emuDisableVertexArrayAttrib(GLuint vao, GLuint index) {
    VaoGuard g;
    g.bind(vao);
    e.DisableVertexAttribArray(index);
}

// ---------------------------------------------------------------------------------------------
// glClipControl. Core ES has the classic GL convention: clip z in [-w, w], NDC z in [-1, 1] mapped
// to depth 0.5 * z + 0.5. The game's projections are reverse-Z with a [0, 1] clip range
// (src/math/math.h), so without the call every depth lands in [0.5, 1]: the sun's shadow
// comparisons, the position reconstruction of the AO / SSR / volumetrics / DOF passes and the
// sky's "depth == 0" test all read wrong values, and the scene comes out black or garbled.
//
// GL_EXT_clip_control is the exact desktop call (same enums). A driver without it gets the same
// result from the vertex stages: src/render/shader.cpp appends z' = 2z - w to every vertex shader
// when glesRemapDepth() is true (clip and depth then match the [0, 1] convention, with less
// reverse-Z precision far away).
// ---------------------------------------------------------------------------------------------
bool hasExtension(const char* name) {
    if (!e.GetStringi) return false;
    GLint n = 0;
    e.GetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; ++i) {
        const char* ext = (const char*)e.GetStringi(GL_EXTENSIONS, GLuint(i));
        if (ext && std::strcmp(ext, name) == 0) return true;
    }
    return false;
}

void loadClipControl() {
    e.ClipControlEXT = nullptr;
    if (hasExtension("GL_EXT_clip_control")) {
        // Extension entry points come from eglGetProcAddress (the ES library need not export
        // them); looked up by name so this file does not link against EGL.
        using GetProc = void* (*)(const char*);
        // (glXGetProcAddressARB: the desktop scacelith_gles target, whose context is a GLX one.)
        for (const char* loader : {"eglGetProcAddress", "glXGetProcAddressARB"})
            if (!e.ClipControlEXT)
                if (auto proc = (GetProc)dlsym(RTLD_DEFAULT, loader))
                    e.ClipControlEXT = (PFNGLCLIPCONTROLPROC)proc("glClipControlEXT");
    }
    g_remapDepth = e.ClipControlEXT == nullptr;
    LOGI("GLES: depth convention %s", g_remapDepth ? "remapped in the vertex shaders (no GL_EXT_clip_control)"
                                                   : "set by GL_EXT_clip_control");
}

void APIENTRY emuClipControl(GLenum origin, GLenum depth) {
    if (e.ClipControlEXT) {
        e.ClipControlEXT(origin, depth);
        return;
    }
    if (origin == GL_LOWER_LEFT && depth == GL_ZERO_TO_ONE) return;   // the shader remap does this one
    LOGE("GLES: glClipControl(0x%x, 0x%x) is not supported by the ES backend", origin, depth);
}

}  // namespace

namespace gl46 {

void installGlesFallbacks() {
    if (!loadGles()) {
        g_ready = false;   // honest even when a layer was installed before and this is a retry
        return;
    }
    e.ActiveTexture(GL_TEXTURE0);   // the game pins its texture unit; every emulation assumes it
    loadClipControl();
    // The 33 entry points the game calls that ES 3.2 does not provide. They are assigned
    // unconditionally: on EGL, eglGetProcAddress returns a non-null stub for names the driver
    // does not implement (here: a pointer into anonymous memory, and calling it faults with
    // SIGSEGV "trying to execute non-executable memory"), so a null check cannot tell a missing
    // function from a present one — the desktop-GL assumption this file's null checks once made.
    // The emulators implement exactly the GL 4.6 semantics the game was written against, which is
    // what this backend must deliver; glDeleteTextures is wrapped even when the driver has it
    // (it is core ES): the target table must forget the names the GL recycles.
    glCreateTextures = emuCreateTextures;
    glBindTextureUnit = emuBindTextureUnit;
    glTextureStorage2D = emuTextureStorage2D;
    glTextureStorage3D = emuTextureStorage3D;
    glTextureSubImage2D = emuTextureSubImage2D;
    glTextureSubImage3D = emuTextureSubImage3D;
    glTextureParameteri = emuTextureParameteri;
    glTextureParameterf = emuTextureParameterf;
    glGenerateTextureMipmap = emuGenerateTextureMipmap;
    glGetTextureLevelParameteriv = emuGetTextureLevelParameteriv;
    glCreateBuffers = emuCreateBuffers;
    glNamedBufferStorage = emuNamedBufferStorage;
    glNamedBufferSubData = emuNamedBufferSubData;
    glCopyNamedBufferSubData = emuCopyNamedBufferSubData;
    glInvalidateBufferData = emuInvalidateBufferData;
    glInvalidateBufferSubData = emuInvalidateBufferSubData;
    glCreateFramebuffers = emuCreateFramebuffers;
    glNamedFramebufferTexture = emuNamedFramebufferTexture;
    glNamedFramebufferTextureLayer = emuNamedFramebufferTextureLayer;
    glNamedFramebufferRenderbuffer = emuNamedFramebufferRenderbuffer;
    glNamedFramebufferDrawBuffer = emuNamedFramebufferDrawBuffer;
    glNamedFramebufferDrawBuffers = emuNamedFramebufferDrawBuffers;
    glClearNamedFramebufferfv = emuClearNamedFramebufferfv;
    glCheckNamedFramebufferStatus = emuCheckNamedFramebufferStatus;
    glCreateRenderbuffers = emuCreateRenderbuffers;
    glNamedRenderbufferStorage = emuNamedRenderbufferStorage;
    glNamedRenderbufferStorageMultisample = emuNamedRenderbufferStorageMultisample;
    glCreateSamplers = emuCreateSamplers;
    glCreateVertexArrays = emuCreateVertexArrays;
    glVertexArrayAttribFormat = emuVertexArrayAttribFormat;
    glVertexArrayAttribBinding = emuVertexArrayAttribBinding;
    glVertexArrayVertexBuffer = emuVertexArrayVertexBuffer;
    glVertexArrayElementBuffer = emuVertexArrayElementBuffer;
    glEnableVertexArrayAttrib = emuEnableVertexArrayAttrib;
    glDisableVertexArrayAttrib = emuDisableVertexArrayAttrib;
    glClipControl = emuClipControl;
    glDeleteTextures = emuDeleteTextures;
    g_ready = true;
    LOGI("GLES: compatibility layer installed");
}

bool glesReady() { return g_ready; }

bool glesRemapDepth() { return g_remapDepth; }

bool glesHasExtension(const char* name) { return g_ready && hasExtension(name); }

void resetGlesFallbacks() {
    g_texTarget.clear();
    for (int unit = 0; unit < kUnits; ++unit) g_slots[unit].clear();
    g_activeUnit = 0;
}

}  // namespace gl46

#endif  // SCACELITH_GLES
