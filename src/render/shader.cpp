#include "shader.h"
#include "../core/embedded.h"
#include "../core/log.h"
#ifdef SCACELITH_GLES
#include "../gl/gl46_gles.h"
#include <regex>
#endif
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <functional>

std::string ProgramDesc::key() const {
    std::string k = vs + "|" + tcs + "|" + tes + "|" + gs + "|" + fs + "|" + cs + "|" + material + "|" + displacement;
    for (auto& d : defines) k += "|" + d;
    return k;
}

namespace shaders {
namespace {
std::map<std::string, std::pair<ProgramDesc, std::unique_ptr<ShaderProgram>>> g_cache;

int fileIndex(std::vector<std::string>& table, const std::string& path) {
    for (size_t i = 0; i < table.size(); ++i)
        if (table[i] == path) return int(i);
    table.push_back(path);
    return int(table.size() - 1);
}

void expand(const std::string& path, const std::string& material, const std::string& displacement,
            std::vector<std::string>& table, std::set<std::string>& included, std::string& out, bool isRoot) {
    // Include-once is implemented with preprocessor guards (not by skipping text) so includes
    // inside #ifdef blocks behave like C: the guard only takes effect in active branches.
    if (included.count(path)) {  // cycle through the current include stack
        LOGW("recursive #include of %s ignored", path.c_str());
        return;
    }
    included.insert(path);
    std::string src = embedded::text(path.c_str());
    int idx = fileIndex(table, path);
    std::istringstream in(src);
    std::string line;
    int lineNo = 0;
    std::string guard = "SCACELITH_INC_" + std::to_string(std::hash<std::string>()(path) & 0xFFFFFFFFu);
    if (!isRoot) out += "#ifndef " + guard + "\n#define " + guard + "\n#line 1 " + std::to_string(idx) + "\n";
    while (std::getline(in, line)) {
        ++lineNo;
        size_t p = line.find_first_not_of(" \t");
        if (p != std::string::npos && line.compare(p, 8, "#include") == 0) {
            size_t a = line.find('"', p), b = line.find('"', a + 1);
            if (a != std::string::npos && b != std::string::npos) {
                expand(line.substr(a + 1, b - a - 1), material, displacement, table, included, out, false);
                out += "#line " + std::to_string(lineNo + 1) + " " + std::to_string(idx) + "\n";
                continue;
            }
        }
        if (p != std::string::npos && line.compare(p, 16, "#pragma material") == 0) {
            out += "\n";
            if (!material.empty()) {
                expand(material, material, displacement, table, included, out, false);
                out += "#line " + std::to_string(lineNo + 1) + " " + std::to_string(idx) + "\n";
            }
            continue;
        }
        if (p != std::string::npos && line.compare(p, 20, "#pragma displacement") == 0) {
            out += "\n";
            if (!displacement.empty()) {
                out += "#define MATERIAL_HAS_DISPLACEMENT 1\n";
                expand(displacement, material, displacement, table, included, out, false);
                out += "#line " + std::to_string(lineNo + 1) + " " + std::to_string(idx) + "\n";
            }
            continue;
        }
        if (p != std::string::npos && line.compare(p, 8, "#version") == 0) { out += "\n"; continue; }  // re-emitted by preamble()
        out += line;
        out += '\n';
    }
    if (!isRoot) out += "#endif\n";
    included.erase(path);
}

// A program that fails to link leaves the driver's count of its interface to guesswork: dump the
// preprocessed stages next to the log (the file channel is the only one that survives on the
// device), so the locations, components and bindings can be counted against the driver's limits.
void dumpLinkFailure(const ProgramDesc& d) {
    std::string dir;
    if (const char* e = std::getenv("SCACELITH_DUMP_SHADERS")) dir = e;
    else if (const char* e = std::getenv("SCACELITH_FILES_DIR")) dir = e;
    if (dir.empty()) return;
    const struct St { const std::string* path; const char* ext; } stages[] = {
        {&d.vs, "vert"}, {&d.tcs, "tesc"}, {&d.tes, "tese"}, {&d.gs, "geom"}, {&d.fs, "frag"}, {&d.cs, "comp"}};
    const size_t h = std::hash<std::string>()(d.key());
    for (const St& st : stages) {
        if (st.path->empty()) continue;
        std::vector<std::string> table;
        std::string src = preprocess(*st.path, d.material, d.displacement, d.defines, &table);
        std::string name = dir + "/linkfail_" + std::to_string(h) + "_" + st.ext + ".glsl";
        if (FILE* f = std::fopen(name.c_str(), "w")) { std::fputs(src.c_str(), f); std::fclose(f); }
    }
}

GLuint compileStage(GLenum type, const std::string& path, const ProgramDesc& d) {
    std::vector<std::string> table;
    std::string src = preprocess(path, d.material, d.displacement, d.defines, &table);
    if (const char* dump = std::getenv("SCACELITH_DUMP_SHADERS")) {
        std::string name = path;
        for (char& c : name) if (c == '/') c = '_';
        if (FILE* f = std::fopen((std::string(dump) + "/" + name + "." + std::to_string(std::hash<std::string>()(d.key())) + ".glsl").c_str(), "w")) {
            std::fputs(src.c_str(), f);
            std::fclose(f);
        }
    }
    GLuint sh = glCreateShader(type);
    const char* s = src.c_str();
    glShaderSource(sh, 1, &s, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
        std::string log(size_t(len > 1 ? len : 1), '\0');
        glGetShaderInfoLog(sh, len, nullptr, &log[0]);
        std::string files;
        for (size_t i = 0; i < table.size(); ++i) files += "  " + std::to_string(i) + " = " + table[i] + "\n";
        LOGE("shader compile failed: %s (material '%s')\n%s\nsource index table:\n%s", path.c_str(), d.material.c_str(),
             log.c_str(), files.c_str());
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

GLuint link(const ProgramDesc& d) {
    struct St { GLenum type; const std::string* path; } stages[] = {
        {GL_VERTEX_SHADER, &d.vs}, {GL_TESS_CONTROL_SHADER, &d.tcs}, {GL_TESS_EVALUATION_SHADER, &d.tes},
        {GL_GEOMETRY_SHADER, &d.gs}, {GL_FRAGMENT_SHADER, &d.fs}, {GL_COMPUTE_SHADER, &d.cs}};
    GLuint prog = glCreateProgram();
    std::vector<GLuint> shs;
    bool ok = true;
    for (auto& st : stages) {
        if (st.path->empty()) continue;
        GLuint sh = compileStage(st.type, *st.path, d);
        if (!sh) { ok = false; break; }
        glAttachShader(prog, sh);
        shs.push_back(sh);
    }
    if (ok) {
        glLinkProgram(prog);
        GLint linked = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &linked);
        if (!linked) {
            GLint len = 0;
            glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
            std::string log(size_t(len > 1 ? len : 1), '\0');
            glGetProgramInfoLog(prog, len, nullptr, &log[0]);
            LOGE("program link failed (%s):\n%s", d.key().c_str(), log.c_str());
            dumpLinkFailure(d);
            ok = false;
        }
    }
    for (GLuint sh : shs) { glDetachShader(prog, sh); glDeleteShader(sh); }
    if (!ok) { glDeleteProgram(prog); return 0; }
    return prog;
}
}  // namespace

// The #version line and the shader language setup. Desktop builds emit the GLSL 4.60 core the
// shaders are written in; the Android build (SCACELITH_GLES, src/gl/gl46_gles.cpp) emits GLSL ES
// 3.20, the highest a GL ES 3.2 context compiles:
//   * SCACELITH_GLES is defined for the shaders, so the few constructs ES lacks (the gl_PerVertex
//     block and gl_ClipDistance of shaders/passes/mesh_common.glsl) pick a portable path.
//   * ES has no default precision for float / int in any stage, and defaults sampler types to
//     lowp, which would quantise HDR colours and texture coordinates: everything the game samples
//     or interpolates is declared highp here.
std::string preamble(bool fragment, bool dualSource) {
#ifdef SCACELITH_GLES
    std::string out = "#version 320 es\n#define SCACELITH_GLES 1\n";
    // The transparent path of forward.frag writes dual-source blending outputs (src0 + src1 * dst):
    // a fragment-only extension GLSL ES makes opt-in, and the compiler's error for the index
    // layout qualifier names it. A #extension must precede every declaration (the precision
    // lines below included), so it goes here; a driver without the extension warns and fails
    // those materials only.
    //
    // Only the program that actually declares dual-source outputs may enable it: on Adreno the
    // extension flips the output-location check to gl_MaxDualSourceDrawBuffers (= 1 there), so a
    // shader with MRT outputs then fails to link with "Output <name> location or component
    // exceeds max allowed" on every location >= 1. Enabling it unconditionally is what left the
    // board and pieces unlinked (no 3D at all) on the device.
    if (fragment && dualSource) out += "#extension GL_EXT_blend_func_extended : enable\n";
    out += "precision highp float;\nprecision highp int;\n";
    out += "precision highp sampler2D;\nprecision highp sampler3D;\nprecision highp samplerCube;\n";
    out += "precision highp sampler2DArray;\nprecision highp samplerCubeArray;\n";
    out += "precision highp sampler2DShadow;\nprecision highp samplerCubeShadow;\n";
    out += "precision highp sampler2DArrayShadow;\nprecision highp sampler2DMS;\n";
    out += "precision highp isampler2D;\nprecision highp usampler2D;\n";
    out += "precision highp isampler2DArray;\nprecision highp usampler2DArray;\n";
    // Image types have no default precision at all in GLSL ES (not even lowp): without these
    // lines every compute shader that declares an image fails to compile -- the BRDF LUT, the
    // atmosphere LUTs, the probe prefilter, the material bakes and most of the post chain --
    // which leaves the lighting black and the scene lit by its emissive surfaces only.
    out += "precision highp image2D;\nprecision highp image2DArray;\nprecision highp image3D;\n";
    out += "precision highp imageCube;\nprecision highp imageCubeArray;\n";
    out += "precision highp iimage2D;\nprecision highp uimage2D;\n";
    return out;
#else
    (void)fragment;
    (void)dualSource;
    return "#version 460 core\n";
#endif
}

std::string preprocess(const std::string& path, const std::string& material, const std::string& displacement,
                       const std::vector<std::string>& defines, std::vector<std::string>* fileTable) {
    std::vector<std::string> localTable;
    std::vector<std::string>& table = fileTable ? *fileTable : localTable;
    const bool fragment = path.size() >= 5 && path.compare(path.size() - 5, 5, ".frag") == 0;
    // Dual-source outputs (layout(location = .., index = ..)) live only in the transparent path
    // of forward.frag; every other program must keep the extension off (see preamble).
    bool dualSource = false;
    if (fragment && path.size() >= 11 && path.compare(path.size() - 11, 11, "forward.frag") == 0)
        for (const auto& d : defines) dualSource = dualSource || d == "MATERIAL_TRANSPARENT";
    std::string out = preamble(fragment, dualSource);
    // Diagnostics: SCACELITH_SHADER_DEFINES="A B=1" adds defines to every stage of every program
    // (on Android through debug.scacelith.env, see platform_android.cpp), to switch shader paths
    // off one at a time and measure what each costs on a device.
    static const std::vector<std::string> extra = [] {
        std::vector<std::string> v;
        if (const char* e = std::getenv("SCACELITH_SHADER_DEFINES")) {
            std::istringstream ss(e);
            for (std::string w; ss >> w;) v.push_back(w);
        }
        return v;
    }();
    for (const auto& d : extra) {
        std::string def = d;
        size_t eq = def.find('=');
        if (eq != std::string::npos) def[eq] = ' ';
        out += "#define " + def + "\n";
    }
    for (auto& d : defines) {
        std::string def = d;
        size_t eq = def.find('=');
        if (eq != std::string::npos) def[eq] = ' ';
        out += "#define " + def + "\n";
    }
    out += "#line 1 0\n";
    std::set<std::string> included;
    expand(path, material, displacement, table, included, out, true);
#ifdef SCACELITH_GLES
    // A driver without GL_EXT_clip_control keeps ES's [-1, 1] clip-space depth (gl46_gles.cpp,
    // emuClipControl): every vertex stage then maps the game's [0, 1] reverse-Z to it, after the
    // shader's own main() has run. (The tessellated vertex stage writes no gl_Position; ES never
    // builds it anyway.)
    const bool vertex = path.size() >= 5 && path.compare(path.size() - 5, 5, ".vert") == 0;
    bool tessellated = false;
    for (const auto& d : defines) tessellated = tessellated || d == "MESH_TESSELLATED";
    if (vertex && !tessellated && gl46::glesRemapDepth()) {
        static const std::regex kMain(R"(\bvoid\s+main\s*\(\s*(void)?\s*\))");
        out = std::regex_replace(out, kMain, "void scacelith_vertex_main()");
        out += "\nvoid main() {\n    scacelith_vertex_main();\n"
               "    gl_Position.z = 2.0 * gl_Position.z - gl_Position.w;\n}\n";
    }
#endif
    return out;
}

const ShaderProgram& get(const ProgramDesc& d) {
    std::string k = d.key();
    auto it = g_cache.find(k);
    if (it != g_cache.end()) return *it->second.second;
    auto p = std::make_unique<ShaderProgram>();
    p->id = link(d);
    auto& slot = g_cache[k];
    slot.first = d;
    slot.second = std::move(p);
    return *slot.second;
}

const ShaderProgram& compute(const std::string& cs, const std::vector<std::string>& defines) {
    ProgramDesc d;
    d.cs = cs;
    d.defines = defines;
    return get(d);
}

const ShaderProgram& fullscreen(const std::string& fs, const std::vector<std::string>& defines) {
    ProgramDesc d;
    d.vs = "shaders/passes/fullscreen.vert";
    d.fs = fs;
    d.defines = defines;
    return get(d);
}

void reloadAll() {
    int ok = 0, failed = 0;
    for (auto& kv : g_cache) {
        GLuint np = link(kv.second.first);
        if (np) {
            if (kv.second.second->id) glDeleteProgram(kv.second.second->id);
            kv.second.second->id = np;
            ++ok;
        } else {
            ++failed;
        }
    }
    LOGI("shader reload: %d ok, %d failed", ok, failed);
}

void shutdown() {
    for (auto& kv : g_cache)
        if (kv.second.second->id) glDeleteProgram(kv.second.second->id);
    g_cache.clear();
}
}  // namespace shaders
