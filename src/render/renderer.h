// Scacelith renderer: forward+ PBR pipeline, HDR, reverse-Z, OpenGL 4.6 DSA.
//
// Frame outline (Renderer::endFrame):
//   1. upload FrameUBO, LightingUBO, DrawData SSBO, lights
//   2. atmosphere: sky-view LUT when the sun moved                         (render-lighting)
//   3. sun shadow cascades (static cache + dynamic casters)                (render-lighting)
//   4. light probes / IBL bake when dirty (probeBounces: 1-3 by preset)    (render-lighting)
//   5. planar reflection passes + Gaussian mip chain                       (render-lighting)
//   6. depth + normal + velocity prepass (frustum culled)
//   7. PostFX::computeAO                                                   (render-post)
//   8. opaque forward pass -> HDR colour, normal/roughness, specular, velocity
//   9. sky (atmosphere, sun disk, clouds)
//  10. transparents (glass, eyes' cornea), dual-source blended transmittance
//  11. PostFX::resolve -> SSR, volumetrics, TAA, DOF, motion blur, bloom, tonemap -> backbuffer
// The UI is drawn by the caller on the backbuffer after endFrame().
#pragma once
#include <unordered_map>
#include "../math/math.h"
#include "gpu.h"
#include "material.h"
#include "mesh.h"
#include <memory>
#include <type_traits>
#include <vector>

class PostFX;
class ShaderProgram;

