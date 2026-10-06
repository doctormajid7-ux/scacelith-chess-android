#include "gl_context.h"
#include "gl46.h"
#include "../core/log.h"
#include <string>

namespace gl46 {
static void APIENTRY debugCallback(GLenum, GLenum type, GLuint id, GLenum severity, GLsizei, const GLchar* message, const void*) {
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
    if (id == 131185 || id == 131218 || id == 131204) return;  // NVIDIA buffer/shader recompile chatter
    if (type == GL_DEBUG_TYPE_ERROR) LOGE("GL: %s", message);
    else LOGW("GL: %s", message);
}

void afterContextCreated(bool debug) {
    LOGI("GL %s | %s | %s", (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER),
         (const char*)glGetString(GL_VENDOR));
    // The driver's interface limits, once: a program that fails to link with "location or
    // component exceeds max allowed" is counted against these, and the ES floor is lower than
    // desktop's. Names as numbers: the enum values are the same registry entries everywhere.
    static const struct { unsigned id; const char* name; } kLimits[] = {
        {0x0C82, "drawBuffers"}, {0x88FC, "dualSourceDrawBuffers"}, {0x8872, "texUnits"},
        {0x8B4D, "combinedTexUnits"}, {0x8B4C, "vertexTexUnits"}, {0x9122, "vertexOutComponents"},
        {0x9125, "fragmentInComponents"}, {0x8B4B, "varyingComponents"}, {0x8DFC, "varyingVectors"},
        {0x8B4A, "vertexUniformComponents"}, {0x8B49, "fragmentUniformComponents"},
        {0x8DFB, "vertexUniformVectors"}, {0x8DFD, "fragmentUniformVectors"}};
    std::string lim;
    for (const auto& l : kLimits) {
        GLint v = 0;
        glGetIntegerv(l.id, &v);
        lim += std::string(l.name) + "=" + std::to_string(int(v)) + " ";
    }
    LOGI("GL limits: %s", lim.c_str());
    if (debug) {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(debugCallback, nullptr);
    }
    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
}
}  // namespace gl46
