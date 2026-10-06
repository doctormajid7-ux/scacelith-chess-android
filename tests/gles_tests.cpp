// Tests for the GL ES 3.2 compatibility layer (src/gl/gl46_gles.cpp).
//
// That file is Android-only code, and the machine this port was prepared on can run neither a
// device nor an emulator (no /dev/kvm, x86-64 system images only). What the layer *does* is
// platform-neutral, though, so this is where it is exercised: the layer is pointed at the fake
// libGLESv2 of tests/gles_fake.cpp (SCACELITH_GLES_LIBRARY, which the layer honours), the tests
// call the layer through the gl46 globals exactly as the game does, and read the driver state
// through the fake's accessors.
//
// What is under test is not "does GL work" (the fake has no GPU) but the two things that would
// make the game show a black screen on a device without saying why:
//   * every entry point GL ES 3.2 does not provide, that the game calls, is really installed;
//   * every emulation leaves the driver exactly as it found it, and lands each classic call on the
//     object and the target it was asked for (the layer edits objects by binding them: a missing
//     bind, or a restored-to bind, silently edits the wrong object).
//
// Built on non-Windows hosts only (see the tests section of CMakeLists.txt).
#include "test.h"

#include "gles_fake.h"

#if defined(SCACELITH_GLES_TESTS)

#include "gl/gl46.h"
#include "gl/gl46_gles.h"

// SCACELITH_GLES_FAKE_LIB: the absolute path of the fake libGLESv2 this binary is linked against,
// written by CMakeLists.txt (a generated header, so no quoting has to survive a command line).
#include "gles_fake_path.h"

#include <cstdlib>
#include <dlfcn.h>

