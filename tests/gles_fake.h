// A fake libGLESv2 for the GL ES 3.2 compatibility layer (src/gl/gl46_gles.cpp).
//
// The layer is Android-only code, and the machine this port was prepared on can run neither a
// device nor an emulator (no /dev/kvm, x86-64 system images only). Its bookkeeping, though, is
// platform-neutral: it dlopens a library called libGLESv2.so and resolves the classic ES entry
// points out of it. Point it at this one instead (SCACELITH_GLES_LIBRARY, which the layer honours)
// and every emulation can be run, and every classic call it makes, be seen.
//
// tests/gles_fake.cpp implements that library: the classic ES 3.2 entry points the layer resolves,
// over a state that behaves like a driver's (bindings per texture unit and per target, buffers,
// framebuffers, renderbuffers, vertex arrays, per-object storage) and a log of every call made
// into it. Deliberately driver-like, not GL-like: it does not validate anything, it recycles
// object names in the smallest-first order a driver's name pool does (which is what makes the
// layer's texture-target table testable), and it records instead of complaining.
//
// Declared here rather than resolved through dlsym: the library is linked into scacelith_tests as
// well, so the tests call these directly. The two views -- the layer's dlopen and the test's link
// -- are the same instance of it.
#pragma once
#include <initializer_list>
#include <vector>

#include "../third_party/khronos/GL/glcorearb.h"

namespace fakegles {

// One call into the fake driver: the entry point's name and its first twelve arguments, held as
// integers (pointers and floats are cast or bit-cast into them; a couple of floats have their own
// accessor below).
struct Call {
    const char* fn = nullptr;
    long long a[12] = {};
};

// A freshly created context: no binding, no name handed out, an empty log.
void reset();
void clearLog();

const std::vector<Call>& log();
int logSize();
int countOf(const char* fn);
const Call* last(const char* fn);
// Whether 'fn' was called with (at least) these first arguments.
bool saw(const char* fn, std::initializer_list<long long> args);

// The driver state the layer claims to have restored, or to have left alone.
GLuint boundTexture(GLuint unit, GLenum target);
GLuint activeUnit();
GLuint boundBuffer(GLenum target);
GLuint elementBuffer(GLuint vao);
GLuint boundFramebuffer(GLenum target);
GLuint boundRenderbuffer();
GLuint boundVertexArray();
// The framebuffer that was bound when glCheckFramebufferStatus was last called.
GLuint checkedFbo();
GLfloat lastClearValue();
GLfloat lastTexParameterfValue();
// The width glTexStorage2D/3D recorded for a texture (0 when it has no storage).
GLint storedWidth(GLuint texture);
// Calls naming an object the fake never handed out -- the layer passing through a name of its own,
// or a name it should have forgotten.
int unknownNameCalls();

}  // namespace fakegles
