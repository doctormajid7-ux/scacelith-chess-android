// Forward shading pass (main view, planar reflections, probe capture).
#include "shaders/include/common.glsl"
#include "shaders/include/surface.glsl"
#include "shaders/include/noise.glsl"
#pragma material
#include "shaders/include/lighting.glsl"
#include "shaders/passes/fragment_input.glsl"

#if defined(MATERIAL_SCREEN_DOOR) && defined(PASS_MAIN) && !defined(MATERIAL_TRANSPARENT)
// The opaque main pass does not write depth, so the screen-door discard below does not need late
// depth tests: keep hidden fragments from being shaded.
layout(early_fragment_tests) in;
#endif

#ifdef MATERIAL_TRANSPARENT
// Dual-source blending: dst = src0 + dst * src1 (src1 = coloured transmittance).
layout(location = 0, index = 0) out vec4 outColor;
layout(location = 0, index = 1) out vec4 outTransmittance;
#else
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outNormalRough;
layout(location = 2) out vec4 outSpecular;
layout(location = 3) out vec2 outVelocity;
#endif

// Geometric specular anti-aliasing (Kaplanyan & Tokuyoshi 2019, Filament variant): widen the lobe
// by the screen-space variance of the normal.
float specularAA(vec3 n, float perceptualRoughness) {
    float strength = lighting.lightingMisc.x;
    if (strength <= 0.0) return perceptualRoughness;
    vec3 du = dFdx(n), dv = dFdy(n);
    float variance = 0.15 * strength * (dot(du, du) + dot(dv, dv));
    float a = perceptualRoughness * perceptualRoughness;
    float kernel = min(2.0 * variance, 0.18);
    return sqrt(sqrt(clamp(a * a + kernel, 0.0, 1.0)));
}