namespace render {

namespace lighting {
class Atmosphere;
class SunShadows;
class LightProbes;
class PlanarReflections;
}  // namespace lighting
struct LightingUBOData;

// View camera. Local frame: -Z forward, +Y up, +X right (OpenGL convention).
struct Camera {
    m::vec3 position{0, 1.2f, 1.0f};
    m::quat orientation;
    float fovY = 50.0f * m::DEG;
    float nearZ = 0.03f;
    m::mat4 view() const;
    m::mat4 proj(float aspect) const;
    m::vec3 forward() const { return m::rotate(orientation, m::vec3(0, 0, -1)); }
    m::vec3 right() const { return m::rotate(orientation, m::vec3(1, 0, 0)); }
    m::vec3 up() const { return m::rotate(orientation, m::vec3(0, 1, 0)); }
    // World-space ray through pixel (px,py) (origin top-left) of a w*h viewport.
    m::Ray screenRay(float px, float py, int w, int h) const;
    void lookAt(m::vec3 target, m::vec3 upHint = m::vec3(0, 1, 0));
};

// Lighting environment. Photometric units: the sun in lux, emissive and sky in nits; the
// exposure (EV100) maps them to display. Shaders receive values pre-multiplied by exposure.
//
// With physicalSky (default) the sun colour and illuminance are derived from sunDirection through
// the atmosphere model (Hillaire 2020: ~128 klux at the top of the atmosphere, reddened and dimmed
// near the horizon); sunColor / sunIlluminance / skyIlluminance are then ignored, and
// sunIntensityScale / skyIntensity scale the physical values (artistic control).
struct Environment {
    // Towards the sun. Default = hall::recommendedSunDirection() (elevation ~31 deg, azimuth ~35 deg
    // towards +Z): through the +Z window of the -X wall onto the board.
    m::vec3 sunDirection = m::normalize(m::vec3(-0.70f, 0.52f, 0.49f));
    m::vec3 sunColor{1.0f, 0.95f, 0.88f};                                  // chromaticity (physicalSky = false)
    float sunIlluminance = 80000.0f;                                       // lux (physicalSky = false)
    float skyIlluminance = 12000.0f;                                       // lux-ish scale for the fallback ambient
    float turbidity = 2.6f;                                                // haze: scales the Mie density (~2 clear, 6 hazy)
    float exposureEV100 = 12.3f;  // manual exposure (render-post may add auto exposure on top); sunlit
                                  // white marble ~2.5 after exposure, shaded walls ~0.1-0.2
    float time = 0.0f;            // seconds, drives subtle animation (dust, flicker, clouds)
    // --- render-lighting additions ---
    bool physicalSky = true;      // derive sun colour/illuminance from the atmosphere
    float sunIntensityScale = 1.0f;
    float skyIntensity = 1.0f;    // scales sky radiance (and so the daylight entering the windows)
    float cloudCoverage = 0.3f;   // 0 = clear sky, 1 = overcast-ish soft cumulus layer
    float sunSoftness = 1.0f;     // multiplies the sun's angular radius for PCSS penumbrae (1 = physical 0.27 deg)
    float ambientIntensity = 1.0f;  // scales all image based lighting (probes, planar, fallback)
    float altitudeKm = 0.25f;     // viewer altitude in the atmosphere (the palace stands on a hill)
};

enum DrawFlags : uint32_t {
    DRAW_CAST_SHADOW = 1u << 0,
    DRAW_STATIC = 1u << 1,          // never moves (shadow caches and light probes rely on it)
    DRAW_NO_REFLECTION = 1u << 2,   // skipped in planar reflection and probe passes
    DRAW_HIDDEN_MAIN = 1u << 3,     // skipped in the main camera pass (e.g. the player's own head)
    DRAW_NO_CULL = 1u << 5,         // never frustum culled (e.g. displacement beyond mesh bounds)
};

struct DrawItem {
    const Mesh* mesh = nullptr;
    const Material* material = nullptr;
    m::mat4 model;
    m::mat4 prevModel;          // previous frame transform (motion vectors). Defaults to model.
    bool hasPrevModel = false;
    m::vec4 inst[4] = {};       // per-instance parameters (SurfaceInput.instParams)
    uint32_t flags = DRAW_CAST_SHADOW;
    uint32_t objectId = 0;      // stable id: seeds per-object randomness (objectSeed)
    // Screen-door opacity in the main view and the planar reflections (1 = opaque): an ordered
    // dither leaves out 1 - opacity of the pixels, the same ones in the prepass and the main pass,
    // and TAA blends them into a see-through surface. Shadows and probes are unaffected. Only
    // honoured by materials with the MATERIAL_SCREEN_DOOR define (others stay opaque).
    float opacity = 1.0f;
    // Designation highlight of opaque draws in the main view and the planar reflections (Coach
    // mode: the pieces the coach talks about), as if a cool light of hue rgb picked the object
    // out, breathing slowly (forward.frag): a pale object turns a cooler white, and towards the
    // silhouette every object takes on the hue, with a thin rim of rgb (as a level after
    // exposure, sunlit white marble ~2.5) that keeps it visible on dark ones. a = strength in
    // [0,1] (0 = off). No new program variant: any material.
    m::vec4 highlight{0.0f, 0.0f, 0.0f, 0.0f};
};

// Point light; becomes a spot light after setSpot() (spotCosOuter > -1). 64 bytes, mirrors PointLightData.
struct PointLight {
    m::vec3 position;
    float radius = 5.0f;        // influence range (m)
    m::vec3 color{1, 1, 1};
    float intensity = 100.0f;   // candela
    m::vec3 direction{0, -1, 0};  // spot axis (from the light towards the scene)
    float spotCosOuter = -2.0f;   // cos(outer half angle); <= -1 = omni. Use setSpot().
    float spotCosInner = -2.0f;
    float sourceRadius = 0.0f;    // emitter radius (m): widens highlights (candle flame ~0.01)
    float pad0 = 0.0f, pad1 = 0.0f;
    void setSpot(m::vec3 dir, float innerAngle, float outerAngle) {
        direction = m::normalize(dir);
        spotCosInner = std::cos(innerAngle);
        spotCosOuter = std::cos(outerAngle);
    }
};
static_assert(sizeof(PointLight) == 64 && std::is_trivially_copyable_v<PointLight>, "PointLight mirrors PointLightData");

// Planar reflector (floor, table top, board): renders the mirrored scene into a layer of the
// planar reflection texture array (TEXUNIT_PLANAR, half the render resolution for every
// reflector). Materials pick a layer via planarReflector.
struct PlanarReflector {
    m::vec3 point{0, 0, 0};
    m::vec3 normal{0, 1, 0};
    bool enabled = true;
    // Optional world bounds of the reflecting surface. When valid, the reflection is skipped
    // when off screen and rendered only inside the reflector's screen rectangle (scissor).
    m::AABB bounds;
    // Skip objects whose bounding radius / distance is below this in the reflection (0 = off).
    float minObjectSize = 0.002f;
};

// Light probe placement (see Renderer::setLightProbes). Probes capture DRAW_STATIC geometry only.
struct LightProbeDesc {
    m::vec3 position;
    float radius = 8.0f;        // outer influence radius (m)
    float innerRadius = 0.0f;   // priority probes: full weight inside this radius
    bool priority = false;      // local probe (e.g. above the table) overriding the grid probes
    m::AABB box;                // parallax proxy box (invalid = scene bounds)
};

enum class Quality { Low, Medium, High, Ultra };

struct RenderSettings {
    Quality quality = Quality::High;
    float renderScale = 1.0f;
    int shadowMapSize = 4096;
    bool planarReflections = true;
    bool ssao = true;
    bool ssr = true;
    bool volumetrics = true;
    bool taa = true;
    bool motionBlur = true;
    bool dof = true;
    bool bloom = true;
    bool tessellation = true;
    // --- render-lighting additions ---
    int shadowCascades = 3;        // 2 (table + hall) or 3 (table + mid + hall)
    bool staticShadowCache = true; // cache DRAW_STATIC casters per cascade
    bool lightProbes = true;       // runtime light probes (else hemisphere fallback ambient)
    int probeResolution = 128;     // capture / prefiltered cube size
    int probeBounces = 2;
    float specularAA = 1.0f;
    // The simple renderer (shaders/passes/simple.frag, Renderer::renderSimple): one forward pass
    // with base colours, sun + shadow, point lights and hemisphere ambient, then a fragment
    // tonemap. No prepass, probes, reflections or compute post chain: for GPUs the full renderer
    // overwhelms (phones). The quality preset still sets the shadow map.
    bool simple = false;
    // Options of the simple renderer: material textures (0 plain colours, 1 the procedural
    // surfaces without their micro detail, 2 in full) and 4x MSAA. Its indirect lighting is
    // lightProbes, as in the full renderer.
    int simpleMaterials = 0;
    bool simpleMsaa = false;
    void applyPreset(Quality q);
};

// Mirrors FrameUBO in shaders/include/common.glsl (std140, keep in sync!).
struct FrameUBOData {
    m::mat4 view, proj, viewProj, invView, invProj, invViewProj;
    m::mat4 prevViewProj;        // previous frame, unjittered
    m::mat4 viewProjNoJitter;    // current frame, unjittered
    m::vec4 cameraPos;           // xyz, w = time (s)
    m::vec4 resolution;          // w, h, 1/w, 1/h of the current render target
    m::vec4 jitter;              // xy = current jitter (NDC), zw = previous
    m::vec4 sunDirection;        // xyz towards sun, w = angular radius (rad)
    m::vec4 sunRadiance;         // rgb = illuminance * colour * exposure, w = raw lux
    m::vec4 skyParams;           // x = turbidity, y = sky illuminance * exposure, z = EV100, w = frame index
    m::vec4 exposure;            // x = pre-exposure multiplier, y = 1/x, z = near plane, w = aspect
    m::vec4 ambientSky;          // fallback hemisphere ambient (pre-exposed)
    m::vec4 ambientGround;
    m::mat4 shadowMatrix[4];     // world -> shadow NDC-to-[0,1] (xy uv, z depth) per cascade
    m::vec4 shadowCascade[4];    // xyz = cascade centre, w = radius (world) per cascade
    m::vec4 shadowParams;        // x = cascade count, y = normal bias (m), z = depth bias, w = light size factor
    m::vec4 clipPlane;           // world plane (xyz n, w d): keep dot(n,p)+d >= 0. (0,0,0,1) = off
    m::vec4 passInfo;            // x = pass id (0 main,1 prepass,2 shadow,3 planar,4 probe), y = layer, z = lightCount, w = planar count
    m::vec4 planarPlanes[4];     // planar reflector planes (xyz n, w d)
    m::mat4 planarViewProj[4];   // reflected view-proj of each planar reflector (for projective lookup)
};
static_assert(sizeof(FrameUBOData) == 1344 && std::is_trivially_copyable_v<FrameUBOData>, "FrameUBOData mirrors FrameUBO");

// Mirrors DrawData in shaders/include/common.glsl (std430).
struct DrawDataGPU {
    m::mat4 model;
    m::mat4 prevModel;
    m::mat4 normalMatrix;   // upper 3x3 used
    m::vec4 matParams[8];
    m::vec4 instParams[4];
    m::vec4 info;           // x = objectSeed, y = planarReflector index (-1 none), z = flags, w = objectId
    m::vec4 fade;           // x = screen-door opacity, y = 1 when the dither changes every frame (TAA), zw unused
    m::vec4 highlight;      // DrawItem::highlight
};
static_assert(sizeof(DrawDataGPU) == 432 && std::is_trivially_copyable_v<DrawDataGPU>, "DrawDataGPU mirrors DrawData");

enum class PassId { Main = 0, Prepass = 1, Shadow = 2, Planar = 3, Probe = 4 };

// Item selection for Renderer::drawScene.
struct DrawFilter {
    uint32_t skipFlags = 0;
    uint32_t requireFlags = 0;          // every one of these flags must be set
    const m::vec4* planes = nullptr;    // culling planes (xyz n, w d); inside: dot(n,c)+d >= -radius
    int planeCount = 0;
    m::vec3 eye{0, 0, 0};
    float minSize = 0.0f;               // skip items whose radius / distance(eye) is below this
    float minRadius = 0.0f;             // skip items smaller than this (m) whose centre is inside
    m::AABB minRadiusRegion;            //   this region (e.g. pieces in coarse shadow cascades)
    bool allowTessellation = true;
};

class Renderer {
public:
    Renderer();
    ~Renderer();
    bool init(const RenderSettings& settings);
    void shutdown();
    void resize(int width, int height);
    void setSettings(const RenderSettings& s);
    const RenderSettings& settings() const { return settings_; }

