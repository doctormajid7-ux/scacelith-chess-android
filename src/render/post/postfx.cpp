// Render-post package: screen-space effects and the post-processing chain. See postfx.h for the
// frame graph and the output contracts, shaders/post/*.comp|frag for the passes.
#include "postfx.h"
#include "post_noise.h"
#include "../shader.h"
#include "../../core/log.h"
#include <algorithm>
#include <cmath>
#include <vector>

using m::vec4;

namespace {

// Mirrors PostUBO in shaders/post/post_common.glsl (std140, vec4 members only).
struct PostUBOData {
    vec4 renderSize, halfSize, outputSize, timing;
    vec4 ao, aoB, ssr, ssrB, vol, volB, volC, taa, mb, dof, bloom, expo, expoB, display, grade, misc;
};
static_assert(sizeof(PostUBOData) == 20 * 16 && std::is_trivially_copyable_v<PostUBOData>, "PostUBOData mirrors PostUBO");

constexpr int kBloomLevels = 6;
constexpr int kNoiseUnit = 7;
constexpr int kShadowUnit = 5;

struct QualityParams {
    int aoSlices, aoSteps, ssrIterations, volSteps, mbSamples, motes;
    float dofStep;  // DOF gather: samples = maxR^2 / (2 * dofStep) (half-res px), min 12
};
const QualityParams kQuality[4] = {
    {1, 4, 24, 12, 8, 700, 1.0f},     // Low
    {1, 6, 32, 16, 10, 1400, 0.8f},   // Medium
    {2, 6, 48, 24, 14, 2200, 0.55f},  // High
    {3, 8, 64, 32, 18, 3200, 0.4f},   // Ultra
};

void bindTex(int unit, GLuint id) { glBindTextureUnit(GLuint(unit), id); }
void bindImg(int unit, const gpu::Texture& t, int level = 0, GLenum access = GL_WRITE_ONLY) {
    glBindImageTexture(GLuint(unit), t.id, level, GL_FALSE, 0, access, t.format);
}
void barrier() { glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT); }
const ShaderProgram* compute(const char* path) {
    const ShaderProgram& p = shaders::compute(path);
    if (!p.valid()) return nullptr;
    p.use();
    return &p;
}
int divUp(int a, int b) { return (a + b - 1) / b; }
int mipSize(int s, int level) { return std::max(1, s >> level); }

}  // namespace

struct PostFX::Impl {
    int w = 0, h = 0, hw = 0, hh = 0;
    int mbTile = 16;
    gpu::Buffer ubo, histogram;
    gpu::Texture blueNoise, dust, aoWhite, ssrNone, exposure;
    gpu::Texture linDepth[2], halfDepth[2], halfNormal, hiz;
    gpu::Texture aoRaw, aoHist[2], aoOut;
    gpu::Texture colorPyr, ssrRays, ssrResolved, ssrHist[2];
    gpu::Texture volRaw, volHist[2];
    gpu::Texture scene, chainA, taaHist[2];
    gpu::Texture dofHalf, dofTiles, dofBlur;
    gpu::Texture mbTiles, mbNeighbor;
    gpu::Texture bloomDown, bloomUp;
    gpu::Framebuffer fbScene, fbChainA;
    GLuint shadowSampler = 0, vao = 0;
    int cur = 0;                     // ping-pong slot written this frame ([cur ^ 1] = previous frame)
    uint32_t frameCounter = 0;
    bool frameOpen = false;          // computeAO ran for the current frame
    bool aoValid = false, ssrValid = false, volValid = false, taaValid = false, expoValid = false;
    int quality = 2;
    PostUBOData u{};