void main() {
    SurfaceInput i = buildSurfaceInput();
#if defined(DBG_FLAT) && defined(PASS_MAIN) && !defined(MATERIAL_TRANSPARENT)
    outColor = vec4(0.5, 0.5, 0.5, 1.0);
    outNormalRough = vec4(i.normalWS, 0.5);
    outSpecular = vec4(0.04, 0.04, 0.04, 0.0);
    outVelocity = vec2(0.0);
    return;
#endif
    Surface s = defaultSurface(i);
#if defined(DBG_FLAT_AT) && DBG_FLAT_AT == 1 && defined(PASS_MAIN) && !defined(MATERIAL_TRANSPARENT)
    outColor = vec4(vec3(0.5), 1.0); outNormalRough = vec4(i.normalWS, 0.5); outSpecular = vec4(0.04); outVelocity = vec2(0.0);
    return;
#endif

#ifndef DBG_NO_SURFACE
    surface(i, s);
#endif
#ifdef MATERIAL_ALPHA_TEST
    if (s.alpha < 0.5) discard;
#endif
    s.normalWS = normalize(s.normalWS);
    s.clearcoatNormalWS = normalize(s.clearcoatNormalWS);
    s.roughness = clamp(s.roughness, 0.02, 1.0);
#if defined(DBG_FLAT_AT) && DBG_FLAT_AT == 2 && defined(PASS_MAIN) && !defined(MATERIAL_TRANSPARENT)
    outColor = vec4(s.albedo, 1.0); outNormalRough = vec4(i.normalWS, 0.5); outSpecular = vec4(0.04); outVelocity = vec2(0.0);
    return;
#endif

#if defined(PASS_MAIN) && !defined(DBG_NO_SPECAA)
    s.roughness = specularAA(i.normalWS, s.roughness);
    s.clearcoatRoughness = specularAA(i.normalWS, clamp(s.clearcoatRoughness, 0.02, 1.0));
#endif
    float planarLayer = draws[DRAW_INDEX_FS].info.y;
#if defined(DBG_FLAT_AT) && DBG_FLAT_AT == 3 && defined(PASS_MAIN) && !defined(MATERIAL_TRANSPARENT)
    outColor = vec4(s.albedo * (planarLayer + 2.0), 1.0); outNormalRough = vec4(i.normalWS, 0.5); outSpecular = vec4(0.04); outVelocity = vec2(0.0);
    return;
#endif

#ifdef DBG_NO_SHADE
    vec3 c = s.albedo;
#else
    vec3 c = shadeSurface(i, s, planarLayer);
#endif
#if (defined(PASS_MAIN) || defined(PASS_PLANAR)) && !defined(MATERIAL_TRANSPARENT)
    {
        // Designation highlight (DrawItem::highlight), as if a cool light picked the object out.
        // 1. On a pale surface the lit colour is filtered towards the highlight's hue (red and
        //    green held back), over the whole object and more towards the silhouette (Fresnel on
        //    the interpolated normal, so polish or bump detail does not speckle it): a white
        //    piece turns a cool, bluish white that stands apart from its cream neighbours. Not on
        //    a dark one: its look is mostly reflections, which would turn it into a blue piece.
        //    On a turned piece the grazing band is thin on screen (NoV < 0.5 is the outer 13% of
        //    a column's half width), so the edge term reaches in to NoV ~0.85.
        // 2. Near the silhouette the colour moves to the hue itself, at a luminance that follows
        //    the surface's (a little brighter on a dark one) but stays under the tonemapper's
        //    shoulder on a bright one, whose highlight desaturation would erase a blue-white:
        //    a clear cobalt edge on sunlit white, a soft blue sheen on black, no neon line.
        // 3. A thin absolute rim on the silhouette keeps it visible on black; a faint lift.
        // Breathes slowly.
        vec4 hl = draws[DRAW_INDEX_FS].highlight;
        if (hl.a > 0.0) {  // uniform per draw
            float NoV = saturate(dot(i.normalWS, i.viewDirWS));
            float k = hl.a * (0.80 + 0.20 * sin(i.time * 3.4));
            vec3 hue = hl.rgb / max(luminance(hl.rgb), 1e-4);
            vec3 filt = hue / max(max(hue.r, hue.g), hue.b);
            float lum = luminance(c);
            float edge = smoothstep(0.85, 0.1, NoV);
            float pale = smoothstep(0.08, 0.45, luminance(s.albedo));
            c *= mix(vec3(1.0), filt, k * pale * (0.35 + 0.45 * edge));
            float tone = min(1.3 * lum + 0.01, 0.25 + 0.15 * lum);
            c = mix(c, hue * tone, k * 0.7 * edge * sqrt(edge));
            c += hl.rgb * (0.18 * k * pow(1.0 - NoV, 3.5));
            c *= 1.0 + 0.08 * k;
        }
    }
#endif
    c = min(c, vec3(60000.0));
#if defined(DBG_FLAT_AT) && DBG_FLAT_AT == 4 && defined(PASS_MAIN) && !defined(MATERIAL_TRANSPARENT)
    outColor = vec4(c, 1.0); outNormalRough = vec4(i.normalWS, 0.5); outSpecular = vec4(0.04); outVelocity = vec2(0.0);
    return;
#endif

#ifdef MATERIAL_TRANSPARENT
    if (s.transmission > 0.0) {
        // Glass contract: the specular reflection is never scaled by alpha; the diffuse
        // (scattering) part is already weighted by 1 - transmission, and the background is
        // multiplied by transmission * (1 - F)^2 * albedo tint.
        outColor = vec4(c, 1.0);
        outTransmittance = vec4(transmittanceOf(i, s), 1.0);
    } else {
        // Plain coverage blending (fades, decals).
        float a = clamp(s.alpha, 0.0, 1.0);
        outColor = vec4(c * a, a);
        outTransmittance = vec4(vec3(1.0 - a), 1.0);
    }
#else
#ifdef PASS_PLANAR
    // Planar reflections keep the distance to the mirror plane in alpha (roughness-aware blur).
    outColor = vec4(c, max(dot(i.positionWS, frame.clipPlane.xyz) + frame.clipPlane.w, 1e-3));
#else
    outColor = vec4(c, 1.0);
#endif
    float rough = s.clearcoat > 0.5 ? min(s.roughness, s.clearcoatRoughness) : s.roughness;
    vec3 nOut = s.clearcoat > 0.5 ? s.clearcoatNormalWS : s.normalWS;
    outNormalRough = vec4(nOut, rough);
    vec3 f0 = mix(vec3(0.16 * s.specular * s.specular), s.albedo, s.metallic);
    if (s.clearcoat > 0.5) f0 = max(f0, vec3(0.04 * s.clearcoat));
    // a = SSR mask (trace, resolve, and combine.comp's optional composite): a reflector's pixels
    // are left out only while its planar reflection is active this frame. shadeSurface never
    // blends SSR into a reflector's material, active or not, so with the default forward
    // composite the traces of an inactive reflector (planar off, camera behind the plane, plane
    // off screen) go unused.
    bool planar = planarLayer >= 0.0 && frame.passInfo.w > planarLayer && lighting.planarInfo[int(planarLayer)].x > 0.5;
    outSpecular = vec4(f0, planar ? 0.0 : (rough < 0.6 ? 1.0 : 0.0));
#ifdef DBG_NO_VELOCITY
    outVelocity = vec2(0.0);
#else
    outVelocity = motionVector();
#endif
#endif
#if defined(MATERIAL_SCREEN_DOOR) && !defined(MATERIAL_TRANSPARENT) && (defined(PASS_MAIN) || defined(PASS_PLANAR))
    // Same pixels as the prepass. Last, so the derivatives above still see whole quads.
    if (screenDoorHidden()) discard;
#endif
}