    void beginFrame(const Camera& cam, const Environment& env, float dt);
    void submit(const DrawItem& item);
    void addLight(const PointLight& light);
    int addPlanarReflector(const PlanarReflector& r);  // returns index, call once at scene setup
    PlanarReflector& planarReflector(int i) { return planar_[size_t(i)]; }
    void endFrame();

    // Marks the static environment as changed (re-bake probes / static shadows).
    void invalidateStatic() { staticDirty_ = true; }
    // Compiles now the program a material uses in a pass, for materials that first appear during
    // play (otherwise their first draw waits for the compile). GL context required.
    void warmProgram(const Material& mat, PassId pass) { programFor(mat, pass); }

    // --- Lighting configuration (render-lighting). Defaults follow game/layout.h (the hall). ---
    // World bounds of the scene: shadow casters range and default probe parallax box.
    void setSceneBounds(const m::AABB& bounds);
    // Receiver regions of the sun cascades, finest first (count <= 3). Default: table + players,
    // the area around the table, the whole hall.
    void setShadowRegions(const m::AABB* regions, int count);
    // Light probe layout (<= 16). Default: one priority probe above the table + a 3x4 hall grid.
    void setLightProbes(const std::vector<LightProbeDesc>& probes);
    GLuint specularProbes() const;       // TEXUNIT_SPECULAR: prefiltered probe cube array
    GLuint lightingUBO() const { return lightingUbo_.id; }
    GLuint brdfLut() const { return brdfLut_.id; }