namespace {

// The library the layer must resolve. SCACELITH_GLES_FAKE_LIB is the path of the fake this test
// binary is linked against, so the layer's dlopen and the test's calls are the same instance.
void setFakeLibrary(const char* path) { setenv("SCACELITH_GLES_LIBRARY", path, 1); }

// A clean layer over a clean driver: what every test starts from. The log is left as the install
// left it (the layer pinning the scratch unit is the first thing in it, which is how
// gles_installs_every_entry_point_the_game_calls knows the fake is the library it loaded).
void setup() {
    setFakeLibrary(SCACELITH_GLES_FAKE_LIB);
    fakegles::reset();
    gl46::resetGlesFallbacks();
    gl46::installGlesFallbacks();
}

GLuint makeTexture(GLenum target) {
    GLuint texture = 0;
    glCreateTextures(target, 1, &texture);
    return texture;
}

GLuint makeBuffer() {
    GLuint buffer = 0;
    glCreateBuffers(1, &buffer);
    return buffer;
}

GLuint makeFramebuffer() {
    GLuint fb = 0;
    glCreateFramebuffers(1, &fb);
    return fb;
}

// The driver's own entry points, resolved the way the layer resolves them: by name, out of the fake
// library, because the gl46 globals hold these very names and on a desktop build they are null
// (the game fills them from the driver). The game never calls any of them -- it only ever binds
// through the layer -- so a test that has to leave the driver in a state the layer must restore is
// standing in for "something else in the process bound this".
struct FakeDriver {
    void (*bindBuffer)(GLenum, GLuint) = nullptr;
    void (*bindFramebuffer)(GLenum, GLuint) = nullptr;
    void (*bindRenderbuffer)(GLenum, GLuint) = nullptr;
    void (*bindVertexArray)(GLuint) = nullptr;
};

FakeDriver& driver() {
    static FakeDriver d = [] {
        void* lib = dlopen(SCACELITH_GLES_FAKE_LIB, RTLD_NOW | RTLD_LOCAL);
        FakeDriver fn;
        fn.bindBuffer = reinterpret_cast<decltype(fn.bindBuffer)>(dlsym(lib, "glBindBuffer"));
        fn.bindFramebuffer = reinterpret_cast<decltype(fn.bindFramebuffer)>(dlsym(lib, "glBindFramebuffer"));
        fn.bindRenderbuffer = reinterpret_cast<decltype(fn.bindRenderbuffer)>(dlsym(lib, "glBindRenderbuffer"));
        fn.bindVertexArray = reinterpret_cast<decltype(fn.bindVertexArray)>(dlsym(lib, "glBindVertexArray"));
        return fn;
    }();
    return d;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Completeness: the entry points that are the whole reason this file exists.
// ---------------------------------------------------------------------------------------------
TEST(gles_installs_every_entry_point_the_game_calls) {
    // The install itself, without setup()'s trailing clearLog: one of its own calls into the driver
    // (it pins the scratch texture unit) is how this test tells the fake was the library loaded.
    setFakeLibrary(SCACELITH_GLES_FAKE_LIB);
    fakegles::reset();
    gl46::resetGlesFallbacks();
    gl46::installGlesFallbacks();
    CHECK(gl46::glesReady());
    // The 33 entry points GL ES 3.2 does not have and the game calls, plus the wrapped
    // glDeleteTextures: a null here is a crash on the first frame, or a silent no-op.
    CHECK(glCreateTextures != nullptr);
    CHECK(glBindTextureUnit != nullptr);
    CHECK(glTextureStorage2D != nullptr);
    CHECK(glTextureStorage3D != nullptr);
    CHECK(glTextureSubImage2D != nullptr);
    CHECK(glTextureSubImage3D != nullptr);
    CHECK(glTextureParameteri != nullptr);
    CHECK(glTextureParameterf != nullptr);
    CHECK(glGenerateTextureMipmap != nullptr);
    CHECK(glGetTextureLevelParameteriv != nullptr);
    CHECK(glCreateBuffers != nullptr);
    CHECK(glNamedBufferStorage != nullptr);
    CHECK(glNamedBufferSubData != nullptr);
    CHECK(glCopyNamedBufferSubData != nullptr);
    CHECK(glInvalidateBufferData != nullptr);
    CHECK(glInvalidateBufferSubData != nullptr);
    CHECK(glCreateFramebuffers != nullptr);
    CHECK(glNamedFramebufferTexture != nullptr);
    CHECK(glNamedFramebufferTextureLayer != nullptr);
    CHECK(glNamedFramebufferRenderbuffer != nullptr);
    CHECK(glNamedFramebufferDrawBuffer != nullptr);
    CHECK(glNamedFramebufferDrawBuffers != nullptr);
    CHECK(glClearNamedFramebufferfv != nullptr);
    CHECK(glCheckNamedFramebufferStatus != nullptr);
    CHECK(glCreateRenderbuffers != nullptr);
    CHECK(glNamedRenderbufferStorage != nullptr);
    CHECK(glCreateSamplers != nullptr);
    CHECK(glCreateVertexArrays != nullptr);
    CHECK(glVertexArrayAttribFormat != nullptr);
    CHECK(glVertexArrayAttribBinding != nullptr);
    CHECK(glVertexArrayVertexBuffer != nullptr);
    CHECK(glVertexArrayElementBuffer != nullptr);
    CHECK(glEnableVertexArrayAttrib != nullptr);
    CHECK(glDisableVertexArrayAttrib != nullptr);
    CHECK(glClipControl != nullptr);
    CHECK(glDeleteTextures != nullptr);
    // And it really is the fake that was loaded, not a system libGLESv2: the install binds the
    // scratch unit, which the driver sees.
    CHECK(fakegles::saw("glActiveTexture", {GL_TEXTURE0}));
}

TEST(gles_missing_library_is_reported) {
    setFakeLibrary("/nonexistent/libGLESv2.so");
    gl46::resetGlesFallbacks();
    gl46::installGlesFallbacks();
    CHECK(!gl46::glesReady());   // an explicit request that cannot be honoured fails, it does not
                                 // silently fall back to a driver that is not there
    setup();                     // and the next test starts from an installed layer
    CHECK(gl46::glesReady());
}

// ---------------------------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------------------------
TEST(gles_texture_edit_binds_its_object_and_restores_the_slot) {
    setup();
    GLuint edited = makeTexture(GL_TEXTURE_2D);
    GLuint atUnitZero = makeTexture(GL_TEXTURE_2D);
    GLuint atUnitThree = makeTexture(GL_TEXTURE_2D);
    glBindTextureUnit(0, atUnitZero);
    glBindTextureUnit(3, atUnitThree);   // leaves the active unit at 3, so the scratch unit has to
                                         // be made active again before the classic call
    fakegles::clearLog();

    glTextureStorage2D(edited, 1, GL_RGBA8, 64, 64);

    // The classic call went to the object asked for, on the target its creation recorded: the
    // width the driver stored is the proof, and it is not the one of what stood bound at unit 0.
    CHECK(fakegles::saw("glBindTexture", {GL_TEXTURE_2D, edited}));
    CHECK(fakegles::saw("glTexStorage2D", {GL_TEXTURE_2D, 1, GL_RGBA8, 64, 64}));
    CHECK_EQ(fakegles::storedWidth(edited), 64);
    CHECK_EQ(fakegles::storedWidth(atUnitZero), 0);
    // Unit 0 is back exactly where it was, and unit 3 was never touched.
    const fakegles::Call* restore = fakegles::last("glBindTexture");
    REQUIRE(restore != nullptr);
    CHECK_EQ(restore->a[1], static_cast<long long>(atUnitZero));
    CHECK_EQ(fakegles::boundTexture(0, GL_TEXTURE_2D), atUnitZero);
    CHECK_EQ(fakegles::boundTexture(3, GL_TEXTURE_2D), atUnitThree);
    CHECK_EQ(fakegles::activeUnit(), 0u);   // the scratch unit, where the emulation left it
}

TEST(gles_texture_target_comes_from_creation) {
    setup();
    GLuint array = makeTexture(GL_TEXTURE_2D_ARRAY);
    glTextureStorage3D(array, 4, GL_RGBA8, 64, 64, 32);
    CHECK(fakegles::saw("glTexStorage3D", {GL_TEXTURE_2D_ARRAY, 4, GL_RGBA8, 64, 64, 32}));
    glTextureSubImage3D(array, 0, 0, 0, 0, 4, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    CHECK(fakegles::saw("glTexSubImage3D", {GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, 4, 4, 4, GL_RGBA,
                                           GL_UNSIGNED_BYTE}));
    glTextureSubImage2D(array, 0, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    CHECK(fakegles::saw("glTexSubImage2D", {GL_TEXTURE_2D_ARRAY, 0, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE}));
    glGenerateTextureMipmap(array);
    CHECK(fakegles::saw("glGenerateMipmap", {GL_TEXTURE_2D_ARRAY}));

    GLuint cube = makeTexture(GL_TEXTURE_CUBE_MAP);
    fakegles::clearLog();
    glGenerateTextureMipmap(cube);
    CHECK(fakegles::saw("glGenerateMipmap", {GL_TEXTURE_CUBE_MAP}));

    GLuint three = makeTexture(GL_TEXTURE_3D);
    fakegles::clearLog();
    glTextureSubImage3D(three, 1, 0, 0, 0, 2, 2, 2, GL_RGBA, GL_FLOAT, nullptr);
    CHECK(fakegles::saw("glTexSubImage3D", {GL_TEXTURE_3D, 1, 0, 0, 0, 2, 2, 2, GL_RGBA, GL_FLOAT}));
}

TEST(gles_texture_level_query_reads_the_object_it_was_given) {
    setup();
    GLuint big = makeTexture(GL_TEXTURE_2D);
    glTextureStorage2D(big, 1, GL_RGBA8, 64, 64);
    GLuint small_ = makeTexture(GL_TEXTURE_2D);
    glTextureStorage2D(small_, 1, GL_RGBA8, 8, 8);
    glBindTextureUnit(0, small_);   // what stands where the emulation will bind
    fakegles::clearLog();

    GLint width = 0;
    glGetTextureLevelParameteriv(big, 0, GL_TEXTURE_WIDTH, &width);

    CHECK_EQ(width, 64);   // the storage of 'big': 8 would mean the emulation asked about the wrong one
    CHECK(fakegles::saw("glGetTexLevelParameteriv", {GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH}));
    CHECK_EQ(fakegles::boundTexture(0, GL_TEXTURE_2D), small_);
}

TEST(gles_texture_parameters_are_routed_or_dropped) {
    setup();
    GLuint array = makeTexture(GL_TEXTURE_2D_ARRAY);
    glTextureParameteri(array, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    CHECK(fakegles::saw("glTexParameteri", {GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE}));

    GLuint flat = makeTexture(GL_TEXTURE_2D);
    fakegles::clearLog();
    // Wrap R does not exist on a 2D texture in ES: passing it through would be a GL error, and the
    // layer drops it instead.
    glTextureParameteri(flat, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    CHECK_EQ(fakegles::logSize(), 0);

    // GL_TEXTURE_MAX_ANISOTROPY (0x84FE) is the same value as the EXT name ES drivers know, so it
    // is passed through as it is.
    glTextureParameterf(flat, 0x84FE, 4.0f);
    CHECK(fakegles::saw("glTexParameterf", {GL_TEXTURE_2D, 0x84FE}));
    CHECK_EQ(fakegles::lastTexParameterfValue(), 4.0f);
    CHECK_EQ(fakegles::boundTexture(0, GL_TEXTURE_2D), 0u);
}

TEST(gles_bind_texture_unit) {
    setup();
    GLuint flat = makeTexture(GL_TEXTURE_2D);
    GLuint array = makeTexture(GL_TEXTURE_2D_ARRAY);
    fakegles::clearLog();

    glBindTextureUnit(5, flat);
    CHECK(fakegles::saw("glBindTexture", {GL_TEXTURE_2D, flat}));
    CHECK_EQ(fakegles::boundTexture(5, GL_TEXTURE_2D), flat);

    // The same unit with another target: the two slots are independent, which is why the layer
    // keeps a texture's target instead of guessing one.
    glBindTextureUnit(5, array);
    CHECK(fakegles::saw("glBindTexture", {GL_TEXTURE_2D_ARRAY, array}));
    CHECK_EQ(fakegles::boundTexture(5, GL_TEXTURE_2D), flat);
    CHECK_EQ(fakegles::boundTexture(5, GL_TEXTURE_2D_ARRAY), array);

    // Unbinding has no target to name: the layer assumes 2D, which the game never relies on.
    glBindTextureUnit(5, 0);
    CHECK(fakegles::saw("glBindTexture", {GL_TEXTURE_2D, 0}));
    CHECK_EQ(fakegles::boundTexture(5, GL_TEXTURE_2D), 0u);

    // A unit past the end of the tracked table is dropped, not passed to a driver that would take
    // it and bind somewhere the layer is not looking.
    fakegles::clearLog();
    glBindTextureUnit(64, flat);
    CHECK_EQ(fakegles::logSize(), 0);
}

TEST(gles_texture_delete_forgets_the_recycled_name) {
    setup();
    GLuint first = makeTexture(GL_TEXTURE_2D);
    glTextureStorage2D(first, 1, GL_RGBA8, 16, 16);
    glBindTextureUnit(0, first);

    glDeleteTextures(1, &first);
    CHECK(fakegles::saw("glDeleteTextures", {1}));

    // The fake recycles the name, as a driver does: the next texture is the deleted one, with
    // another target. A layer that kept the old entry would bind it to GL_TEXTURE_2D_ARRAY.
    GLuint second = makeTexture(GL_TEXTURE_2D);
    CHECK_EQ(second, first);
    fakegles::clearLog();
    glTextureStorage2D(second, 1, GL_RGBA4, 4, 4);
    CHECK(fakegles::saw("glTexStorage2D", {GL_TEXTURE_2D, 1, GL_RGBA4, 4, 4}));

    // The per-unit slot forgot the name too: what the emulation puts back is nothing, not the
    // recycled name (which would put a live object back where the game had a deleted one).
    const fakegles::Call* restore = fakegles::last("glBindTexture");
    REQUIRE(restore != nullptr);
    CHECK_EQ(restore->a[1], 0);
    CHECK_EQ(fakegles::unknownNameCalls(), 0);
}

TEST(gles_create_textures_tracks_every_name) {
    setup();
    GLuint ids[3] = {};
    glCreateTextures(GL_TEXTURE_2D_ARRAY, 3, ids);
    CHECK(fakegles::saw("glGenTextures", {3}));
    for (GLuint id : ids) {
        REQUIRE(id != 0);
        fakegles::clearLog();
        glTextureStorage3D(id, 1, GL_RGBA8, 2, 2, 2);
        CHECK(fakegles::saw("glTexStorage3D", {GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, 2, 2, 2}));
    }
}

// ---------------------------------------------------------------------------------------------
// Buffers
// ---------------------------------------------------------------------------------------------
TEST(gles_buffer_edit_restores_every_binding_it_touches) {
    setup();
    GLuint buffer = makeBuffer();
    GLuint sentinel = makeBuffer();
    CHECK(fakegles::saw("glGenBuffers", {1}));
    driver().bindBuffer(GL_ARRAY_BUFFER, sentinel);
    driver().bindBuffer(GL_COPY_READ_BUFFER, sentinel);
    driver().bindBuffer(GL_COPY_WRITE_BUFFER, sentinel);
    fakegles::clearLog();

    glNamedBufferStorage(buffer, 256, nullptr, 0);
    CHECK(fakegles::saw("glBufferStorage", {GL_ARRAY_BUFFER, 256, 0}));
    CHECK_EQ(fakegles::boundBuffer(GL_ARRAY_BUFFER), sentinel);

    glNamedBufferSubData(buffer, 16, 32, nullptr);
    CHECK(fakegles::saw("glBufferSubData", {GL_ARRAY_BUFFER, 16, 32}));
    CHECK_EQ(fakegles::boundBuffer(GL_ARRAY_BUFFER), sentinel);

    GLuint other = makeBuffer();
    glCopyNamedBufferSubData(buffer, other, 0, 8, 16);
    // The two copy targets, and not GL_ARRAY_BUFFER: a copy through the wrong target pair is
    // silently a different operation.
    CHECK(fakegles::saw("glCopyBufferSubData", {GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 8, 16}));
    CHECK_EQ(fakegles::boundBuffer(GL_COPY_READ_BUFFER), sentinel);
    CHECK_EQ(fakegles::boundBuffer(GL_COPY_WRITE_BUFFER), sentinel);
    CHECK_EQ(fakegles::boundBuffer(GL_ARRAY_BUFFER), sentinel);
}

// ---------------------------------------------------------------------------------------------
// Framebuffers
// ---------------------------------------------------------------------------------------------
TEST(gles_framebuffer_attachments_route_by_texture_target) {
    setup();
    GLuint fb = makeFramebuffer();
    GLuint flat = makeTexture(GL_TEXTURE_2D);
    GLuint array = makeTexture(GL_TEXTURE_2D_ARRAY);
    GLuint cube = makeTexture(GL_TEXTURE_CUBE_MAP);
    GLuint drawSentinel = makeFramebuffer();
    GLuint readSentinel = makeFramebuffer();
    driver().bindFramebuffer(GL_DRAW_FRAMEBUFFER, drawSentinel);
    driver().bindFramebuffer(GL_READ_FRAMEBUFFER, readSentinel);
    fakegles::clearLog();

    glNamedFramebufferTexture(fb, GL_COLOR_ATTACHMENT0, flat, 0);
    CHECK(fakegles::saw("glBindFramebuffer", {GL_DRAW_FRAMEBUFFER, fb}));
    CHECK(fakegles::saw("glFramebufferTexture2D", {GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                                  flat, 0}));
    // Only the draw side was bound, and both sides are back where they were.
    CHECK(!fakegles::saw("glBindFramebuffer", {GL_READ_FRAMEBUFFER, fb}));
    CHECK_EQ(fakegles::boundFramebuffer(GL_DRAW_FRAMEBUFFER), drawSentinel);
    CHECK_EQ(fakegles::boundFramebuffer(GL_READ_FRAMEBUFFER), readSentinel);

    // An array texture and a cube map attach a layer: an unlayered ES attachment is layer 0 of it,
    // which is what a whole-texture attachment means to the game's non-layered targets.
    glNamedFramebufferTexture(fb, GL_COLOR_ATTACHMENT0, array, 1);
    CHECK(fakegles::saw("glFramebufferTextureLayer", {GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, array, 1, 0}));
    glNamedFramebufferTexture(fb, GL_COLOR_ATTACHMENT0, cube, 0);
    CHECK(fakegles::saw("glFramebufferTextureLayer", {GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, cube, 0, 0}));

    // Detaching goes through a 2D face, the only unlayered attachment ES has.
    glNamedFramebufferTexture(fb, GL_COLOR_ATTACHMENT0, 0, 0);
    CHECK(fakegles::saw("glFramebufferTexture2D", {GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0,
                                                  0}));

    // The explicit layer form passes both the level and the layer through.
    glNamedFramebufferTextureLayer(fb, GL_DEPTH_ATTACHMENT, array, 2, 3);
    CHECK(fakegles::saw("glFramebufferTextureLayer", {GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, array, 2, 3}));
    CHECK_EQ(fakegles::boundFramebuffer(GL_DRAW_FRAMEBUFFER), drawSentinel);
    CHECK_EQ(fakegles::boundFramebuffer(GL_READ_FRAMEBUFFER), readSentinel);
}

TEST(gles_framebuffer_renderbuffer_draw_buffers_and_status) {
    setup();
    GLuint fb = makeFramebuffer();
    GLuint rb = 0;
    glCreateRenderbuffers(1, &rb);
    CHECK(fakegles::saw("glGenRenderbuffers", {1}));
    GLuint rbSentinel = 0;
    glCreateRenderbuffers(1, &rbSentinel);
    driver().bindRenderbuffer(GL_RENDERBUFFER, rbSentinel);
    fakegles::clearLog();

    glNamedRenderbufferStorage(rb, GL_DEPTH_COMPONENT24, 128, 128);
    CHECK(fakegles::saw("glRenderbufferStorage", {GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, 128, 128}));
    CHECK_EQ(fakegles::boundRenderbuffer(), rbSentinel);

    glNamedFramebufferRenderbuffer(fb, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb);
    CHECK(fakegles::saw("glFramebufferRenderbuffer", {GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                                                     rb}));
    glNamedFramebufferDrawBuffer(fb, GL_COLOR_ATTACHMENT1);
    CHECK(fakegles::saw("glDrawBuffers", {1, GL_COLOR_ATTACHMENT1}));
    const GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glNamedFramebufferDrawBuffers(fb, 2, bufs);
    CHECK(fakegles::saw("glDrawBuffers", {2, GL_COLOR_ATTACHMENT0}));

    const GLfloat clearColor[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    glClearNamedFramebufferfv(fb, GL_COLOR, 0, clearColor);
    CHECK(fakegles::saw("glClearBufferfv", {GL_COLOR, 0}));
    CHECK_EQ(fakegles::lastClearValue(), 0.25f);

    // The status is asked of the framebuffer's own binding, and the answer comes back.
    const GLenum status = glCheckNamedFramebufferStatus(fb, GL_FRAMEBUFFER);
    CHECK_EQ(status, GLuint(GL_FRAMEBUFFER_COMPLETE));
    CHECK_EQ(fakegles::checkedFbo(), fb);
    CHECK_EQ(fakegles::boundFramebuffer(GL_DRAW_FRAMEBUFFER), 0u);
}

// ---------------------------------------------------------------------------------------------
// Vertex arrays
// ---------------------------------------------------------------------------------------------
TEST(gles_vertex_array_edit_restores_the_binding) {
    setup();
    GLuint vao = 0;
    glCreateVertexArrays(1, &vao);
    CHECK(fakegles::saw("glGenVertexArrays", {1}));
    GLuint sentinel = 0;
    glCreateVertexArrays(1, &sentinel);
    GLuint vbo = makeBuffer();
    GLuint ebo = makeBuffer();
    driver().bindVertexArray(sentinel);
    fakegles::clearLog();

    glVertexArrayAttribFormat(vao, 3, 4, GL_FLOAT, GL_FALSE, 16);
    CHECK(fakegles::saw("glBindVertexArray", {vao}));
    CHECK(fakegles::saw("glVertexAttribFormat", {3, 4, GL_FLOAT, GL_FALSE, 16}));
    CHECK_EQ(fakegles::boundVertexArray(), sentinel);

    glVertexArrayAttribBinding(vao, 3, 2);
    CHECK(fakegles::saw("glVertexAttribBinding", {3, 2}));
    glVertexArrayVertexBuffer(vao, 2, vbo, 64, 32);
    CHECK(fakegles::saw("glBindVertexBuffer", {2, vbo, 64, 32}));
    glEnableVertexArrayAttrib(vao, 3);
    CHECK(fakegles::saw("glEnableVertexAttribArray", {3}));
    glDisableVertexArrayAttrib(vao, 3);
    CHECK(fakegles::saw("glDisableVertexAttribArray", {3}));
    CHECK_EQ(fakegles::boundVertexArray(), sentinel);

    // The element buffer belongs to the array, so it stays with it when the array is unbound --
    // which is why this one needs no restoring, only the array still bound while it is set.
    glVertexArrayElementBuffer(vao, ebo);
    CHECK(fakegles::saw("glBindBuffer", {GL_ELEMENT_ARRAY_BUFFER, ebo}));
    CHECK_EQ(fakegles::elementBuffer(vao), ebo);
    CHECK_EQ(fakegles::elementBuffer(sentinel), 0u);
    CHECK_EQ(fakegles::boundVertexArray(), sentinel);
}

// ---------------------------------------------------------------------------------------------
// Samplers, and what happens to a name the layer never created
// ---------------------------------------------------------------------------------------------
TEST(gles_samplers_and_unknown_texture_names) {
    setup();
    GLuint sampler = 0;
    glCreateSamplers(1, &sampler);
    CHECK(fakegles::saw("glGenSamplers", {1}));
    CHECK_EQ(sampler, 1u);   // the name came from the driver, not from the layer

    // A texture the layer did not create has no recorded target, so it is assumed to be 2D and
    // handed to the driver as it is: the layer never invents a name, and never drops one.
    fakegles::clearLog();
    glTextureStorage2D(999, 1, GL_RGBA8, 4, 4);
    CHECK(fakegles::saw("glBindTexture", {GL_TEXTURE_2D, 999}));
    CHECK(fakegles::saw("glTexStorage2D", {GL_TEXTURE_2D, 1, GL_RGBA8, 4, 4}));
    CHECK_EQ(fakegles::unknownNameCalls(), 1);
}

// ---------------------------------------------------------------------------------------------
// The two calls that are deliberately nothing
// ---------------------------------------------------------------------------------------------
TEST(gles_invalidate_and_clip_control_do_nothing) {
    setup();
    GLuint buffer = makeBuffer();
    const int before = fakegles::logSize();

    glInvalidateBufferData(buffer);            // a driver hint on desktop; nothing the game reads back
    glInvalidateBufferSubData(buffer, 0, 16);
    // The fake driver lists no GL_EXT_clip_control: the layer leaves the [0,1] depth range to the
    // vertex shaders (gl46::glesRemapDepth) and makes no driver call.
    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);

    CHECK_EQ(fakegles::logSize(), before);
    CHECK(gl46::glesRemapDepth());
}

// ---------------------------------------------------------------------------------------------
// The install: it fills only what the driver did not resolve
// ---------------------------------------------------------------------------------------------
namespace {
// Stand-ins for a driver that resolves an entry point itself. installGlesFallbacks fills a pointer
// only when it is null, so these must survive it.
void APIENTRY driverOwnCreateTextures(GLenum, GLsizei, GLuint*) {}
void APIENTRY driverOwnClipControl(GLenum, GLenum) {}
}  // namespace

TEST(gles_install_always_installs_the_33_dsa_emulators) {
    setup();
    // A driver resolves every name eglGetProcAddress asks it for (a non-null stub for the ones it
    // does not provide is allowed and is what Qualcomm's does), so "keep the driver's own" is not
    // a test the layer can make: glClipControl and the desktop-only DSA calls would stay those
    // non-executable stubs. The install assigns the emulators unconditionally, and the game's
    // own entry points the driver does provide (glCreateTextures over glCreateTextures... no:
    // ES 3.2 has none of the 33) keep working underneath them.
    gl46::installGlesFallbacks();
    CHECK(glCreateTextures != &driverOwnCreateTextures);   // the emulation, not the stand-in
    CHECK(glClipControl != &driverOwnClipControl);
    // ... and the layer still works through them.
    GLuint texture = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &texture);
    CHECK(texture != 0);
    CHECK_EQ(fakegles::boundTexture(0, GL_TEXTURE_2D), 0u);
    CHECK(gl46::glesReady());
    glCreateTextures = nullptr;
    glClipControl = nullptr;
    gl46::installGlesFallbacks();   // put the layer back for the tests that follow
    CHECK(glCreateTextures != nullptr);
}

// ---------------------------------------------------------------------------------------------
// glBindTextureUnit: the active unit it has to move, and the table it keeps
// ---------------------------------------------------------------------------------------------
TEST(gles_bind_texture_unit_reuses_the_active_unit) {
    setup();
    GLuint a = makeTexture(GL_TEXTURE_2D);
    GLuint b = makeTexture(GL_TEXTURE_2D);
    GLuint c = makeTexture(GL_TEXTURE_2D);
    fakegles::clearLog();

    // glBindTextureUnit has to make its unit active to bind the classic way, and the layer caches
    // the active unit: a run of binds to the same unit costs one glActiveTexture, not one each.
    glBindTextureUnit(3, a);
    glBindTextureUnit(3, b);
    glBindTextureUnit(3, c);
    CHECK_EQ(fakegles::countOf("glActiveTexture"), 1);
    CHECK(fakegles::saw("glActiveTexture", {GL_TEXTURE0 + 3}));
    CHECK_EQ(fakegles::activeUnit(), 3u);
    CHECK_EQ(fakegles::boundTexture(3, GL_TEXTURE_2D), c);   // the last bind won on that unit

    // Another unit costs one more, and coming back to 3 costs another: the cache remembers only the
    // current unit, so a unit left earlier is made active again.
    glBindTextureUnit(7, a);
    CHECK_EQ(fakegles::countOf("glActiveTexture"), 2);
    glBindTextureUnit(3, a);
    CHECK_EQ(fakegles::countOf("glActiveTexture"), 3);
    CHECK(fakegles::saw("glActiveTexture", {GL_TEXTURE0 + 3}));
    CHECK(fakegles::saw("glActiveTexture", {GL_TEXTURE0 + 7}));
    CHECK_EQ(fakegles::boundTexture(3, GL_TEXTURE_2D), a);
    CHECK_EQ(fakegles::boundTexture(7, GL_TEXTURE_2D), a);

    // A DSA edit hides a classic bind to the scratch unit (0): it makes unit 0 active first and
    // leaves it there, so the next glBindTextureUnit to 0 needs no glActiveTexture either.
    GLuint edited = makeTexture(GL_TEXTURE_2D);
    const int before = fakegles::countOf("glActiveTexture");
    glTextureStorage2D(edited, 1, GL_RGBA8, 8, 8);
    CHECK_EQ(fakegles::countOf("glActiveTexture"), before + 1);   // unit 0 made active
    CHECK_EQ(fakegles::activeUnit(), 0u);
    glBindTextureUnit(0, a);
    CHECK_EQ(fakegles::countOf("glActiveTexture"), before + 1);   // already on 0: no extra call
    CHECK_EQ(fakegles::boundTexture(0, GL_TEXTURE_2D), a);
}

TEST(gles_texture_slots_keep_every_target_isolated) {
    setup();
    // The layer remembers the texture bound to each (unit, target) so it can put that slot back
    // after editing an object by binding it. The game's targets are a handful, but nothing caps
    // them: this fills one unit with far more distinct targets than the old fixed table held (24),
    // and checks that each one still saves and restores its *own* binding. A table that reused a
    // slot past its size would silently put the wrong texture back here.
    constexpr int kTargets = 40;
    GLuint tex[kTargets] = {};
    auto target = [](int i) { return GLenum(0x9000 + i); };   // distinct, opaque to the layer
    for (int i = 0; i < kTargets; ++i) tex[i] = makeTexture(target(i));
    for (int i = 0; i < kTargets; ++i) glBindTextureUnit(0, tex[i]);

    // Every bind reached the driver -- the layer never drops one -- and each target holds its own
    // texture, not a neighbour's.
    for (int i = 0; i < kTargets; ++i) CHECK_EQ(fakegles::boundTexture(0, target(i)), tex[i]);

    // Editing any of them, however deep in the table, binds the object asked for and puts its own
    // slot back: the first, one in the middle, and the last all restore what they held, not
    // whatever another target's slot happened to contain.
    for (int i : {0, kTargets / 2, kTargets - 1}) {
        GLuint other = makeTexture(target(i));   // a different texture object on the same target
        glTextureStorage2D(other, 1, GL_RGBA8, 4, 4);
        CHECK(fakegles::saw("glTexStorage2D", {target(i), 1, GL_RGBA8, 4, 4}));
        CHECK_EQ(fakegles::boundTexture(0, target(i)), tex[i]);
    }

    // A second unit has its own slots: filling unit 0 changed nothing there, and a target it never
    // bound stays empty (0) rather than inheriting unit 0's.
    glBindTextureUnit(1, tex[0]);
    CHECK_EQ(fakegles::boundTexture(1, target(0)), tex[0]);
    CHECK_EQ(fakegles::boundTexture(1, target(1)), 0u);
    CHECK_EQ(fakegles::boundTexture(1, target(kTargets - 1)), 0u);

    // A deep target on unit 1 disturbs neither unit 0's deep slots nor its shallow ones.
    glBindTextureUnit(1, tex[kTargets - 1]);
    CHECK_EQ(fakegles::boundTexture(1, target(kTargets - 1)), tex[kTargets - 1]);
    CHECK_EQ(fakegles::boundTexture(0, target(kTargets - 1)), tex[kTargets - 1]);
    CHECK_EQ(fakegles::boundTexture(0, target(0)), tex[0]);
}

#else

// Windows: the harness needs a fake libGLESv2 to dlopen, which only the non-Windows build makes
// (see CMakeLists.txt); the layer itself is never compiled there.
TEST(gles_shim_harness_needs_a_non_windows_host) {
    SKIP("the GL ES compatibility layer harness is built on non-Windows hosts only");
}

#endif  // SCACELITH_GLES_TESTS