    void destroyTargets() {
        for (gpu::Texture* t : {&linDepth[0], &linDepth[1], &halfDepth[0], &halfDepth[1], &halfNormal, &hiz, &aoRaw, &aoHist[0],
                                &aoHist[1], &aoOut, &colorPyr, &ssrRays, &ssrResolved, &ssrHist[0], &ssrHist[1], &volRaw, &volHist[0],
                                &volHist[1], &scene, &chainA, &taaHist[0], &taaHist[1], &dofHalf, &dofTiles, &dofBlur, &mbTiles,
                                &mbNeighbor, &bloomDown, &bloomUp})
            t->destroy();
        fbScene.destroy();
        fbChainA.destroy();
    }
    void invalidateHistories() { aoValid = ssrValid = volValid = taaValid = false; }
    // Targets of the optional effects, created on first use (resize destroys them): nothing is
    // allocated for what the quality preset leaves off.
    void ensureSSR() {
        if (ssrHist[0].id) return;
        for (int i = 0; i < 2; ++i) ssrHist[i] = gpu::createTexture2D(w, h, GL_RGBA16F);
        hiz = gpu::createTexture2D(w, h, GL_RGBA32F, 0);   // rg = min/max (GL ES has no RG32F image format)
        gpu::setFilter(hiz, GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST);
        colorPyr = gpu::createTexture2D(w, h, GL_RGBA16F, std::min(gpu::mipCount(w, h), 8));
        ssrRays = gpu::createTexture2D(hw, hh, GL_RGBA32F);
        ssrResolved = gpu::createTexture2D(w, h, GL_RGBA16F);
    }
    void ensureVolumetrics() {
        if (volRaw.id) return;
        for (int i = 0; i < 2; ++i) volHist[i] = gpu::createTexture2D(hw, hh, GL_RGBA16F);
        volRaw = gpu::createTexture2D(hw, hh, GL_RGBA16F);
    }
    void ensureDOF() {
        if (dofHalf.id) return;
        dofHalf = gpu::createTexture2D(hw, hh, GL_RGBA16F);
        dofTiles = gpu::createTexture2D(divUp(hw, 8), divUp(hh, 8), GL_RGBA16F);
        dofBlur = gpu::createTexture2D(hw, hh, GL_RGBA16F);
    }
    // A texture to bind where an optional target does not exist (its pass is off: never sampled).
    GLuint orNone(const gpu::Texture& t) const { return t.id ? t.id : ssrNone.id; }
    void beginFrame(PostSettings& s, const PostInputs& in);
    void prepareDepth(const PostInputs& in, bool hiz0);
    void runGTAO(const PostInputs& in);
    void runSSR(const PostInputs& in);
    void runVolumetrics(const PostInputs& in);
    void drawMotes(const PostInputs& in, const PostSettings& s, gpu::Texture& target);
};

PostFX::PostFX() : impl_(new Impl) {}
PostFX::~PostFX() {
    shutdown();
    delete impl_;
    impl_ = nullptr;
}