    // Fade to black overlay (0 = none, 1 = black) and HUD tint, forwarded to post.
    float fade = 0.0f;

    PostFX& post() { return *post_; }
    int width() const { return width_; }
    int height() const { return height_; }
    const FrameUBOData& frameData() const { return frame_; }
    const Camera& camera() const { return camera_; }
    const Environment& environment() const { return env_; }

    // Reads the backbuffer (after endFrame, before swap) as tightly packed RGB8, top row first.
    void readBackbuffer(std::vector<uint8_t>& rgb, int& w, int& h);

    // Render targets (valid after resize). Owned here, consumed by PostFX.
    struct Targets {
        gpu::Texture hdr;          // RGBA16F
        gpu::Texture depth;        // DEPTH32F, reverse-Z
        gpu::Texture normalRough;  // RGBA16F: world normal xyz, roughness w
        gpu::Texture specular;     // RGBA8: F0 rgb, a = reflection mask (1 = wants SSR)
        gpu::Texture velocity;     // RG16F: uv(current) - uv(previous)
        gpu::Framebuffer fbMain, fbPrepass;
        gpu::Framebuffer fbTransparent;  // hdr + depth only (dual-source blended transparents)
        int w = 0, h = 0;
    };
    const Targets& targets() const { return rt_; }

