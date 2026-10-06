#include "renderer.h"
#include "../core/log.h"
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include "lighting/atmosphere.h"
#include "lighting/lighting_data.h"
#include "lighting/planar.h"
#include "lighting/probes.h"
#include "lighting/shadows.h"
#include "post/postfx.h"
#include "shader.h"
#include "../game/layout.h"
#include <algorithm>
#include <cstring>

using namespace m;

namespace render {

static Renderer* g_renderer = nullptr;
Renderer& renderer() { return *g_renderer; }
void setRenderer(Renderer* r) { g_renderer = r; }

// ---------------------------------------------------------------------------------------------
mat4 Camera::view() const {
    mat3 r = toMat3(conjugate(orientation));
    return mat4(r, -(r * position));
}
mat4 Camera::proj(float aspect) const { return perspectiveReverseZ(fovY, aspect, nearZ); }

Ray Camera::screenRay(float px, float py, int w, int h) const {
    float aspect = float(w) / float(h);
    float t = std::tan(fovY * 0.5f);
    float x = (2.0f * (px + 0.5f) / float(w) - 1.0f) * t * aspect;
    float y = (1.0f - 2.0f * (py + 0.5f) / float(h)) * t;
    vec3 d = normalize(forward() + right() * x + up() * y);
    return {position, d};
}

void Camera::lookAt(vec3 target, vec3 upHint) {
    // Our camera looks down local -Z, lookRotation maps local +Z: look away from the target.
    orientation = lookRotation(position - target, upHint);
}

void RenderSettings::applyPreset(Quality q) {
    quality = q;
    switch (q) {
        case Quality::Low:
            shadowMapSize = 2048; planarReflections = false; ssao = true; ssr = false; volumetrics = false;
            taa = true; motionBlur = false; dof = false; bloom = true; tessellation = false;
            shadowCascades = 2; probeResolution = 64; probeBounces = 1;
#ifdef SCACELITH_GLES
            // Mobile Low: the effects a phone screen barely shows go. No ambient occlusion, no
            // light probes (the hemisphere ambient replaces them, and the multi-second bake at
            // start goes too), one 1024^2 shadow cascade pair with hard-ish PCF (lighting.glsl).
            ssao = false; lightProbes = false; shadowMapSize = 1024; specularAA = 0.0f;
#endif
            break;
        case Quality::Medium:
            shadowMapSize = 2048; planarReflections = true; ssao = true; ssr = false; volumetrics = true;
            taa = true; motionBlur = true; dof = false; bloom = true; tessellation = false;
            shadowCascades = 3; probeResolution = 64; probeBounces = 2; break;
        case Quality::High:
            shadowMapSize = 4096; planarReflections = true; ssao = true; ssr = true; volumetrics = true;
            taa = true; motionBlur = true; dof = true; bloom = true; tessellation = true;
            shadowCascades = 3; probeResolution = 128; probeBounces = 2; break;
        case Quality::Ultra:
            shadowMapSize = 4096; planarReflections = true; ssao = true; ssr = true; volumetrics = true;
            taa = true; motionBlur = true; dof = true; bloom = true; tessellation = true; renderScale = 1.0f;
            shadowCascades = 3; probeResolution = 128; probeBounces = 3; break;
    }
}

// ---------------------------------------------------------------------------------------------
Renderer::Renderer()
    : lub_(new LightingUBOData()),
      atmosphere_(new lighting::Atmosphere),
      shadows_(new lighting::SunShadows),
      probes_(new lighting::LightProbes),
      planarRefl_(new lighting::PlanarReflections),
      post_(new PostFX) {}
Renderer::~Renderer() { shutdown(); }

static GLuint g_samplerShadowCmp = 0, g_samplerShadowRaw = 0;

bool Renderer::init(const RenderSettings& s) {
    settings_ = s;
    gpu::ensureBuffer(frameUbo_, sizeof(FrameUBOData));
    gpu::ensureBuffer(drawSsbo_, sizeof(DrawDataGPU) * 256);
    gpu::ensureBuffer(lightSsbo_, sizeof(PointLight) * 16);
    gpu::ensureBuffer(lightingUbo_, sizeof(LightingUBOData));
    *lub_ = LightingUBOData();
    glNamedBufferSubData(lightingUbo_.id, 0, sizeof(LightingUBOData), lub_.get());

    auto fill = [](gpu::Texture& t, const void* px, GLenum fmt, GLenum type) {
        if (t.target == GL_TEXTURE_2D) glTextureSubImage2D(t.id, 0, 0, 0, 1, 1, fmt, type, px);
        else glTextureSubImage3D(t.id, 0, 0, 0, 0, 1, 1, t.depth, fmt, type, px);
    };
    const float black[4 * 6] = {};
    dummy2D_ = gpu::createTexture2D(1, 1, GL_RGBA16F);
    fill(dummy2D_, black, GL_RGBA, GL_FLOAT);
    dummyArray_ = gpu::createTexture2DArray(1, 1, 1, GL_RGBA16F);
    fill(dummyArray_, black, GL_RGBA, GL_FLOAT);
    dummyCubeArray_ = gpu::createCubemapArray(1, 1, GL_RGBA16F);
    fill(dummyCubeArray_, black, GL_RGBA, GL_FLOAT);

    glCreateSamplers(1, &g_samplerShadowCmp);
    glSamplerParameteri(g_samplerShadowCmp, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(g_samplerShadowCmp, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(g_samplerShadowCmp, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(g_samplerShadowCmp, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(g_samplerShadowCmp, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glSamplerParameteri(g_samplerShadowCmp, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glCreateSamplers(1, &g_samplerShadowRaw);
    glSamplerParameteri(g_samplerShadowRaw, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glSamplerParameteri(g_samplerShadowRaw, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glSamplerParameteri(g_samplerShadowRaw, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(g_samplerShadowRaw, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Split-sum DFG LUT (+ sheen albedo), computed once.
    brdfLut_ = gpu::createTexture2D(128, 128, GL_RGBA16F);
    glObjectLabel(GL_TEXTURE, brdfLut_.id, -1, "brdf.dfg");
    {
        const ShaderProgram& p = shaders::compute("shaders/lighting/dfg_lut.comp");
        if (p.valid()) {
            p.use();
            p.set("uSize", 128);
            glBindImageTexture(0, brdfLut_.id, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
            gpu::dispatch2D(128, 128);
            glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
        }
    }

    atmosphere_->init();
    shadows_->init(settings_.shadowMapSize, settings_.shadowCascades, settings_.staticShadowCache);
    probes_->init(settings_.probeResolution);
    defaultLightingLayout();

    if (!post_->init()) return false;
    return true;
}

void Renderer::defaultLightingLayout() {
    using namespace layout;
    const float wt = WALL_THICKNESS + 0.1f;
    sceneBounds_ = AABB();
    sceneBounds_.add(vec3(HALL_MIN_X - wt, -0.2f, HALL_MIN_Z - wt));
    sceneBounds_.add(vec3(HALL_MAX_X + wt, HALL_HEIGHT + 0.8f, HALL_MAX_Z + wt));
    AABB regions[3];
    regions[0].add(vec3(-0.85f, 0.0f, -1.15f));      // table + seated players
    regions[0].add(vec3(0.85f, 1.70f, 1.15f));
    regions[1].add(vec3(-3.2f, 0.0f, -3.4f));        // chairs, floor around the table
    regions[1].add(vec3(3.2f, 2.6f, 3.4f));
    regions[2] = sceneBounds_;                        // the whole hall and its thick walls
    shadows_->setRegions(regions, 3);

    AABB interior;
    interior.add(vec3(HALL_MIN_X, 0.0f, HALL_MIN_Z));
    interior.add(vec3(HALL_MAX_X, HALL_HEIGHT, HALL_MAX_Z));
    std::vector<LightProbeDesc> probes;
    LightProbeDesc table;
    table.position = vec3(0.0f, TABLE_TOP_Y + 0.32f, 0.0f);
    table.radius = 1.7f;
    table.innerRadius = 0.95f;
    table.priority = true;
    table.box = interior;
    probes.push_back(table);
    const float xs[3] = {HALL_MIN_X + 2.4f, 0.0f, HALL_MAX_X - 2.4f};
    for (int iz = 0; iz < 4; ++iz)
        for (int ix = 0; ix < 3; ++ix) {
            LightProbeDesc p;
            float z = HALL_MIN_Z + (HALL_MAX_Z - HALL_MIN_Z) * (float(iz) + 0.5f) / 4.0f;
            p.position = vec3(xs[ix], 1.8f, z);
            p.radius = 7.5f;
            p.box = interior;
            probes.push_back(p);
        }
    for (int k = 0; k < 3; ++k) {   // upper row: ceiling, vaults, the top of the windows
        LightProbeDesc p;
        p.position = vec3(0.0f, HALL_HEIGHT - 2.4f, 7.0f * float(k - 1));
        p.radius = 9.0f;
        p.box = interior;
        probes.push_back(p);
    }
    probes_->setProbes(probes);
}

void Renderer::setSceneBounds(const AABB& b) {
    sceneBounds_ = b;
    staticDirty_ = true;
}

void Renderer::setShadowRegions(const AABB* regions, int count) {
    shadows_->setRegions(regions, count);
    staticDirty_ = true;
}

void Renderer::setLightProbes(const std::vector<LightProbeDesc>& probes) {
    std::vector<LightProbeDesc> p = probes;
    for (auto& d : p)
        if (!d.box.valid()) d.box = sceneBounds_;
    probes_->setProbes(p);
    staticDirty_ = true;
}

GLuint Renderer::specularProbes() const { return probes_->baked() ? probes_->specularArray() : dummyCubeArray_.id; }

void Renderer::shutdown() {
    if (!frameUbo_.id) return;
    destroySimpleMsaa();
    post_->shutdown();
    destroyTargets();
    shadows_->shutdown();
    probes_->shutdown();
    planarRefl_->shutdown();
    atmosphere_->shutdown();
    brdfLut_.destroy();
    for (auto* t : {&dummy2D_, &dummyArray_, &dummyCubeArray_}) t->destroy();
    frameUbo_.destroy();
    drawSsbo_.destroy();
    lightSsbo_.destroy();
    lightingUbo_.destroy();
    if (g_samplerShadowCmp) glDeleteSamplers(1, &g_samplerShadowCmp);
    if (g_samplerShadowRaw) glDeleteSamplers(1, &g_samplerShadowRaw);
    g_samplerShadowCmp = g_samplerShadowRaw = 0;
    shaders::shutdown();
}

void Renderer::setSettings(const RenderSettings& s) {
    bool resizeNeeded = s.renderScale != settings_.renderScale;
    bool shadowsChanged = s.shadowMapSize != settings_.shadowMapSize || s.shadowCascades != settings_.shadowCascades ||
                          s.staticShadowCache != settings_.staticShadowCache;
    bool probesChanged = s.probeResolution != settings_.probeResolution || s.probeBounces != settings_.probeBounces ||
                         s.lightProbes != settings_.lightProbes;
    bool planarChanged = s.planarReflections != settings_.planarReflections;
    settings_ = s;
    if (shadowsChanged && frameUbo_.id) shadows_->init(settings_.shadowMapSize, settings_.shadowCascades, settings_.staticShadowCache);
    if (probesChanged && frameUbo_.id) {
        std::vector<LightProbeDesc> keep = probes_->probes();
        probes_->init(settings_.probeResolution);
        probes_->setProbes(keep);
    }
    if (resizeNeeded && width_ > 0) { int w = width_, h = height_; width_ = 0; resize(w, h); }
    else if (planarChanged && width_ > 0) allocatePlanar();
}

void Renderer::createTargets(int w, int h) {
    rt_.w = w;
    rt_.h = h;
    rt_.hdr = gpu::createTexture2D(w, h, GL_RGBA16F);
    rt_.depth = gpu::createTexture2D(w, h, GL_DEPTH_COMPONENT32F);
    rt_.normalRough = gpu::createTexture2D(w, h, GL_RGBA16F);
    rt_.specular = gpu::createTexture2D(w, h, GL_RGBA8);
    rt_.velocity = gpu::createTexture2D(w, h, GL_RG16F);
    rt_.fbMain = gpu::createFramebuffer({&rt_.hdr, &rt_.normalRough, &rt_.specular, &rt_.velocity}, &rt_.depth);
    rt_.fbPrepass = gpu::createFramebuffer({&rt_.normalRough, &rt_.velocity}, &rt_.depth);
    rt_.fbTransparent = gpu::createFramebuffer({&rt_.hdr}, &rt_.depth);
    gpu::checkFramebuffer(rt_.fbMain, "main");
    gpu::checkFramebuffer(rt_.fbPrepass, "prepass");
    gpu::checkFramebuffer(rt_.fbTransparent, "transparent");
}

void Renderer::destroyTargets() {
    for (auto* t : {&rt_.hdr, &rt_.depth, &rt_.normalRough, &rt_.specular, &rt_.velocity}) t->destroy();
    rt_.fbMain.destroy();
    rt_.fbPrepass.destroy();
    rt_.fbTransparent.destroy();
}

void Renderer::resize(int w, int h) {
    if (w == width_ && h == height_) return;
    width_ = w;
    height_ = h;
    int rw = std::max(1, int(float(w) * settings_.renderScale)), rh = std::max(1, int(float(h) * settings_.renderScale));
    destroyTargets();
    createTargets(rw, rh);
    allocatePlanar();
    post_->resize(rw, rh);
}

// Planar reflections: half resolution, one array layer per reflector (up to 4), no targets
// while they are off (Low preset).
void Renderer::allocatePlanar() {
    int layers = settings_.planarReflections ? std::max(1, int(planar_.size())) : 0;
    planarRefl_->resize(std::max(1, rt_.w / 2), std::max(1, rt_.h / 2), layers);
}

// ---------------------------------------------------------------------------------------------
static float halton(int i, int b) {
    float f = 1, r = 0;
    while (i > 0) { f /= float(b); r += f * float(i % b); i /= b; }
    return r;
}

static float mieScaleOf(const Environment& env) { return std::max(0.2f, env.turbidity / 2.0f); }

void Renderer::beginFrame(const Camera& cam, const Environment& env, float dt) {
    camera_ = cam;
    env_ = env;
    dt_ = dt;
    items_.clear();
    drawData_.clear();
    programMemo_.clear();
    lights_.clear();
    ++frameIndex_;

    int w = rt_.w, h = rt_.h;
    float aspect = float(w) / float(h);
    mat4 view = cam.view();
    mat4 projNoJ = cam.proj(aspect);
    vec2 jitter(0, 0);
    if (settings_.taa && !settings_.simple) {   // the simple renderer has no TAA to resolve the jitter
        int k = int(frameIndex_ % 8) + 1;
        jitter = vec2((halton(k, 2) - 0.5f) * 2.0f / float(w), (halton(k, 3) - 0.5f) * 2.0f / float(h));
    }
    mat4 proj = projNoJ;
    // ndc' = ndc + jitter (clip.w = -z_view, so the offset goes negated into column 2).
    proj.c[2].x -= jitter.x;
    proj.c[2].y -= jitter.y;

    FrameUBOData& f = frame_;
    f.view = view;
    f.proj = proj;
    f.viewProj = proj * view;
    f.invView = inverseAffine(view);
    f.invProj = inverse(proj);
    f.invViewProj = inverse(f.viewProj);
    f.viewProjNoJitter = projNoJ * view;
    f.prevViewProj = frameIndex_ > 1 ? prevViewProj_ : f.viewProjNoJitter;
    f.cameraPos = vec4(cam.position, env.time);
    f.resolution = vec4(float(w), float(h), 1.0f / float(w), 1.0f / float(h));
    f.jitter = vec4(jitter.x, jitter.y, prevJitter_.x, prevJitter_.y);
    float exposure = 1.0f / (1.2f * std::pow(2.0f, env.exposureEV100));
    f.exposure = vec4(exposure, 1.0f / exposure, cam.nearZ, aspect);
    vec3 sunDir = normalize(env.sunDirection);
    vec3 sunLux = env.physicalSky ? atmosphere_->sunIlluminance(sunDir, mieScaleOf(env), env.altitudeKm) * env.sunIntensityScale
                                  : env.sunColor * env.sunIlluminance;
    float sunLum = 0.2126f * sunLux.x + 0.7152f * sunLux.y + 0.0722f * sunLux.z;
    f.sunDirection = vec4(sunDir, 0.00465f);
    f.sunRadiance = vec4(sunLux * exposure, sunLum);
    float skyLux = env.physicalSky ? std::max(sunLum * 0.2f, 2000.0f) * env.skyIntensity : env.skyIlluminance;
    // Frame index wrapped below 2^24 (exact as a float; a multiple of 64 for its &7, >>3 and mod-64 readers).
    f.skyParams = vec4(env.turbidity, skyLux * exposure, env.exposureEV100, float(frameIndex_ & 0xFFFFFFu));
    // Fallback hemisphere ambient (only used when light probes are off / not baked yet).
    f.ambientSky = vec4(vec3(0.55f, 0.65f, 0.85f) * (skyLux * 0.12f / PI) * exposure, 0);
    f.ambientGround = vec4(vec3(0.85f, 0.78f, 0.68f) * (sunLum * 0.035f / PI) * exposure, 0);
    f.clipPlane = vec4(0, 0, 0, 1);
    f.passInfo = vec4(0, 0, 0, float(planar_.size()));

    prevViewProj_ = f.viewProjNoJitter;
    prevJitter_ = jitter;
}

void Renderer::submit(const DrawItem& d) {
    if (!d.mesh || !d.material || d.mesh->indexCount == 0) return;
    Item it;
    it.d = d;
    it.drawIndex = uint32_t(drawData_.size());
    vec3 c = transformPoint(d.model, d.mesh->bounds.center());
    it.viewDepth = -transformPoint(frame_.view, c).z;
    float sx = length(d.model.c[0].xyz()), sy = length(d.model.c[1].xyz()), sz = length(d.model.c[2].xyz());
    it.center = c;
    it.radius = length(d.mesh->bounds.extent()) * std::max(sx, std::max(sy, sz)) * 1.02f + 1e-3f;
    items_.push_back(it);

    DrawDataGPU g;
    g.model = d.model;
    g.prevModel = d.hasPrevModel ? d.prevModel : d.model;
    g.normalMatrix = mat4(normalMatrix(d.model));
    for (int i = 0; i < 8; ++i) g.matParams[i] = d.material->params[i];
    for (int i = 0; i < 4; ++i) g.instParams[i] = d.inst[i];
    g.info = vec4(float(hash32(d.objectId * 747796405u + 2891336453u) & 0xFFFFFF) / 16777216.0f,
                  float(d.material->planarReflector), float(d.flags), float(d.objectId));
    g.fade = vec4(clamp(d.opacity, 0.0f, 1.0f), settings_.taa ? 1.0f : 0.0f, 0.0f, 0.0f);
    g.highlight = vec4(d.highlight.xyz(), clamp(d.highlight.w, 0.0f, 1.0f));
    drawData_.push_back(g);
}

void Renderer::addLight(const PointLight& l) { lights_.push_back(l); }

int Renderer::addPlanarReflector(const PlanarReflector& r) {
    if (planar_.size() >= 4) return -1;
    planar_.push_back(r);
    if (planarRefl_->colorArray() && int(planar_.size()) > planarRefl_->layers()) allocatePlanar();
    return int(planar_.size() - 1);
}

void Renderer::uploadFrameUBO(const FrameUBOData& d) {
    glNamedBufferSubData(frameUbo_.id, 0, sizeof(FrameUBOData), &d);
    glBindBufferBase(GL_UNIFORM_BUFFER, UBO_FRAME, frameUbo_.id);
}

// Tessellation shaders are a desktop-GL feature: a GL ES 3.2 context has none, so the Android
// build (SCACELITH_GLES) always takes the untessellated path the Low and Medium presets already
// use (mesh.vert with GL_TRIANGLES instead of the mesh.tesc / mesh.tese pair with GL_PATCHES).
// The option stays in the settings file and in the Options page; it simply has no effect there.
static bool tessellationEnabled(const RenderSettings& s) {
#ifdef SCACELITH_GLES
    (void)s;
    return false;
#else
    return s.tessellation;
#endif
}

const ShaderProgram* Renderer::programFor(const Material& mat, PassId pass, bool allowTess) {
    // Per-frame memo (cleared in beginFrame, so a material edited between frames is seen): the
    // lookup below builds the program's key string, too much to redo at every material change of
    // a front-to-back draw order.
    const uint64_t memoKey = (uint64_t(reinterpret_cast<uintptr_t>(&mat)) << 4) ^ (uint64_t(pass) << 1) ^ uint64_t(allowTess);
    auto memo = programMemo_.find(memoKey);
    if (memo != programMemo_.end()) return memo->second;
    const ShaderProgram* found = programLookup(mat, pass, allowTess);
    programMemo_.emplace(memoKey, found);
    return found;
}

const ShaderProgram* Renderer::programLookup(const Material& mat, PassId pass, bool allowTess) {
    ProgramDesc d;
    d.vs = "shaders/passes/mesh.vert";
    bool tess = mat.tessellated && tessellationEnabled(settings_) && allowTess;
    if (tess) {
        d.tcs = "shaders/passes/mesh.tesc";
        d.tes = "shaders/passes/mesh.tese";
        d.defines.push_back("MESH_TESSELLATED");
    }
    d.material = mat.surface;
    d.displacement = mat.displacement;
    if (settings_.simple && pass == PassId::Main) {
        d.fs = "shaders/passes/simple.frag";
        d.defines.push_back("PASS_MAIN");
        d.defines.push_back("PASS_SIMPLE");
        // The material by name (its constants in simple.frag), and whether it runs its real
        // surface function: the small ones that carry information do.
        const std::string base = mat.surface.substr(mat.surface.find_last_of('/') + 1, std::string::npos);
        const std::string name = base.substr(0, base.find('.'));
        d.defines.push_back("SIMPLE_MAT_" + name);
        // (Not the glass: the hall's windows fill much of the screen, and its procedural cords and
        // dust cost a phone more than the whole opaque scene. simple.frag has a plain pane.)
        bool full = name == "clock_display" || name == "robot_eye" || name == "paper" || name == "robot_wire" ||
                    name == "game_marker" || name == "coach_marker";
        // The board's coordinates and the coach's chest lettering: plain material, plus the marking.
        bool decal = false;
        for (const auto& def : mat.defines) decal = decal || def == "BOARD_COORDINATES" || def == "ROBOT_MARKING";
        // Options > Graphics, material textures: 1 = baked (the material's procedural surface,
        // evaluated once into a texture: bakeSimpleMaterials), 2 = the procedural surface per pixel.
        if (settings_.simpleMaterials >= 2) full = true;
        if (full) d.defines.push_back("SIMPLE_FULL");
        else if (decal) d.defines.push_back("SIMPLE_DECAL");
        if (!full && settings_.simpleMaterials == 1) {
            if (const SimpleBake* b = simpleBakeOf(mat)) {
                char buf[160];
                d.defines.push_back("SIMPLE_BAKED");
                d.defines.push_back("SIMPLE_BAKE_MODE=" + std::to_string(b->mode));
                std::snprintf(buf, sizeof(buf), "SIMPLE_BAKE_TILE=vec2(%.6f,%.6f)", b->tile.x, b->tile.y);
                d.defines.push_back(buf);
                std::snprintf(buf, sizeof(buf), "SIMPLE_BAKE_ORIGIN=vec3(%.6f,%.6f,%.6f)", b->origin.x, b->origin.y, b->origin.z);
                d.defines.push_back(buf);
                if (b->world) d.defines.push_back("SIMPLE_BAKE_WS");
            }
        }
        if (settings_.lightProbes) d.defines.push_back("SIMPLE_PROBES");
        if (mat.doubleSided) d.defines.push_back("MATERIAL_DOUBLE_SIDED");
        if (mat.transparent) d.defines.push_back("MATERIAL_TRANSPARENT");
        for (auto& def : mat.defines) d.defines.push_back(def);
        const ShaderProgram& p = shaders::get(d);
        return p.valid() ? &p : nullptr;
    }
    switch (pass) {
        case PassId::Main: d.fs = "shaders/passes/forward.frag"; d.defines.push_back("PASS_MAIN"); break;
        case PassId::Planar: d.fs = "shaders/passes/forward.frag"; d.defines.push_back("PASS_PLANAR"); break;
        case PassId::Probe: d.fs = "shaders/passes/forward.frag"; d.defines.push_back("PASS_PROBE"); break;
        case PassId::Prepass: d.fs = "shaders/passes/prepass.frag"; d.defines.push_back("PASS_PREPASS"); break;
        case PassId::Shadow: d.fs = "shaders/passes/shadow.frag"; d.defines.push_back("PASS_SHADOW"); break;
    }
    if (mat.doubleSided) d.defines.push_back("MATERIAL_DOUBLE_SIDED");
    if (mat.transparent) d.defines.push_back("MATERIAL_TRANSPARENT");
    for (auto& def : mat.defines) d.defines.push_back(def);
    const ShaderProgram& p = shaders::get(d);
    return p.valid() ? &p : nullptr;
}

void Renderer::bindGlobalTextures() {
    // Keep every unit the lighting samples (8..15) complete with a dummy of the right type.
    for (int u = TEXUNIT_SHADOW; u <= TEXUNIT_SSR; ++u) glBindTextureUnit(GLuint(u), dummy2D_.id);
    glBindTextureUnit(TEXUNIT_SHADOW, shadows_->depthArray());
    glBindSampler(TEXUNIT_SHADOW, g_samplerShadowCmp);
    glBindTextureUnit(TEXUNIT_SHADOW_DEPTH, shadows_->depthArray());
    glBindSampler(TEXUNIT_SHADOW_DEPTH, g_samplerShadowRaw);
    glBindTextureUnit(TEXUNIT_SPECULAR, specularProbes());
    glBindTextureUnit(TEXUNIT_PLANAR, planarRefl_->colorArray() ? planarRefl_->colorArray() : dummyArray_.id);
    glBindTextureUnit(TEXUNIT_BRDF_LUT, brdfLut_.id);
    glBindBufferBase(GL_UNIFORM_BUFFER, UBO_LIGHTING, lightingUbo_.id);
}

void Renderer::drawScene(PassId pass, bool transparents, const DrawFilter& flt) {
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, SSBO_DRAWS, drawSsbo_.id);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, SSBO_LIGHTS, lightSsbo_.id);
    const ShaderProgram* current = nullptr;
    // Opaques are sorted by material: look its program up once per run of items, not per draw.
    const Material* memoMat = nullptr;
    const ShaderProgram* memoProg = nullptr;
    int planarLayer = pass == PassId::Planar ? int(frame_.passInfo.y) : -2;
    for (const Item& it : items_) {
        const Material& mat = *it.d.material;
        if (mat.transparent != transparents) continue;
        if (it.d.flags & flt.skipFlags) continue;
        if ((it.d.flags & flt.requireFlags) != flt.requireFlags) continue;
        if (pass == PassId::Shadow && !(mat.castShadow && (it.d.flags & DRAW_CAST_SHADOW))) continue;
        if (pass == PassId::Planar && mat.planarReflector == planarLayer) continue;  // don't reflect the mirror itself
        if (!(it.d.flags & DRAW_NO_CULL)) {
            if (flt.planeCount > 0 && !gpu::sphereVisible(flt.planes, flt.planeCount, it.center, it.radius)) continue;
            if (flt.minSize > 0.0f && it.radius < flt.minSize * distance(flt.eye, it.center)) continue;
            if (flt.minRadius > 0.0f && it.radius < flt.minRadius) {
                const AABB& b = flt.minRadiusRegion;
                vec3 c = it.center;
                if (c.x >= b.lo.x && c.y >= b.lo.y && c.z >= b.lo.z && c.x <= b.hi.x && c.y <= b.hi.y && c.z <= b.hi.z) continue;
            }
        }
        if (&mat != memoMat) {
            memoProg = programFor(mat, pass, flt.allowTessellation);
            memoMat = &mat;
        }
        const ShaderProgram* p = memoProg;
        if (!p) continue;
        if (p != current) { p->use(); current = p; }
        glProgramUniform1i(p->id, 0, int(it.drawIndex));
        for (int t = 0; t < 8; ++t)
            if (mat.textures[t]) glBindTextureUnit(GLuint(t), mat.textures[t]);
        if (pass == PassId::Main && settings_.simple && settings_.simpleMaterials == 1)
            if (const SimpleBake* sb = simpleBakeOf(mat)) glBindTextureUnit(5, sb->tex.id);   // simple.frag's uBaked
        if (mat.doubleSided || pass == PassId::Shadow) glDisable(GL_CULL_FACE);
        else glEnable(GL_CULL_FACE);
        it.d.mesh->bind();
        bool tess = mat.tessellated && tessellationEnabled(settings_) && flt.allowTessellation;
        if (tess) glPatchParameteri(GL_PATCH_VERTICES, 3);
        const bool timed = pass == PassId::Main && gpu::profileGroupsEnabled();
        double t0 = 0.0;
        if (timed) { glFinish(); t0 = gpu::profileNowMs(); }
        glDrawElements(tess ? GL_PATCHES : GL_TRIANGLES, GLsizei(it.d.mesh->indexCount), GL_UNSIGNED_INT, nullptr);
        if (timed) {
            glFinish();
            std::string key = "mat:" + mat.surface.substr(mat.surface.find_last_of('/') + 1);
            for (const auto& def : mat.defines) key += "+" + def;
            gpu::profileAdd(key, gpu::profileNowMs() - t0);
        }
    }
    glEnable(GL_CULL_FACE);
}

void Renderer::renderSky() {
    const ShaderProgram& p = shaders::fullscreen("shaders/passes/sky.frag");
    if (!p.valid()) return;
    atmosphere_->bindSkyTextures();
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL);  // only where nothing was drawn (depth == 0)
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    p.use();
    gpu::drawFullscreenTriangle();
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
}

void Renderer::updateLightingUBO() {
    LightingUBOData& l = *lub_;
    float exposure = frame_.exposure.x;
    probes_->fillUBO(l, exposure, settings_.lightProbes);
    float softness = std::max(env_.sunSoftness, 0.05f);
    vec3 toa = lighting::Atmosphere::solarTOA() * env_.sunIntensityScale;
    float toaLum = 0.2126f * toa.x + 0.7152f * toa.y + 0.0722f * toa.z;
    l.sunParams = vec4(frame_.sunDirection.w, std::tan(frame_.sunDirection.w * softness), softness, toaLum * exposure);
    l.sunTOA = vec4(toa * exposure, env_.altitudeKm);
    l.skyParams2 = vec4(clamp(env_.cloudCoverage, 0.0f, 1.0f), env_.time, mieScaleOf(env_), env_.skyIntensity);
    l.lightingMisc = vec4(settings_.specularAA, 0.06f, env_.ambientIntensity, 0.0f);
    glNamedBufferSubData(lightingUbo_.id, 0, GLsizeiptr(LIGHTING_UBO_CPU_SIZE), lub_.get());
    glBindBufferBase(GL_UNIFORM_BUFFER, UBO_LIGHTING, lightingUbo_.id);
}

void Renderer::endFrame() {
    gpu::DebugGroup g("frame");
    frame_.passInfo = vec4(0, 0, float(lights_.size()), float(planar_.size()));
    gpu::ensureBuffer(drawSsbo_, std::max<size_t>(1, drawData_.size()) * sizeof(DrawDataGPU));
    if (!drawData_.empty()) glNamedBufferSubData(drawSsbo_.id, 0, GLsizeiptr(drawData_.size() * sizeof(DrawDataGPU)), drawData_.data());
    gpu::ensureBuffer(lightSsbo_, std::max<size_t>(1, lights_.size()) * sizeof(PointLight));
    if (!lights_.empty()) glNamedBufferSubData(lightSsbo_.id, 0, GLsizeiptr(lights_.size() * sizeof(PointLight)), lights_.data());
    if (settings_.simple && settings_.simpleMaterials == 1) bakeSimpleMaterials();

    // Sort opaques by program/material to limit state changes, transparents back to front.
    // The simple renderer has no depth prepass: its opaques go front to back instead, so the early
    // depth test rejects what nearer surfaces already cover (the shader is cheap, a program
    // change costs less than the overdraw it saves).
    static const bool sortByMaterial = std::getenv("SCACELITH_SORT_BY_MATERIAL") != nullptr;   // diagnostics (A/B)
    const bool frontToBack = settings_.simple && !sortByMaterial;
    std::stable_sort(items_.begin(), items_.end(), [frontToBack](const Item& a, const Item& b) {
        if (a.d.material->transparent != b.d.material->transparent) return !a.d.material->transparent;
        if (a.d.material->transparent) return a.viewDepth > b.viewDepth;
        if (frontToBack) return a.viewDepth < b.viewDepth;
        if (a.d.material != b.d.material) return a.d.material < b.d.material;
        return a.viewDepth < b.viewDepth;
    });

    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glDisable(GL_BLEND);

    // ---- Lighting: atmosphere, shadows, probes, planar reflections ---------------------------
    vec3 sunDir = frame_.sunDirection.xyz();
    float mie = mieScaleOf(env_);
    {
        gpu::ProfileScope prof("sky");
        atmosphere_->updateLuts(sunDir, mie, env_.altitudeKm);
        updateLightingUBO();
        uploadFrameUBO(frame_);
        bindGlobalTextures();
    }
    // A frame with nothing submitted (the game's loading screen) bakes neither the static shadow
    // cache nor the probes: they would hold an empty world. staticDirty_ stays set until a frame
    // draws the scene.
    const bool drawsScene = !items_.empty();
    const bool bakeStatic = staticDirty_ && drawsScene;
    shadows_->render(*this, sunDir, std::max(env_.sunSoftness, 0.05f), bakeStatic);
    planarRefl_->prepare(*this);
    updateLightingUBO();
    uploadFrameUBO(frame_);
    bindGlobalTextures();

    // Light probes: bake on the first frame that draws items, on invalidateStatic() and when the
    // sun / sky changed a lot.
    if (settings_.lightProbes && drawsScene) {
        const float key[6] = {sunDir.x, sunDir.y, sunDir.z, mie, env_.cloudCoverage, env_.skyIntensity * env_.sunIntensityScale};
        const float* b = bakeKey_;
        bool sunMoved = key[0] * b[0] + key[1] * b[1] + key[2] * b[2] < std::cos(1.0f * DEG);
        bool skyChanged = std::fabs(key[3] - b[3]) > 0.05f || std::fabs(key[4] - b[4]) > 0.05f ||
                          std::fabs(key[5] - b[5]) > 0.02f * std::max(b[5], 1e-3f);
        if (bakeStatic || !probes_->baked() || sunMoved || skyChanged) {
            probes_->bake(*this, settings_.probeBounces);
            std::memcpy(bakeKey_, key, sizeof(key));
            updateLightingUBO();
            bindGlobalTextures();
        }
    }
    if (settings_.simple) {   // after the probes: the simple renderer reads them when they are on
        if (drawsScene) staticDirty_ = false;
        vec4 viewPlanes[6];
        DrawFilter mainFilter;
        mainFilter.skipFlags = DRAW_HIDDEN_MAIN;
        mainFilter.planes = viewPlanes;
        mainFilter.planeCount = gpu::frustumPlanes(frame_.viewProjNoJitter, viewPlanes);
        renderSimple(mainFilter);
        return;
    }
    if (drawsScene) staticDirty_ = false;
    planarRefl_->render(*this);
    bindGlobalTextures();

    // Culling planes of the main view.
    vec4 viewPlanes[6];
    DrawFilter mainFilter;
    mainFilter.skipFlags = DRAW_HIDDEN_MAIN;
    mainFilter.planes = viewPlanes;
    mainFilter.planeCount = gpu::frustumPlanes(frame_.viewProjNoJitter, viewPlanes);

    // Prepass
    {
        gpu::DebugGroup pg("prepass");
        gpu::ProfileScope prof("prepass");
        frame_.passInfo.x = float(PassId::Prepass);
        uploadFrameUBO(frame_);
        glBindFramebuffer(GL_FRAMEBUFFER, rt_.fbPrepass.id);
        glViewport(0, 0, rt_.w, rt_.h);
        const float zero4[4] = {0, 0, 0, 0};
        float zero = 0.0f;
        glClearNamedFramebufferfv(rt_.fbPrepass.id, GL_COLOR, 0, zero4);
        glClearNamedFramebufferfv(rt_.fbPrepass.id, GL_COLOR, 1, zero4);
        glClearNamedFramebufferfv(rt_.fbPrepass.id, GL_DEPTH, 0, &zero);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_GREATER);
        glDepthMask(GL_TRUE);
        drawScene(PassId::Prepass, false, mainFilter);
    }

    PostInputs pin;
    pin.rt = &rt_;
    pin.frame = &frame_;
    pin.frameUbo = frameUbo_.id;
    pin.shadowArray = shadows_->depthArray();
    pin.dt = dt_;
    pin.backbufferW = width_;
    pin.backbufferH = height_;
    pin.quality = int(settings_.quality);  // render-post: sample-count preset (PostInputs::quality)
    post_->settings.ssao = settings_.ssao;
    post_->settings.ssr = settings_.ssr;
    post_->settings.volumetrics = settings_.volumetrics;
    post_->settings.taa = settings_.taa;
    post_->settings.motionBlur = settings_.motionBlur;
    post_->settings.dof = settings_.dof;
    post_->settings.bloom = settings_.bloom;
    post_->settings.fade = fade;
    {
        gpu::ProfileScope prof("post.ao");
        post_->computeAO(pin);
    }

    // Opaque forward pass
    {
        gpu::DebugGroup mg("opaque");
        gpu::ProfileScope prof("opaque");
        frame_.passInfo.x = float(PassId::Main);
        uploadFrameUBO(frame_);
        glBindFramebuffer(GL_FRAMEBUFFER, rt_.fbMain.id);
        glViewport(0, 0, rt_.w, rt_.h);
        const float zero4[4] = {0, 0, 0, 0};
        glClearNamedFramebufferfv(rt_.fbMain.id, GL_COLOR, 0, zero4);
        glClearNamedFramebufferfv(rt_.fbMain.id, GL_COLOR, 2, zero4);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_GEQUAL);
        glDepthMask(GL_FALSE);
        drawScene(PassId::Main, false, mainFilter);
        glDepthMask(GL_TRUE);
    }
    {
        gpu::DebugGroup sg("sky");
        gpu::ProfileScope prof("sky.draw");
        renderSky();
    }
    // Transparents: dst = src0 + dst * src1 (coloured transmittance, dual-source blending).
    {
        gpu::DebugGroup tg("transparent");
        gpu::ProfileScope prof("transparent");
        glBindFramebuffer(GL_FRAMEBUFFER, rt_.fbTransparent.id);
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_ONE, GL_SRC1_COLOR);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_GREATER);
        glDepthMask(GL_FALSE);
        drawScene(PassId::Main, true, mainFilter);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        // Mesa validates dual-source factors against the draw buffer count even with blending
        // disabled: restore a single-source function for the next MRT passes.
        glBlendFunc(GL_ONE, GL_ZERO);
    }
    {
        gpu::ProfileScope prof("post.resolve");
        post_->resolve(pin);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width_, height_);
    gpu::profileEndFrame();
}

// ---- Baked material textures of the simple renderer -----------------------------------------
const Renderer::SimpleBake* Renderer::simpleBakeOf(const Material& mat) const {
    auto it = simpleBakes_.find(&mat);
    return it != simpleBakes_.end() && it->second.ok ? &it->second : nullptr;
}

// Bakes, once, every material the frame draws that has a bake plan (below) and none yet. Runs at
// the start of a frame, with the draws already uploaded: the bake reads the material's and the
// object's parameters from the first item that uses it.
void Renderer::bakeSimpleMaterials() {
    for (const Item& it : items_) {
        const Material& mat = *it.d.material;
        if (mat.transparent || simpleBakes_.count(&mat)) continue;
        SimpleBake& b = simpleBakes_[&mat];   // a failed plan stays, ok = false: never retried
        const std::string base = mat.surface.substr(mat.surface.find_last_of('/') + 1);
        const std::string name = base.substr(0, base.find('.'));
        auto has = [&](const char* d) {
            for (const auto& x : mat.defines)
                if (x == d) return true;
            return false;
        };
        // The plan: how the material is laid out (object or world space, mesh uv), the size of
        // the patch baked (its pattern repeats beyond it) and the texture resolution.
        int size = 512;
        b.mode = 1;
        if (name == "marble") {
            if (has("MARBLE_FLOOR")) {   // world xz, tiles of params[4].x from params[4].zw
                const float tileM = std::max(mat.params[4].x, 0.05f);
                const float span = tileM * std::max(1.0f, std::round(2.4f / tileM));
                b.mode = 2; b.world = true; b.tile = m::vec2(span, span);
                b.origin = m::vec3(mat.params[4].z, 0.0f, mat.params[4].w);
                size = 1024;
            } else if (has("MARBLE_PIECE")) b.tile = m::vec2(0.08f, 0.08f);
            else if (has("MARBLE_BOARD")) b.tile = m::vec2(0.12f, 0.12f);
            else b.tile = m::vec2(0.6f, 0.6f);
            if (has("BOARD_COORDINATES")) { simpleBakes_.erase(&mat); continue; }   // decal path
        } else if (name == "limestone") {   // world space, whole ashlar blocks (running bond)
            const float bw = std::max(mat.params[2].x, 0.05f), bh = std::max(mat.params[2].y, 0.05f);
            b.world = true;
            b.tile = m::vec2(bw * std::max(1.0f, std::round(3.0f / bw)), 2.0f * bh * std::max(1.0f, std::round(1.5f / bh)));
            b.origin = mat.params[5].xyz();
            size = 1024;
        } else if (name == "wood" || name == "painted") b.tile = m::vec2(1.0f, 1.0f);
        else if (name == "cloth") b.tile = m::vec2(0.3f, 0.3f);
        else if (name == "metal" || name == "lacquer") b.tile = m::vec2(0.2f, 0.2f);
        else if (name == "robot_porcelain" && !has("ROBOT_MARKING")) { b.tile = m::vec2(0.3f, 0.3f); size = 256; }
        else if (name == "robot_joint") { b.tile = m::vec2(0.08f, 0.08f); size = 256; }
        else if (name == "wax") { b.tile = m::vec2(0.1f, 0.1f); size = 256; }
        else if (name == "ceiling" || name == "tapestry") { b.mode = 3; size = 1024; }
        else { simpleBakes_.erase(&mat); continue; }   // no plan: plain colour

        ProgramDesc d;
        d.vs = "shaders/passes/fullscreen.vert";
        d.fs = "shaders/passes/simple_bake.frag";
        d.material = mat.surface;
        d.defines.push_back("PASS_MAIN");
        for (auto& def : mat.defines) d.defines.push_back(def);
        const ShaderProgram& p = shaders::get(d);
        if (!p.valid()) continue;
        const int layers = b.mode == 1 ? 3 : 1;
        b.tex = gpu::createTexture2DArray(size, size, layers, GL_SRGB8_ALPHA8, 0);
        gpu::setWrap(b.tex, GL_REPEAT);
        auto t0 = std::chrono::steady_clock::now();
        p.use();
        glProgramUniform1i(p.id, 0, int(it.drawIndex));
        glProgramUniform2f(p.id, 2, b.mode == 3 ? 1.0f : b.tile.x, b.mode == 3 ? 1.0f : b.tile.y);
        glProgramUniform3f(p.id, 3, b.origin.x, b.origin.y, b.origin.z);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, SSBO_DRAWS, drawSsbo_.id);
        glBindBufferBase(GL_UNIFORM_BUFFER, UBO_FRAME, frameUbo_.id);
        for (int t = 0; t < 8; ++t)
            if (mat.textures[t]) glBindTextureUnit(GLuint(t), mat.textures[t]);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
#ifndef SCACELITH_GLES
        glEnable(GL_FRAMEBUFFER_SRGB);   // the albedo goes in as sRGB (ES always converts)
#endif
        glViewport(0, 0, size, size);
        for (int l = 0; l < layers; ++l) {
            gpu::Framebuffer fb = gpu::createFramebufferLayer(&b.tex, l, nullptr, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, fb.id);
            glProgramUniform1i(p.id, 1, b.mode == 1 ? l : (b.mode == 2 ? 1 : 3));
            gpu::drawFullscreenTriangle();
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            fb.destroy();
        }
#ifndef SCACELITH_GLES
        glDisable(GL_FRAMEBUFFER_SRGB);
#endif
        glGenerateTextureMipmap(b.tex.id);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        glFinish();   // the bake's time in the log (once per material)
        b.ok = true;
        LOGI("simple renderer: baked %s%s (%dx%d x%d) in %.0f ms", name.c_str(), mat.defines.empty() ? "" : ("+" + mat.defines[0]).c_str(),
             size, size, layers, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    glViewport(0, 0, rt_.w, rt_.h);
}

// The multisampled targets of the simple renderer's MSAA option, (re)made at the render size.
bool Renderer::ensureSimpleMsaa() {
    if (msaa_.fb && msaa_.w == rt_.w && msaa_.h == rt_.h) return true;
    destroySimpleMsaa();
    GLint maxSamples = 0;
    glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
    const int samples = std::min(4, int(maxSamples));
    if (samples < 2) return false;
    glCreateRenderbuffers(1, &msaa_.color);
    glNamedRenderbufferStorageMultisample(msaa_.color, samples, GL_RGBA16F, rt_.w, rt_.h);
    glCreateRenderbuffers(1, &msaa_.depth);
    glNamedRenderbufferStorageMultisample(msaa_.depth, samples, GL_DEPTH_COMPONENT32F, rt_.w, rt_.h);
    glCreateFramebuffers(1, &msaa_.fb);
    glNamedFramebufferRenderbuffer(msaa_.fb, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, msaa_.color);
    glNamedFramebufferRenderbuffer(msaa_.fb, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, msaa_.depth);
    glNamedFramebufferDrawBuffer(msaa_.fb, GL_COLOR_ATTACHMENT0);
    if (glCheckNamedFramebufferStatus(msaa_.fb, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        LOGW("simple renderer: no %dx MSAA target, anti-aliasing off", samples);
        destroySimpleMsaa();
        return false;
    }
    msaa_.w = rt_.w;
    msaa_.h = rt_.h;
    LOGI("simple renderer: %dx MSAA at %dx%d", samples, rt_.w, rt_.h);
    return true;
}

void Renderer::destroySimpleMsaa() {
    if (msaa_.fb) glDeleteFramebuffers(1, &msaa_.fb);
    if (msaa_.color) glDeleteRenderbuffers(1, &msaa_.color);
    if (msaa_.depth) glDeleteRenderbuffers(1, &msaa_.depth);
    msaa_ = SimpleMsaa();
}

// The simple renderer (RenderSettings::simple): opaques, sky, transparents into the HDR target,
// then the display transform to the backbuffer.
void Renderer::renderSimple(const DrawFilter& mainFilter) {
    {
        gpu::DebugGroup og("simple.opaque");
        gpu::ProfileScope prof("opaque");
        frame_.passInfo.x = float(PassId::Main);
        uploadFrameUBO(frame_);
        GLuint target = rt_.fbTransparent.id;
        if (settings_.simpleMsaa && ensureSimpleMsaa()) target = msaa_.fb;
        glBindFramebuffer(GL_FRAMEBUFFER, target);
        glViewport(0, 0, rt_.w, rt_.h);
        const float zero4[4] = {0, 0, 0, 0};
        float zero = 0.0f;
        glClearNamedFramebufferfv(target, GL_COLOR, 0, zero4);
        glClearNamedFramebufferfv(target, GL_DEPTH, 0, &zero);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_GREATER);
        glDepthMask(GL_TRUE);
        drawScene(PassId::Main, false, mainFilter);
    }
    {
        gpu::ProfileScope prof("sky.draw");
        renderSky();
    }
    {
        gpu::DebugGroup tg("simple.transparent");
        gpu::ProfileScope prof("transparent");
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        // Premultiplied colour over what is behind; the target's alpha is left alone.
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_GREATER);
        glDepthMask(GL_FALSE);
        drawScene(PassId::Main, true, mainFilter);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ZERO);
    }
    if (settings_.simpleMsaa && msaa_.fb) {
        // Resolve the samples into the HDR target the display pass reads (on a tiled GPU the
        // samples never leave the tile memory: the resolve happens as the tile is written out).
        gpu::DebugGroup rg("simple.resolve");
        glBindFramebuffer(GL_READ_FRAMEBUFFER, msaa_.fb);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rt_.fbTransparent.id);
        glBlitFramebuffer(0, 0, rt_.w, rt_.h, 0, 0, rt_.w, rt_.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    {
        gpu::DebugGroup dg("simple.display");
        gpu::ProfileScope prof("post.resolve");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, width_, height_);
        glDisable(GL_DEPTH_TEST);
        const ShaderProgram& p = shaders::fullscreen("shaders/post/simple_tonemap.frag");
        if (p.valid()) {
            p.use();
            glProgramUniform1f(p.id, 1, std::exp2(post_->settings.exposureCompensation));
            glProgramUniform1f(p.id, 2, post_->settings.fade);
            glBindTextureUnit(0, rt_.hdr.id);
            gpu::drawFullscreenTriangle();
        }
    }
    glViewport(0, 0, width_, height_);
    gpu::profileEndFrame();
}

void Renderer::readBackbuffer(std::vector<uint8_t>& rgb, int& w, int& h) {
    w = width_;
    h = height_;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    rgb.resize(size_t(w) * size_t(h) * 3);
#ifdef SCACELITH_GLES
    // GL ES reads the default framebuffer as RGBA / UNSIGNED_BYTE only (GL_RGB is an error).
    std::vector<uint8_t> tmp(size_t(w) * size_t(h) * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, tmp.data());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            std::memcpy(&rgb[(size_t(y) * size_t(w) + size_t(x)) * 3], &tmp[(size_t(h - 1 - y) * size_t(w) + size_t(x)) * 4], 3);
#else
    std::vector<uint8_t> tmp(size_t(w) * size_t(h) * 3);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, tmp.data());
    for (int y = 0; y < h; ++y) std::memcpy(&rgb[size_t(y) * size_t(w) * 3], &tmp[size_t(h - 1 - y) * size_t(w) * 3], size_t(w) * 3);
#endif
}

}  // namespace render
