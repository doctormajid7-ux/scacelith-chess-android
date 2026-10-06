// Simple forward shading (RenderSettings::simple): the light renderer of GPUs the full one
// overwhelms (phones). One pass straight into the HDR target, no prepass, no compute post chain.
//
//  * Surfaces: the large materials (stone, marble, wood, cloth, porcelain...) are not evaluated:
//    their procedural surface functions are what a phone GPU cannot afford per pixel. They take
//    their base colour (matParams[0].rgb by the library's convention) and a few per-material
//    constants below. The small materials that carry information (clock display, eyes, score
//    sheet, board coordinates, markers) run their real surface function: SIMPLE_FULL,
//    chosen by Renderer::programFor.
//  * Lighting: sun (GGX + Lambert) with a 4-tap shadow lookup, point lights, hemisphere ambient,
//    emission. No probes, no reflections, no AO.
//  * Transparents: premultiplied alpha blending (GL_ONE, GL_ONE_MINUS_SRC_ALPHA), no dual source.
#include "shaders/include/common.glsl"
#include "shaders/include/surface.glsl"
#if defined(SIMPLE_FULL) || defined(SIMPLE_DECAL)
// SIMPLE_DECAL: the material file is included for the one function that draws its marking (the
// board coordinates, the coach's chest lettering), not for its surface (see surfaceSimple).
#include "shaders/include/noise.glsl"
#pragma material
#endif
#ifdef SIMPLE_PROBES
// Indirect lighting on (Renderer::programFor): the full renderer's lighting library, for its
// probe blend (gatherProbes, probeSpecular). Kept out otherwise: even unused behind a uniform
// branch, its code costs a phone GPU registers and time.
#include "shaders/include/lighting.glsl"
#else
#include "shaders/include/brdf.glsl"
#include "shaders/lighting/lighting_ubo.glsl"
layout(binding = 8) uniform sampler2DArrayShadow uShadowMap;
vec3 hemisphereAmbient(vec3 n) { return mix(frame.ambientGround.rgb, frame.ambientSky.rgb, n.y * 0.5 + 0.5); }
#endif
#include "shaders/passes/fragment_input.glsl"

layout(location = 0) out vec4 outColor;

#ifdef SIMPLE_BAKED
// The material's baked surface (Renderer::bakeSimpleMaterials, simple_bake.frag): rgb albedo,
// a roughness. Modes: 1 triplanar (layers = the X, Y, Z planes), 2 the +Y plane (floor), 3 the
// mesh's uv. The pattern repeats every SIMPLE_BAKE_TILE metres from SIMPLE_BAKE_ORIGIN.
layout(binding = 5) uniform sampler2DArray uBaked;
vec4 bakedSurface(SurfaceInput i) {
#ifdef SIMPLE_BAKE_WS
    vec3 p = i.positionWS - SIMPLE_BAKE_ORIGIN;
    vec3 n = i.normalWS;
#else
    vec3 p = i.positionOS - SIMPLE_BAKE_ORIGIN;
    vec3 n = i.normalOS;
#endif
#if SIMPLE_BAKE_MODE == 3
    return texture(uBaked, vec3(i.uv, 0.0));
#elif SIMPLE_BAKE_MODE == 2
    return texture(uBaked, vec3(p.xz / SIMPLE_BAKE_TILE, 0.0));
#else
    vec3 w = pow(abs(n), vec3(4.0));
    w /= max(w.x + w.y + w.z, 1e-5);
    vec4 c = vec4(0.0);
    if (w.x > 0.01) c += w.x * texture(uBaked, vec3(p.zy / SIMPLE_BAKE_TILE, 0.0));
    if (w.y > 0.01) c += w.y * texture(uBaked, vec3(p.xz / SIMPLE_BAKE_TILE, 1.0));
    if (w.z > 0.01) c += w.z * texture(uBaked, vec3(p.xy / SIMPLE_BAKE_TILE, 2.0));
    return c;
#endif
}
#endif