bool PostFX::init() {
    Impl& I = *impl_;
    I.aoWhite = gpu::createTexture2D(1, 1, GL_R8);
    const unsigned char white = 255;
    glTextureSubImage2D(I.aoWhite.id, 0, 0, 0, 1, 1, GL_RED, GL_UNSIGNED_BYTE, &white);
    // Bound on TEXUNIT_SSR while no SSR history exists: zero confidence.
    I.ssrNone = gpu::createTexture2D(1, 1, GL_RGBA16F);
    const float none[4] = {0, 0, 0, 0};
    glTextureSubImage2D(I.ssrNone.id, 0, 0, 0, 1, 1, GL_RGBA, GL_FLOAT, none);

    std::vector<uint16_t> bn = postnoise::blueNoise(64);
#ifdef SCACELITH_GLES
    // GL_R16 (16-bit unorm) is not a GL ES format without EXT_texture_norm16: R16F holds the same
    // [0, 1] values to 11 bits, plenty for a 64x64 dither pattern.
    std::vector<float> bnf(bn.size());
    for (size_t i = 0; i < bn.size(); ++i) bnf[i] = float(bn[i]) / 65535.0f;
    I.blueNoise = gpu::createTexture2D(64, 64, GL_R16F);
    glTextureSubImage2D(I.blueNoise.id, 0, 0, 0, 64, 64, GL_RED, GL_FLOAT, bnf.data());
#else
    I.blueNoise = gpu::createTexture2D(64, 64, GL_R16);
    glTextureSubImage2D(I.blueNoise.id, 0, 0, 0, 64, 64, GL_RED, GL_UNSIGNED_SHORT, bn.data());
#endif
    gpu::setFilter(I.blueNoise, GL_NEAREST, GL_NEAREST);
    gpu::setWrap(I.blueNoise, GL_REPEAT);

    const int dn = 64;
    std::vector<uint8_t> dust = postnoise::dustNoise3D(dn);
    I.dust = gpu::createTexture3D(dn, dn, dn, GL_R8);
    glTextureSubImage3D(I.dust.id, 0, 0, 0, 0, dn, dn, dn, GL_RED, GL_UNSIGNED_BYTE, dust.data());
    gpu::setWrap(I.dust, GL_REPEAT);

    I.exposure = gpu::createTexture2D(1, 1, GL_RGBA32F);
    const float zero4[4] = {0, 0, 0, 0};
    glTextureSubImage2D(I.exposure.id, 0, 0, 0, 1, 1, GL_RGBA, GL_FLOAT, zero4);
    // 256 bins, then the last exposure state (vec4, exposure_average.comp).
    std::vector<uint32_t> zeros(256 + 4, 0u);
    I.histogram = gpu::createBuffer(zeros.size() * sizeof(uint32_t), zeros.data(), GL_DYNAMIC_STORAGE_BIT);
    gpu::ensureBuffer(I.ubo, sizeof(PostUBOData));

    glCreateSamplers(1, &I.shadowSampler);
    glSamplerParameteri(I.shadowSampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(I.shadowSampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(I.shadowSampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(I.shadowSampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(I.shadowSampler, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glSamplerParameteri(I.shadowSampler, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glCreateVertexArrays(1, &I.vao);
    return true;
}

void PostFX::shutdown() {
    if (!impl_) return;
    Impl& I = *impl_;
    I.destroyTargets();
    for (gpu::Texture* t : {&I.blueNoise, &I.dust, &I.aoWhite, &I.ssrNone, &I.exposure}) t->destroy();
    I.ubo.destroy();
    I.histogram.destroy();
    if (I.shadowSampler) glDeleteSamplers(1, &I.shadowSampler);
    if (I.vao) glDeleteVertexArrays(1, &I.vao);
    I.shadowSampler = 0;
    I.vao = 0;
    I.w = I.h = 0;
}

void PostFX::resize(int renderW, int renderH) {
    Impl& I = *impl_;
    if (renderW == I.w && renderH == I.h) return;
    I.destroyTargets();
    I.w = std::max(1, renderW);
    I.h = std::max(1, renderH);
    I.hw = std::max(1, I.w / 2);
    I.hh = std::max(1, I.h / 2);
    const int w = I.w, h = I.h, hw = I.hw, hh = I.hh;
    for (int i = 0; i < 2; ++i) {
        I.linDepth[i] = gpu::createTexture2D(w, h, GL_R32F);
        I.halfDepth[i] = gpu::createTexture2D(hw, hh, GL_R32F);
        I.aoHist[i] = gpu::createTexture2D(hw, hh, GL_RGBA16F);
        I.taaHist[i] = gpu::createTexture2D(w, h, GL_RGBA16F);
    }
    I.halfNormal = gpu::createTexture2D(hw, hh, GL_RGBA8);        I.aoRaw = gpu::createTexture2D(hw, hh, GL_RGBA16F);        I.aoOut = gpu::createTexture2D(w, h, GL_RGBA8);
    I.scene = gpu::createTexture2D(w, h, GL_RGBA16F);
    I.chainA = gpu::createTexture2D(w, h, GL_RGBA16F);
    I.mbTile = std::clamp(int(std::lround(20.0 * double(h) / 1080.0)), 8, 32);        I.mbTiles = gpu::createTexture2D(divUp(w, I.mbTile), divUp(h, I.mbTile), GL_RGBA16F);
        I.mbNeighbor = gpu::createTexture2D(divUp(w, I.mbTile), divUp(h, I.mbTile), GL_RGBA16F);
    int bloomLevels = std::min(kBloomLevels, gpu::mipCount(hw, hh));
    I.bloomDown = gpu::createTexture2D(hw, hh, GL_RGBA16F, bloomLevels);
    I.bloomUp = gpu::createTexture2D(hw, hh, GL_RGBA16F, bloomLevels);
    I.fbScene = gpu::createFramebuffer({&I.scene}, nullptr);
    I.fbChainA = gpu::createFramebuffer({&I.chainA}, nullptr);
    I.invalidateHistories();
    I.expoValid = false;
    LOGI("post: render %dx%d, half %dx%d, motion blur tile %d", w, h, hw, hh, I.mbTile);
}

// ---------------------------------------------------------------------------------------------
void PostFX::Impl::beginFrame(PostSettings& s, const PostInputs& in) {
    cur ^= 1;
    ++frameCounter;
    frameOpen = true;
    quality = std::clamp(in.quality >= 0 ? in.quality : s.quality, 0, 3);
    if (s.resetHistory || s.fade >= 0.999f) invalidateHistories();
    if (s.resetHistory) expoValid = false;
    s.resetHistory = false;
    // A debug view shows its target even while the effect is off, as it did before targets were lazy.
    if (s.ssr || s.debugView == 2 || s.debugView == 7) ensureSSR();
    if ((s.volumetrics && in.shadowArray != 0) || s.debugView == 3) ensureVolumetrics();
    if (s.dof || s.debugView == 4) ensureDOF();
    const QualityParams& q = kQuality[quality];
    const render::FrameUBOData& f = *in.frame;
    int bw = in.backbufferW > 0 ? in.backbufferW : w, bh = in.backbufferH > 0 ? in.backbufferH : h;
    float res = float(h) / 1080.0f;
    u.renderSize = vec4(float(w), float(h), 1.0f / float(w), 1.0f / float(h));
    u.halfSize = vec4(float(hw), float(hh), 1.0f / float(hw), 1.0f / float(hh));
    u.outputSize = vec4(float(bw), float(bh), 1.0f / float(bw), 1.0f / float(bh));
    // The counter is sent wrapped at 2^23: exact as a float, and a multiple of 8 keeps its &7 / &3
    // readers continuous. The counter itself never wraps.
    u.timing = vec4(in.dt, float(frameCounter & 0x7FFFFFu), f.cameraPos.w, res);
    u.ao = vec4(s.aoRadius, s.aoPower, 0.22f * float(hh), aoValid ? 1.0f : 0.0f);
    u.aoB = vec4(float(q.aoSlices), float(q.aoSteps), 0.62f, 0.0f);
    u.ssr = vec4(s.ssrMaxRoughness, s.ssrThickness, s.ssrIntensity, float(q.ssrIterations));
    u.ssrB = vec4(0.08f, float(std::min(gpu::mipCount(w, h) - 1, 8)), ssrValid ? 1.0f : 0.0f, 0.3f);  // HiZ levels
    u.vol = vec4(s.volumetricDensity, s.volumetricAnisotropy, s.volumetricAmbient, s.volumetricMaxDistance);
    u.volB = vec4(float(q.volSteps), s.volumetricNoise, volValid ? 1.0f : 0.0f, s.dustMotes);
    u.volC = vec4(0.022f, 0.007f, -0.013f, 0.34f);
    u.taa = vec4(taaValid ? 1.0f : 0.0f, s.taaSharpness, 1.0f, 0.0f);
    u.mb = vec4(s.motionBlurShutter, float(mbTile), float(q.mbSamples & ~1), float(mbTile));
    // Thin-lens CoC scale (see dof_common.glsl). proj[1][1] = 1 / tan(fovY / 2).
    {
        float p11 = std::max(f.proj.c[1].y, 1e-3f);
        float focal = s.dofFocalLengthMM > 0.0f ? s.dofFocalLengthMM : 12.0f * p11;  // mm
        float sensor = s.dofFocalLengthMM > 0.0f ? 2.0f * focal / p11 : 24.0f;         // mm (image height)
        float zf = std::max(s.dofFocusDistance, 0.05f) * 1000.0f;
        float aperture = focal / std::max(s.dofFStop, 0.5f);
        float scale = 0.5f * aperture * focal / std::max(zf - focal, 1.0f) / sensor * float(h);
        u.dof = vec4(std::max(s.dofFocusDistance, 0.05f), scale, std::clamp(s.dofMaxRadius, 0.0f, 16.0f) * res, q.dofStep);
    }
    u.bloom = vec4(s.bloomIntensity, s.bloomScatter, float(bloomDown.levels), 0.0f);
    u.expo = vec4(s.exposureCompensation, s.autoExposure ? 1.0f : 0.0f, s.autoExposureMinEV, s.autoExposureMaxEV);
    // Target: the pre-exposed luminance a correctly exposed average scene has with the renderer's
    // exposure formula (1/(1.2*2^EV) with K = 12.5: 0.125/1.2).
    u.expoB = vec4(s.autoExposureSpeedUp, s.autoExposureSpeedDown, 0.104f, expoValid ? 1.0f : 0.0f);
    u.display = vec4(s.filmGrain, s.vignette, s.chromaticAberration * res, std::clamp(s.fade, 0.0f, 1.0f));
    u.grade = vec4(s.contrast, s.saturation, s.splitTone, 0.0f);
    u.misc = vec4(float(s.debugView), s.ssrCompositeInResolve ? 1.0f : 0.0f, std::min(s.volumetricSkyDistance, s.volumetricMaxDistance),
                  postnoise::goldenPhase(frameCounter));
    glNamedBufferSubData(ubo.id, 0, sizeof(PostUBOData), &u);
    glBindBufferBase(GL_UNIFORM_BUFFER, UBO_POST, ubo.id);
    glBindBufferBase(GL_UNIFORM_BUFFER, UBO_FRAME, in.frameUbo);
    bindTex(kNoiseUnit, blueNoise.id);
}

void PostFX::Impl::prepareDepth(const PostInputs& in, bool hiz0) {
    gpu::DebugGroup g("post.depth");
    if (const ShaderProgram* p = compute("shaders/post/depth_prep.comp")) {
        bindTex(0, in.rt->depth.id);
        bindImg(0, linDepth[cur]);
        glProgramUniform1i(p->id, 1, hiz0 ? 1 : 0);  // HiZ level 0: only SSR reads the pyramid
        if (hiz0) bindImg(1, hiz, 0);
        gpu::dispatch2D(w, h);
    }
    if (compute("shaders/post/half_prep.comp")) {
        bindTex(0, in.rt->depth.id);
        bindTex(1, in.rt->normalRough.id);
        bindImg(0, halfDepth[cur]);
        bindImg(1, halfNormal);
        gpu::dispatch2D(hw, hh);
    }
    barrier();
}

void PostFX::Impl::runGTAO(const PostInputs& in) {
    gpu::DebugGroup g("post.gtao");
    int prev = cur ^ 1;
    if (!compute("shaders/post/gtao.comp")) return;
    bindTex(0, halfDepth[cur].id);
    bindTex(1, halfNormal.id);
    bindImg(0, aoRaw);
    gpu::dispatch2D(hw, hh);
    barrier();
    if (!compute("shaders/post/gtao_temporal.comp")) return;
    bindTex(0, aoRaw.id);
    bindTex(1, halfDepth[cur].id);
    bindTex(2, halfDepth[prev].id);
    bindTex(3, in.rt->velocity.id);
    bindTex(4, aoHist[prev].id);
    bindImg(0, aoHist[cur]);
    gpu::dispatch2D(hw, hh);
    barrier();
    if (!compute("shaders/post/gtao_upsample.comp")) return;
    bindTex(0, aoHist[cur].id);
    bindTex(1, halfDepth[cur].id);
    bindTex(2, halfNormal.id);
    bindTex(3, linDepth[cur].id);
    bindTex(4, in.rt->normalRough.id);
    bindImg(0, aoOut);
    gpu::dispatch2D(w, h);
    barrier();
    aoValid = true;
}

void PostFX::Impl::runSSR(const PostInputs& in) {
    gpu::DebugGroup g("post.ssr");
    int prev = cur ^ 1;
    // Min/max HiZ pyramid (level 0 written by prepareDepth).
    if (const ShaderProgram* p = compute("shaders/post/hiz_build.comp")) {
        bindTex(0, hiz.id);
        for (int l = 1; l < hiz.levels; ++l) {
            glProgramUniform1i(p->id, 1, l - 1);
            bindImg(0, hiz, l);
            gpu::dispatch2D(mipSize(w, l), mipSize(h, l));
            barrier();
        }
    }
    // Colour pyramid for glossy cone fetches.
    glCopyImageSubData(in.rt->hdr.id, GL_TEXTURE_2D, 0, 0, 0, 0, colorPyr.id, GL_TEXTURE_2D, 0, 0, 0, 0, w, h, 1);
    glGenerateTextureMipmap(colorPyr.id);
    if (!compute("shaders/post/ssr_trace.comp")) return;
    bindTex(0, in.rt->depth.id);
    bindTex(1, in.rt->normalRough.id);
    bindTex(2, in.rt->specular.id);
    bindTex(3, hiz.id);
    bindImg(0, ssrRays);
    gpu::dispatch2D(hw, hh);
    barrier();
    if (!compute("shaders/post/ssr_resolve.comp")) return;
    bindTex(3, ssrRays.id);
    bindTex(4, colorPyr.id);
    bindImg(0, ssrResolved);
    gpu::dispatch2D(w, h);
    barrier();
    if (!compute("shaders/post/ssr_temporal.comp")) return;
    bindTex(0, ssrResolved.id);
    bindTex(1, ssrHist[prev].id);
    bindTex(2, in.rt->velocity.id);
    bindTex(3, linDepth[cur].id);
    bindTex(4, linDepth[prev].id);
    bindImg(0, ssrHist[cur]);
    gpu::dispatch2D(w, h);
    barrier();
    ssrValid = true;
}

void PostFX::Impl::runVolumetrics(const PostInputs& in) {
    gpu::DebugGroup g("post.volumetrics");
    int prev = cur ^ 1;
    if (!compute("shaders/post/volumetric.comp")) return;
    bindTex(0, halfDepth[cur].id);
    bindTex(kShadowUnit, in.shadowArray);
    glBindSampler(kShadowUnit, shadowSampler);
    bindTex(6, dust.id);
    bindImg(0, volRaw);
    gpu::dispatch2D(hw, hh);
    glBindSampler(kShadowUnit, 0);
    barrier();
    if (!compute("shaders/post/volumetric_temporal.comp")) return;
    bindTex(0, volRaw.id);
    bindTex(1, volHist[prev].id);
    bindTex(2, halfDepth[cur].id);
    bindTex(3, halfDepth[prev].id);
    bindTex(4, in.rt->velocity.id);
    bindImg(0, volHist[cur]);
    gpu::dispatch2D(hw, hh);
    barrier();
    volValid = true;
}

void PostFX::Impl::drawMotes(const PostInputs& in, const PostSettings& s, gpu::Texture& target) {
    const ShaderProgram& p = shaders::get([] {
        ProgramDesc d;
        d.vs = "shaders/post/motes.vert";
        d.fs = "shaders/post/motes.frag";
        return d;
    }());
    if (!p.valid()) return;
    gpu::DebugGroup g("post.motes");
    const gpu::Framebuffer& fb = &target == &scene ? fbScene : fbChainA;
    glBindFramebuffer(GL_FRAMEBUFFER, fb.id);
    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    p.use();
    glProgramUniform1f(p.id, 1, 8.0f);
    bindTex(0, linDepth[cur].id);
    bindTex(kShadowUnit, in.shadowArray);
    glBindSampler(kShadowUnit, shadowSampler);
    glBindVertexArray(vao);
    int count = int(float(kQuality[quality].motes) * std::min(s.dustMotes, 2.0f));
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, count);
    glBindSampler(kShadowUnit, 0);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    barrier();
}

// ---------------------------------------------------------------------------------------------
void PostFX::computeAO(const PostInputs& in) {
    Impl& I = *impl_;
    if (!in.rt || !in.frame) return;
    if (I.w != in.rt->w || I.h != in.rt->h) resize(in.rt->w, in.rt->h);
    gpu::DebugGroup g("post.computeAO");
    I.beginFrame(settings, in);
    I.prepareDepth(in, settings.ssr);
    if (settings.ssao) I.runGTAO(in);
    else I.aoValid = false;
    // Units 0..7 are material units: leave them clean for the forward pass.
    for (int u = 0; u < 8; ++u) bindTex(u, 0);
    bindTex(TEXUNIT_AO, settings.ssao && I.aoValid ? I.aoOut.id : I.aoWhite.id);
    bindTex(TEXUNIT_SSR, settings.ssr && I.ssrValid ? I.ssrHist[I.cur ^ 1].id : I.ssrNone.id);
    glUseProgram(0);
}

void PostFX::resolve(const PostInputs& in) {
    Impl& I = *impl_;
    if (!in.rt || !in.frame) return;
    if (I.w != in.rt->w || I.h != in.rt->h) resize(in.rt->w, in.rt->h);
    gpu::DebugGroup g("post.resolve");
    if (!I.frameOpen) {
        I.beginFrame(settings, in);
        I.prepareDepth(in, settings.ssr);
    }
    glBindBufferBase(GL_UNIFORM_BUFFER, UBO_POST, I.ubo.id);
    glBindBufferBase(GL_UNIFORM_BUFFER, UBO_FRAME, in.frameUbo);
    bindTex(kNoiseUnit, I.blueNoise.id);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    const bool doSSR = settings.ssr;
    const bool doVol = settings.volumetrics && in.shadowArray != 0;
    if (doSSR) I.runSSR(in);
    else I.ssrValid = false;
    if (doVol) I.runVolumetrics(in);
    else I.volValid = false;

    // Combine HDR + SSR + volumetrics.
    if (const ShaderProgram* p = compute("shaders/post/combine.comp")) {
        gpu::DebugGroup cg("post.combine");
        int flags = (doSSR && I.ssrValid && settings.ssrCompositeInResolve ? 1 : 0) | (doVol && I.volValid ? 2 : 0);
        glProgramUniform1i(p->id, 1, flags);
        bindTex(0, in.rt->hdr.id);
        bindTex(1, I.orNone(I.ssrHist[I.cur]));
        bindTex(2, in.rt->specular.id);
        bindTex(3, in.rt->normalRough.id);
        bindTex(4, I.linDepth[I.cur].id);
        bindTex(5, I.orNone(I.volHist[I.cur]));
        bindTex(6, I.halfDepth[I.cur].id);
        bindImg(0, I.scene);
        gpu::dispatch2D(I.w, I.h);
        barrier();
    }
    gpu::Texture* cur = &I.scene;
    auto other = [&]() { return cur == &I.scene ? &I.chainA : &I.scene; };

    if (settings.taa && compute("shaders/post/taa.comp")) {
        gpu::DebugGroup tg("post.taa");
        bindTex(0, I.scene.id);
        bindTex(1, I.taaHist[I.cur ^ 1].id);
        bindTex(2, in.rt->velocity.id);
        bindTex(3, I.linDepth[I.cur].id);
        bindImg(0, I.taaHist[I.cur]);
        bindImg(1, I.chainA);
        gpu::dispatch2D(I.w, I.h);
        barrier();
        cur = &I.chainA;
        I.taaValid = true;
    } else {
        I.taaValid = false;
    }

    if (doVol && settings.dustMotes > 0.0f) I.drawMotes(in, settings, *cur);

    if (settings.dof && compute("shaders/post/dof_prepare.comp")) {
        gpu::DebugGroup dg("post.dof");
        bindTex(0, cur->id);
        bindTex(1, I.linDepth[I.cur].id);
        bindImg(0, I.dofHalf);
        bindImg(1, I.dofTiles);
        gpu::dispatch2D(I.hw, I.hh);
        barrier();
        if (compute("shaders/post/dof_gather.comp")) {
            bindTex(0, I.dofHalf.id);
            bindTex(1, I.dofTiles.id);
            bindImg(0, I.dofBlur);
            gpu::dispatch2D(I.hw, I.hh);
            barrier();
        }
        if (compute("shaders/post/dof_composite.comp")) {
            gpu::Texture* dst = other();
            bindTex(0, cur->id);
            bindTex(1, I.dofBlur.id);
            bindTex(2, I.dofHalf.id);
            bindTex(3, I.linDepth[I.cur].id);
            bindImg(0, *dst);
            gpu::dispatch2D(I.w, I.h);
            barrier();
            cur = dst;
        }
    }

    if (settings.motionBlur && settings.motionBlurShutter > 0.0f && compute("shaders/post/mb_tilemax.comp")) {
        gpu::DebugGroup mg("post.motionblur");
        bindTex(0, in.rt->velocity.id);
        bindImg(0, I.mbTiles);
        glDispatchCompute(GLuint(I.mbTiles.width), GLuint(I.mbTiles.height), 1);
        barrier();
        if (compute("shaders/post/mb_neighbormax.comp")) {
            bindTex(0, I.mbTiles.id);
            bindImg(0, I.mbNeighbor);
            gpu::dispatch2D(I.mbTiles.width, I.mbTiles.height);
            barrier();
        }
        if (compute("shaders/post/mb_gather.comp")) {
            gpu::Texture* dst = other();
            bindTex(0, cur->id);
            bindTex(1, in.rt->velocity.id);
            bindTex(2, I.linDepth[I.cur].id);
            bindTex(3, I.mbNeighbor.id);
            bindImg(0, *dst);
            gpu::dispatch2D(I.w, I.h);
            barrier();
            cur = dst;
        }
    }

    // Bloom pyramid (its quarter-resolution level also feeds the exposure histogram).
    const bool needPyramid = settings.bloom || settings.autoExposure || settings.debugView == 6;
    if (needPyramid) {
        gpu::DebugGroup bg("post.bloom");
        if (const ShaderProgram* p = compute("shaders/post/bloom_down.comp")) {
            for (int l = 0; l < I.bloomDown.levels; ++l) {
                bindTex(0, l == 0 ? cur->id : I.bloomDown.id);
                glProgramUniform1i(p->id, 1, l == 0 ? 0 : l - 1);
                glProgramUniform1i(p->id, 2, l == 0 ? 1 : 0);
                bindImg(0, I.bloomDown, l);
                gpu::dispatch2D(mipSize(I.hw, l), mipSize(I.hh, l));
                barrier();
            }
        }
        if (settings.bloom || settings.debugView == 6) {
            int last = I.bloomDown.levels - 1;
            glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);  // the copy reads bloom_down's image stores
            glCopyImageSubData(I.bloomDown.id, GL_TEXTURE_2D, last, 0, 0, 0, I.bloomUp.id, GL_TEXTURE_2D, last, 0, 0, 0,
                               mipSize(I.hw, last), mipSize(I.hh, last), 1);
            if (const ShaderProgram* p = compute("shaders/post/bloom_up.comp")) {
                bindTex(0, I.bloomDown.id);
                bindTex(1, I.bloomUp.id);
                for (int l = last - 1; l >= 0; --l) {
                    glProgramUniform1i(p->id, 1, l);
                    bindImg(0, I.bloomUp, l);
                    gpu::dispatch2D(mipSize(I.hw, l), mipSize(I.hh, l));
                    barrier();
                }
            }
        }
    }
    if (settings.autoExposure && needPyramid) {
        gpu::DebugGroup eg("post.exposure");
        int lod = std::min(1, I.bloomDown.levels - 1);
        if (const ShaderProgram* p = compute("shaders/post/exposure_histogram.comp")) {
            glProgramUniform1i(p->id, 1, lod);
            bindTex(0, I.bloomDown.id);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, SSBO_USER, I.histogram.id);
            gpu::dispatch2D(mipSize(I.hw, lod), mipSize(I.hh, lod), 16, 16);
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
            if (compute("shaders/post/exposure_average.comp")) {
                bindImg(0, I.exposure, 0, GL_WRITE_ONLY);
                glDispatchCompute(1, 1, 1);
                // barrier() + the histogram the average cleared, for next frame's atomics.
                glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT |
                                GL_SHADER_STORAGE_BARRIER_BIT);
                I.expoValid = true;
            }
        }
    } else {
        I.expoValid = false;
    }

    // Display transform to the backbuffer.
    {
        gpu::DebugGroup fg("post.display");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, in.backbufferW, in.backbufferH);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        const ShaderProgram& p = shaders::fullscreen("shaders/post/tonemap.frag");
        if (p.valid()) {
            p.use();
            int flags = (settings.bloom ? 1 : 0) | (settings.autoExposure && I.expoValid ? 2 : 0) |
                        (in.backbufferW != I.w || in.backbufferH != I.h ? 4 : 0);
            glProgramUniform1i(p.id, 1, flags);
            bindTex(0, cur->id);
            bindTex(1, I.bloomUp.id);
            bindTex(2, I.exposure.id);
            GLuint dbg = I.aoOut.id;
            switch (settings.debugView) {
                case 2: dbg = I.ssrHist[I.cur].id; break;
                case 3: dbg = I.volHist[I.cur].id; break;
                case 4: dbg = I.dofHalf.id; break;
                case 5: dbg = in.rt->velocity.id; break;
                case 6: dbg = I.bloomUp.id; break;
                case 7: dbg = I.hiz.id; break;
                default: break;
            }
            bindTex(3, dbg);
            gpu::drawFullscreenTriangle();
        }
    }
    for (int u = 0; u < 8; ++u) bindTex(u, 0);
    glUseProgram(0);
    I.frameOpen = false;
}