    // Draws the submitted items with one pass type into the currently bound framebuffer.
    // Used internally and by lighting code (probe capture, planar reflections).
    void drawScene(PassId pass, bool transparents, const DrawFilter& filter);
    // Uploads a modified copy of the frame UBO (e.g. reflected camera) for sub-passes.
    void uploadFrameUBO(const FrameUBOData& data);
    GLuint frameUBO() const { return frameUbo_.id; }
    // Draws the sky with the currently uploaded frame UBO (depth test: only where depth == 0).
    void renderSky();

private:
    friend class lighting::SunShadows;
    friend class lighting::LightProbes;
    friend class lighting::PlanarReflections;
    struct Item {
        DrawItem d;
        uint32_t drawIndex;
        float viewDepth;
        m::vec3 center;   // world bounding sphere
        float radius;
    };
    const ::ShaderProgram* programFor(const Material& mat, PassId pass, bool allowTess = true);
    const ::ShaderProgram* programLookup(const Material& mat, PassId pass, bool allowTess);
    std::unordered_map<uint64_t, const ::ShaderProgram*> programMemo_;   // programFor, per frame
    void createTargets(int w, int h);
    void destroyTargets();
    void allocatePlanar();
    void bindGlobalTextures();
    void updateLightingUBO();
    void defaultLightingLayout();
    void renderSimple(const DrawFilter& mainFilter);
    // Texture bakes of the simple renderer (Options: material textures = baked): per material,
    // its procedural surface evaluated once into a texture array (simple_bake.frag).
    struct SimpleBake {
        gpu::Texture tex;          // SRGB8_ALPHA8 array: rgb albedo, a roughness; 3 layers (X, Y, Z) or 1
        int mode = 0;              // 1 triplanar, 2 floor (the +Y plane only), 3 mesh uv
        m::vec2 tile{1, 1};        // metres covered by the texture (uv: 1 x 1)
        m::vec3 origin{0, 0, 0};
        bool world = false;        // projected from world positions (else object space)
        bool ok = false;
    };
    std::unordered_map<const Material*, SimpleBake> simpleBakes_;
    void bakeSimpleMaterials();
    const SimpleBake* simpleBakeOf(const Material& mat) const;
    bool ensureSimpleMsaa();
    void destroySimpleMsaa();
    struct SimpleMsaa {
        GLuint fb = 0, color = 0, depth = 0;
        int w = 0, h = 0;
    } msaa_;

    RenderSettings settings_;
    int width_ = 0, height_ = 0;
    Targets rt_;
    Camera camera_;
    Environment env_;
    FrameUBOData frame_{};
    m::mat4 prevViewProj_;
    m::vec2 prevJitter_{0, 0};
    uint32_t frameIndex_ = 0;
    float dt_ = 0.0f;
    bool staticDirty_ = true;
    std::vector<Item> items_;
    std::vector<DrawDataGPU> drawData_;
    std::vector<PointLight> lights_;
    std::vector<PlanarReflector> planar_;
    gpu::Buffer frameUbo_, drawSsbo_, lightSsbo_, lightingUbo_;
    // Lighting sub-systems
    std::unique_ptr<LightingUBOData> lub_;
    std::unique_ptr<lighting::Atmosphere> atmosphere_;
    std::unique_ptr<lighting::SunShadows> shadows_;
    std::unique_ptr<lighting::LightProbes> probes_;
    std::unique_ptr<lighting::PlanarReflections> planarRefl_;
    gpu::Texture brdfLut_;
    m::AABB sceneBounds_;
    float bakeKey_[6] = {};    // probe bake inputs of the last bake
    // Default textures bound to unused units so samplers are always complete
    gpu::Texture dummy2D_, dummyArray_, dummyCubeArray_;
    std::unique_ptr<PostFX> post_;
};

// Global renderer instance (created by the app).
Renderer& renderer();
void setRenderer(Renderer* r);

}  // namespace render