#ifndef SIMPLE_FULL
// Base colour and finish of a material that is not evaluated (see the header).
void surfaceSimple(SurfaceInput i, inout Surface s) {
    s.albedo = i.matParams[0].rgb;
    s.roughness = 0.5;
#if defined(SIMPLE_MAT_metal)
    s.metallic = 1.0;
    s.roughness = clamp(i.matParams[0].a, 0.15, 1.0);
#elif defined(SIMPLE_MAT_robot_joint)
    s.metallic = 1.0;
    s.roughness = clamp(i.matParams[0].a, 0.2, 1.0);
#elif defined(SIMPLE_MAT_marble)
    s.roughness = 0.25;
    s.albedo = mix(i.matParams[0].rgb, i.matParams[1].rgb, 0.25 * saturate(i.matParams[0].a));
#elif defined(SIMPLE_MAT_wood)
    s.roughness = 0.35;
    s.albedo = mix(i.matParams[0].rgb, i.matParams[1].rgb, 0.45);
#elif defined(SIMPLE_MAT_tapestry)
    s.roughness = 0.9;
    s.albedo = mix(i.matParams[0].rgb, i.matParams[1].rgb, 0.3);
#elif defined(SIMPLE_MAT_ceiling)
    // The painted ceiling: its ivory ground, a touch of the pale blue of its panels.
    s.albedo = mix(i.matParams[0].rgb, i.matParams[1].rgb, 0.2);
    s.roughness = 0.8;
#elif defined(SIMPLE_MAT_cloth)
    s.roughness = 0.85;
#elif defined(SIMPLE_MAT_limestone)
    s.roughness = 0.8;
#elif defined(SIMPLE_MAT_lacquer) || defined(SIMPLE_MAT_robot_porcelain)
    s.roughness = 0.3;
#elif defined(SIMPLE_MAT_standard)
    s.roughness = clamp(i.matParams[0].a, 0.05, 1.0);
    s.emission = i.matParams[2].rgb;
#elif defined(SIMPLE_MAT_glass)
    // A plain pane: tint, ior and roughness of the material, a faint uniform film.
    s.albedo = i.matParams[0].rgb;
    s.roughness = clamp(i.matParams[0].a, 0.02, 1.0);
    s.ior = i.matParams[1].x;
    s.specular = sqrt(sq((s.ior - 1.0) / (s.ior + 1.0)) / 0.16);
    s.alpha = 0.05;
    s.transmission = 0.95;
#elif defined(SIMPLE_MAT_wax)
    s.roughness = 0.5;
    s.emission = i.matParams[2].rgb * 0.5;   // the glow through the top of the candle, averaged
#endif
#if defined(SIMPLE_DECAL) && defined(MARBLE_FRAME) && defined(BOARD_COORDINATES)
    // The inlaid coordinates of the board's border, on the plain marble.
    float inlay = boardCoordinates(i.positionOS, normalize(i.normalOS), i.instParams[0], i.instParams[1], i.instParams[2]);
    s.albedo = mix(s.albedo, i.instParams[1].rgb, inlay);
#endif
#if defined(SIMPLE_DECAL) && defined(ROBOT_MARKING)
    // The word printed on the coach's chest, on the plain glaze.
    float ink = robotMarking(i.positionOS, normalize(i.normalOS), i.matParams[5], i.matParams[6].w, i.matParams[7]);
    s.albedo = mix(s.albedo, i.matParams[6].rgb, ink);
#endif
}
#endif

// Sun visibility: first cascade containing p, normal offset, 4 bilinear compare taps.
float simpleSunShadow(vec3 posWS, vec3 nGeom) {
    int n = int(frame.shadowParams.x);
    int c = -1;
    for (int k = 0; k < n; ++k) {
        vec3 q = (frame.shadowMatrix[k] * vec4(posWS, 1.0)).xyz;
        if (all(greaterThan(q, vec3(0.0))) && all(lessThan(q, vec3(1.0)))) { c = k; break; }
    }
    if (c < 0) return 1.0;
    vec4 sc = lighting.shadowScale[c];
    float texel = sc.w;
    float NoL = dot(nGeom, frame.sunDirection.xyz);
    float sinL = sqrt(clamp(1.0 - NoL * NoL, 0.0, 1.0));
    vec3 p = posWS + nGeom * (texel * (0.5 + 1.5 * sinL));
    vec3 uvz = (frame.shadowMatrix[c] * vec4(p, 1.0)).xyz;
    float zr = uvz.z - (texel * 0.35 + 0.0002) / sc.z;
    vec2 texelUV = texel / sc.xy;
    float s = 0.0;
    for (int k = 0; k < 4; ++k) {
        vec2 o = (vec2(k & 1, k >> 1) - 0.5) * 1.5 * texelUV;
        s += texture(uShadowMap, vec4(uvz.xy + o, float(c), zr));
    }
    return s * 0.25;
}


// Direct light of one source (radiance already in 'radiance'), GGX specular + Lambert diffuse.
vec3 simpleLight(vec3 N, vec3 V, vec3 L, vec3 diffColor, vec3 f0, float a, vec3 radiance) {
    float NoL = dot(N, L);
    if (NoL <= 0.0) return vec3(0.0);
    vec3 H = normalize(L + V);
    float NoV = max(dot(N, V), 1e-4), NoH = saturate(dot(N, H)), VoH = saturate(dot(V, H));
    vec3 spec = D_GGX(NoH, a) * V_SmithGGXCorrelated(NoV, NoL, a) * F_Schlick(f0, VoH);
    return (diffColor * INV_PI + spec) * (NoL * radiance);
}

void main() {
    SurfaceInput i = buildSurfaceInput();
    Surface s = defaultSurface(i);
#ifdef SIMPLE_FULL
    surface(i, s);
#else
    surfaceSimple(i, s);
#ifdef SIMPLE_BAKED
    {
        vec4 bk = bakedSurface(i);
        s.albedo = bk.rgb;
        s.roughness = bk.a;
    }
#endif
#endif
    vec3 N = normalize(s.normalWS);
    vec3 V = i.viewDirWS;
    float rough = clamp(s.roughness, 0.05, 1.0);
    float a = rough * rough;
    vec3 diffColor = s.albedo * (1.0 - s.metallic);
    vec3 f0 = mix(vec3(0.16 * s.specular * s.specular), s.albedo, s.metallic);
#ifdef MATERIAL_TRANSPARENT
    if (s.transmission > 0.0) diffColor = vec3(0.0);   // glass: albedo is the transmittance tint
#endif

    vec3 c = vec3(0.0);
    vec3 Ls = frame.sunDirection.xyz;
    if (dot(i.normalWS, Ls) > 0.0)
        c += simpleLight(N, V, Ls, diffColor, f0, a, frame.sunRadiance.rgb * simpleSunShadow(i.positionWS, i.normalWS));
    int nl = int(frame.passInfo.z);
    for (int k = 0; k < nl; ++k) {
        PointLightData pl = pointLights[k];
        vec3 d = pl.position - i.positionWS;
        float dist2 = dot(d, d);
        float falloff = sq(clamp(1.0 - sq(dist2 / (pl.radius * pl.radius)), 0.0, 1.0)) / max(dist2, 1e-4);
        if (falloff <= 0.0) continue;
        vec3 L = d * inversesqrt(dist2);
        if (pl.spotCosOuter > -1.0) {
            float spot = clamp((dot(-L, pl.direction) - pl.spotCosOuter) / max(pl.spotCosInner - pl.spotCosOuter, 1e-4), 0.0, 1.0);
            falloff *= spot * spot;
        }
        c += simpleLight(N, V, L, diffColor, f0, a, pl.color * (pl.intensity * falloff * frame.exposure.x));
    }
    // Ambient. With the light probes (Options: indirect lighting; mode 1 once they are baked) the
    // light bounced around the hall, diffuse and reflected; else the sky / ground hemisphere.
    float ambientK = lighting.lightingMisc.z;
    vec3 R = reflect(-V, N);
    float NoV = max(dot(N, V), 1e-4);
    vec3 Fa = f0 + (max(vec3(1.0 - rough), f0) - f0) * pow(1.0 - NoV, 5.0);
    vec3 ambientDiffuse, ambientSpecular;
#ifdef SIMPLE_PROBES
    if (int(lighting.probeInfo.w) == 1) {   // baked (until then, the hemisphere)
        ProbeBlend pb = gatherProbes(i.positionWS, N);
        ambientDiffuse = pb.irradiance;
        ambientSpecular = probeSpecular(pb, i.positionWS, R, rough);
    } else
#endif
    {
        ambientDiffuse = hemisphereAmbient(N);
        ambientSpecular = hemisphereAmbient(R) * (1.0 - 0.7 * rough);
    }
    c += (diffColor * ambientDiffuse + Fa * ambientSpecular) * (ambientK * s.occlusion);
    c += s.emission * frame.exposure.x;
    c = min(c, vec3(60000.0));

#ifdef MATERIAL_TRANSPARENT
    if (s.transmission > 0.0) {
        // dst = c + dst * T: the grey average of the coloured transmittance of the full renderer.
        float NoVt = max(abs(dot(s.normalWS, V)), 1e-4);
        float F = F_Schlick(sq((s.ior - 1.0) / (s.ior + 1.0)), 1.0, NoVt);
        vec3 T = clamp(s.transmission, 0.0, 1.0) * (1.0 - F) * (1.0 - F) * clamp(s.albedo, 0.0, 1.0);
        outColor = vec4(c, 1.0 - dot(T, vec3(1.0 / 3.0)));
    } else {
        float al = clamp(s.alpha, 0.0, 1.0);
        outColor = vec4(c * al, al);
    }
#else
    outColor = vec4(c, 1.0);
#endif
#if defined(MATERIAL_SCREEN_DOOR) && !defined(MATERIAL_TRANSPARENT)
    if (screenDoorHidden()) discard;
#endif
}
