// Scene lighting for the forward passes (render-lighting package).
//
// Entry points used by shaders/passes/forward.frag:
//   vec3 shadeSurface(SurfaceInput i, Surface s, float planarLayer)   lit, pre-exposed radiance
//   vec3 transmittanceOf(SurfaceInput i, Surface s)                   transparent: dst multiplier
//
// Features: sun with 3 fitted cascades + PCSS contact-hardening soft shadows (receiver-plane
// depth bias), point/spot lights, clear coat (with base F0 correction), sheen (Charlie + energy
// scaling from the DFG LUT), anisotropic GGX (+ bent reflection vector for IBL), subsurface
// (energy-conserving coloured wrap + transmittance through the thickness read back from the shadow
// map), glass transmission, multiple-scattering GGX energy compensation, light probes (blended L2
// SH irradiance + box-projected GGX-prefiltered specular), planar reflections (on the mirror plane
// only; roughness- and distance-aware Gaussian mip lookup, normal distortion, border fade),
// multi-bounce AO, specular occlusion and horizon fading.
#include "shaders/include/brdf.glsl"
#include "shaders/lighting/lighting_ubo.glsl"

layout(binding = 8) uniform sampler2DArrayShadow uShadowMap;
layout(binding = 9) uniform sampler2DArray uShadowDepth;
layout(binding = 11) uniform samplerCubeArray uSpecularProbes;
layout(binding = 12) uniform sampler2D uAO;
layout(binding = 13) uniform sampler2DArray uPlanar;
#ifdef PASS_MAIN
// Previous frame's filtered screen-space reflections (render-post): rgb radiance, a = confidence.
layout(binding = 15) uniform sampler2D uSSRHistory;
#endif
layout(binding = 14) uniform sampler2D uBrdfLut;

// ------------------------------------------------------------------------------------------------
// Sun shadows

vec2 vogelDisk(int i, int n, float phi) {
    float r = sqrt((float(i) + 0.5) / float(n));
    float th = float(i) * 2.39996323 + phi;
    return r * vec2(cos(th), sin(th));
}

// First cascade containing p; near a cascade border the choice is dithered towards the next one
// (TAA resolves the dither into a smooth transition).
int shadowCascade(vec3 p, float dither) {
    int n = int(frame.shadowParams.x);
    float band = lighting.lightingMisc.y;
    for (int c = 0; c < n; ++c) {
        vec3 q = (frame.shadowMatrix[c] * vec4(p, 1.0)).xyz;
        vec2 e = min(q.xy, 1.0 - q.xy);
        float edge = min(e.x, e.y);
        if (edge > 0.0 && q.z > 0.0 && q.z < 1.0) {
            if (c < n - 1 && edge < band && dither * band > edge) continue;
            return c;
        }
    }
    return -1;
}

struct SunShadowResult {
    float visibility;
    float thickness;   // metres of matter between the point and the sun (-1 = unknown)
};

// Depth gradient of the receiver plane in shadow (uv, depth) space from the geometric normal
// (receiver-plane depth bias: lets wide PCSS kernels run on slopes without self-shadowing).
vec2 shadowDepthGradient(int c, vec3 n) {
    mat4 S = frame.shadowMatrix[c];
    vec3 r0 = vec3(S[0][0], S[1][0], S[2][0]);
    vec3 r1 = vec3(S[0][1], S[1][1], S[2][1]);
    vec3 r2 = vec3(S[0][2], S[1][2], S[2][2]);
    vec3 ny = vec3(dot(r0, n) / dot(r0, r0), dot(r1, n) / dot(r1, r1), dot(r2, n) / dot(r2, r2));
    float nz = abs(ny.z) < 1e-6 ? 1e-6 : ny.z;
    vec2 g = -ny.xy / nz;
    // Clamp grazing slopes (tan > ~6).
    vec2 lim = 6.0 * lighting.shadowScale[c].xy / lighting.shadowScale[c].z;
    return clamp(g, -lim, lim);
}

SunShadowResult evalSunShadow(vec3 posWS, vec3 nGeom, vec2 pixel, float extraBlurM, bool wantThickness) {
    SunShadowResult r;
    r.visibility = 1.0;
    r.thickness = -1.0;
    vec3 L = frame.sunDirection.xyz;
    float dither = ign(pixel);
    int c = shadowCascade(posWS, dither);
    if (c < 0) return r;
#ifdef DBG_NO_SHADOW
    return r;
#endif
    vec4 sc = lighting.shadowScale[c];
    float texel = sc.w;
    float NoL = dot(nGeom, L);
    float sinL = sqrt(clamp(1.0 - NoL * NoL, 0.0, 1.0));
    // Normal offset (grows at grazing angles) + tiny constant depth bias.
    vec3 p = posWS + nGeom * (texel * (0.5 + 1.5 * sinL));
    vec3 uvz = (frame.shadowMatrix[c] * vec4(p, 1.0)).xyz;
    float zr = uvz.z - (texel * 0.35 + 0.0002) / sc.z;
    float layer = float(c);
    if (wantThickness) {
        vec3 q = (frame.shadowMatrix[c] * vec4(posWS - nGeom * texel, 1.0)).xyz;
        float d0 = textureLod(uShadowDepth, vec3(q.xy, layer), 0.0).r;
        r.thickness = max(q.z - d0, 0.0) * sc.z;
    }
    vec2 grad = shadowDepthGradient(c, nGeom);
    vec2 texelUV = texel / sc.xy;
#if defined(PASS_PROBE) || defined(SCACELITH_GLES)
    // Probe capture, and every pass on mobile: small bilinear PCF only (no PCSS blocker search).
    float s4 = 0.0;
    for (int k = 0; k < 4; ++k) {
        vec2 o = (vec2(k & 1, k >> 1) - 0.5) * 1.5 * texelUV;
        s4 += texture(uShadowMap, vec4(uvz.xy + o, layer, zr + dot(o, grad)));
    }
    r.visibility = s4 * 0.25;
    return r;
#else
    float rot = dither * TAU;
    float tanT = lighting.sunParams.y;
#if defined(PASS_PLANAR)
    float filterM = texel * 1.5 + extraBlurM;
    const int NP = 8;
#else
    // PCSS blocker search. Blockers can be anywhere between the receiver and the light-space near
    // plane, so the search region is (distance to near plane) * tan(sun radius), capped.
    float searchM = clamp(zr * sc.z * tanT, texel * 2.0, texel * 40.0);
    vec2 searchUV = searchM / sc.xy;
    const int NB = 12;
    float zSum = 0.0, nBlk = 0.0;
    for (int k = 0; k < NB; ++k) {
        vec2 o = vogelDisk(k, NB, rot) * searchUV;
        vec4 d = textureGather(uShadowDepth, vec3(uvz.xy + o, layer), 0);
        vec4 b = vec4(lessThan(d, vec4(zr + dot(o, grad))));
        zSum += dot(d, b);
        nBlk += b.x + b.y + b.z + b.w;
    }
    if (nBlk < 0.5) return r;                        // fully lit
#ifdef DBG_NO_PCF
    r.visibility = 1.0 - nBlk / float(NB * 4);
    return r;
#endif
    float zb = zSum / nBlk;
    float penumbraM = max(zr - zb, 0.0) * sc.z * tanT;
    if (nBlk > float(NB * 4) - 0.5 && penumbraM <= searchM) {  // umbra
        r.visibility = 0.0;
        return r;
    }
    float filterM = clamp(penumbraM + extraBlurM, texel * 1.25, texel * 48.0);
    // Wide penumbrae (thin far blockers: transoms, mullions) get more taps, contact shadows fewer.
    int NP = filterM > texel * 8.0 ? 32 : 16;
#endif
    vec2 fUV = filterM / sc.xy;
    float rot2 = rot + 2.1;
    float s = 0.0;
    for (int k = 0; k < NP; ++k) {
        vec2 o = vogelDisk(k, NP, rot2) * fUV;
        s += texture(uShadowMap, vec4(uvz.xy + o, layer, zr + dot(o, grad)));
    }
    r.visibility = s / float(NP);
    return r;
#endif
}

// ------------------------------------------------------------------------------------------------
// Shading parameters

vec3 f0Of(Surface s) {
    vec3 dielectric = vec3(0.16 * s.specular * s.specular);
    return mix(dielectric, s.albedo, s.metallic);
}

struct Shading {
    vec3 N, V, Ng, R;
    float NoV;
    vec3 diffColor, f0;
    float rough, alpha;
    vec3 dfg;                 // x = A, y = B (split sum), z = sheen DG
    vec3 energyComp;
    float cc, ccRough, ccAlpha;
    vec3 ccN;
    vec3 sheenColor;
    float sheenRough, sheenScale, sheenDG;
    float aniso, at, ab;
    vec3 T, B;
    float sss, sssRadius;
    vec3 sssColor;
    float transmission;
    float thinThickness;
};

vec3 sampleDFG(float NoV, float rough) { return textureLod(uBrdfLut, vec2(NoV, rough), 0.0).rgb; }

Shading prepareShading(SurfaceInput i, Surface s) {
    Shading sh;
    sh.N = s.normalWS;
    sh.V = i.viewDirWS;
    sh.Ng = i.normalWS;
    sh.NoV = max(dot(sh.N, sh.V), 1e-4);
    sh.transmission = clamp(s.transmission, 0.0, 1.0);
    sh.diffColor = s.albedo * (1.0 - s.metallic) * (1.0 - sh.transmission);
    vec3 f0 = f0Of(s);
    if (sh.transmission > 0.0) f0 = mix(f0, vec3(sq((s.ior - 1.0) / (s.ior + 1.0))), 1.0 - s.metallic);
    sh.cc = clamp(s.clearcoat, 0.0, 1.0);
    sh.ccRough = clamp(s.clearcoatRoughness, 0.02, 1.0);
    sh.ccAlpha = max(sh.ccRough * sh.ccRough, 0.0004);
    sh.ccN = s.clearcoatNormalWS;
    float rough = s.roughness;
    if (sh.cc > 0.0) {
        f0 = mix(f0, f0ClearCoatToSurface(f0), sh.cc);
        rough = mix(rough, max(rough, sh.ccRough), sh.cc);
    }
    sh.f0 = f0;
    sh.rough = rough;
    sh.alpha = max(rough * rough, 0.0004);
    sh.dfg = sampleDFG(sh.NoV, rough);
    sh.energyComp = 1.0 + f0 * (1.0 / max(sh.dfg.x + sh.dfg.y, 1e-3) - 1.0);
    sh.sheenColor = s.sheenColor;
    sh.sheenRough = clamp(s.sheenRoughness, 0.07, 1.0);
    sh.sheenDG = 0.0;
    sh.sheenScale = 1.0;
    if (max(s.sheenColor.r, max(s.sheenColor.g, s.sheenColor.b)) > 0.0) {
        sh.sheenDG = sampleDFG(sh.NoV, sh.sheenRough).z;
        sh.sheenScale = max(1.0 - max(s.sheenColor.r, max(s.sheenColor.g, s.sheenColor.b)) * sh.sheenDG, 0.0);
    }
    sh.aniso = clamp(s.anisotropy, -1.0, 1.0);
    vec3 t = s.anisotropyDirWS - sh.N * dot(sh.N, s.anisotropyDirWS);
    sh.T = dot(t, t) > 1e-8 ? normalize(t) : i.tangentWS;
    sh.B = cross(sh.N, sh.T);
    sh.at = max(sh.alpha * (1.0 + sh.aniso), 0.0004);
    sh.ab = max(sh.alpha * (1.0 - sh.aniso), 0.0004);
    vec3 Nr = sh.N;
    if (abs(sh.aniso) > 1e-3) {
        // Bent reflection normal for anisotropic IBL (Filament / McAuley).
        vec3 aDir = sh.aniso >= 0.0 ? sh.B : sh.T;
        vec3 aT = cross(aDir, sh.V);
        vec3 aN = cross(aT, aDir);
        float bend = abs(sh.aniso) * clamp(5.0 * rough, 0.0, 1.0);
        Nr = normalize(mix(sh.N, aN, bend));
    }
    sh.R = reflect(-sh.V, Nr);
    sh.sss = clamp(s.subsurface, 0.0, 1.0);
    sh.sssColor = s.subsurfaceColor;
    sh.sssRadius = max(s.subsurfaceRadius, 1e-4);
    sh.thinThickness = s.thickness;
    return sh;
}

// ------------------------------------------------------------------------------------------------
// Direct lighting. Returns reflected radiance for unit light radiance; 'visibility' is applied
// to everything except the subsurface transmittance (which already accounts for occluders via
// the shadow-map thickness).
vec3 evalLight(Shading sh, vec3 L, float visibility, float thicknessM, float srcSize) {
    vec3 N = sh.N, V = sh.V;
    float NoL = dot(N, L);
    vec3 H = normalize(V + L);
    float NoH = clamp(dot(N, H), 0.0, 1.0), LoH = clamp(dot(L, H), 0.0, 1.0);
    float NoLc = clamp(NoL, 0.0, 1.0);
    vec3 color = vec3(0.0);
    if (NoL > -0.5 || sh.sss > 0.0) {
        // Specular GGX. The isotropic lobe (and the clear coat below) is widened for spherical
        // lights (Karis 2013); the anisotropic lobe is not.
        float a = clamp(sh.alpha + srcSize, 0.0004, 1.0);
        float norm = sq(sh.alpha / a);
        float D, Vis;
        if (abs(sh.aniso) > 1e-3) {
            float ToH = dot(sh.T, H), BoH = dot(sh.B, H);
            float ToV = dot(sh.T, V), BoV = dot(sh.B, V), ToL = dot(sh.T, L), BoL = dot(sh.B, L);
            D = D_GGX_Aniso(sh.at, sh.ab, ToH, BoH, NoH);
            Vis = V_SmithGGXCorrelated_Aniso(sh.at, sh.ab, ToV, BoV, ToL, BoL, sh.NoV, NoLc);
        } else {
            D = D_GGX(NoH, a) * norm;
            Vis = V_SmithGGXCorrelated(sh.NoV, NoLc, a);
        }
        vec3 Fr = D * Vis * F_Schlick(sh.f0, LoH) * sh.energyComp;
        // Diffuse: Burley, or an energy-conserving coloured wrap for subsurface materials
        // (light bleeding past the terminator, red travelling furthest).
        vec3 Fd = sh.diffColor * Fd_Burley(sh.NoV, NoLc, LoH, sh.rough) * NoLc;
        if (sh.sss > 0.0) {
            vec3 w = sh.sss * (0.2 + 0.5 * sh.sssColor);
            vec3 wrapNoL = clamp((vec3(NoL) + w) / sq(1.0 + w), 0.0, 1.0);
            Fd = mix(Fd, sh.diffColor * INV_PI * wrapNoL, sh.sss);
        }
        color = (Fd + Fr * NoLc) * sh.sheenScale;
        if (sh.sheenDG > 0.0) color += sh.sheenColor * (D_Charlie(sh.sheenRough, NoH) * V_Neubelt(sh.NoV, NoLc) * NoLc);
        if (sh.cc > 0.0) {
            float NoHc = clamp(dot(sh.ccN, H), 0.0, 1.0), NoLcc = clamp(dot(sh.ccN, L), 0.0, 1.0);
            float ac = clamp(sh.ccAlpha + srcSize, 0.0004, 1.0);
            float Fc = F_Schlick(0.04, 1.0, LoH) * sh.cc;
            float coat = D_GGX(NoHc, ac) * sq(sh.ccAlpha / ac) * V_Kelemen(LoH) * Fc;
            color = color * (1.0 - Fc) + vec3(coat * NoLcc);
        }
        color *= visibility;
    }
    // Translucency: light entering on the lit side and leaving here, attenuated by the matter in
    // between (Beer-Lambert per channel: subsurfaceColor sets the relative mean free path).
    if (sh.sss > 0.0 && thicknessM >= 0.0) {
        vec3 mfp = sh.sssRadius * (0.25 + 1.5 * sh.sssColor);
        vec3 T = exp(-thicknessM / mfp);
        float back = clamp(0.2 - 0.8 * NoL, 0.0, 1.0);
        float fwd = 0.35 + 0.65 * pow(clamp(dot(V, -L), 0.0, 1.0), 4.0);
        color += sh.diffColor * T * (back * fwd * INV_PI * sh.sss);
    }
    // Thin translucent surfaces (cloth, lampshades, porcelain rims).
    if (sh.thinThickness > 0.0) {
        float back = clamp(-NoL, 0.0, 1.0);
        color += sh.diffColor * sh.sssColor * (back * INV_PI * exp(-sh.thinThickness * 40.0) * max(sh.sss, 0.25) * visibility);
    }
    return color;
}

// ------------------------------------------------------------------------------------------------
// Indirect lighting

vec3 hemisphereAmbient(vec3 n) {
    float t = n.y * 0.5 + 0.5;
    return mix(frame.ambientGround.rgb, frame.ambientSky.rgb, t);
}

struct ProbeBlend {
    vec3 irradiance;   // irradiance / PI at the shading normal (pre-exposed, current exposure)
    int i0, i1;        // two strongest probes for specular
    float w0, w1, w2;  // their weights and the third strongest (subtracted: continuous switch)
};

void probeAccumulate(inout ProbeBlend pb, int k, float w, vec3 n) {
    if (w <= 1e-4) return;
    pb.irradiance += w * max(shEvalProbe(k * 9, n), vec3(0.0));
    if (w > pb.w0) {
        pb.w2 = pb.w1;
        pb.i1 = pb.i0;
        pb.w1 = pb.w0;
        pb.i0 = k;
        pb.w0 = w;
    } else if (w > pb.w1) {
        pb.w2 = pb.w1;
        pb.i1 = k;
        pb.w1 = w;
    } else {
        pb.w2 = max(pb.w2, w);
    }
}

ProbeBlend gatherProbes(vec3 p, vec3 n) {
    ProbeBlend pb;
    pb.irradiance = vec3(0.0);
    pb.i0 = pb.i1 = -1;
    pb.w0 = pb.w1 = pb.w2 = 0.0;
    int count = int(lighting.probeInfo.x);
    // Local (priority) probes take their share first ...
    float remaining = 1.0;
    for (int k = 0; k < count; ++k) {
        if (lighting.probeBoxMin[k].w < 0.5) continue;
        float d = distance(p, lighting.probePos[k].xyz);
        float f = 1.0 - smoothstep(lighting.probeBoxMax[k].w, lighting.probePos[k].w, d);
        float w = f * remaining;
        remaining -= w;
        probeAccumulate(pb, k, w, n);
    }
    // ... the grid probes share the rest: normalised smooth radial kernels plus a tiny
    // inverse-distance (Shepard) term so the blend stays continuous where no kernel reaches.
    if (remaining > 1e-3) {
        float wk[MAX_LIGHT_PROBES];
        float sum = 0.0;
        for (int k = 0; k < count; ++k) {
            wk[k] = 0.0;
            if (lighting.probeBoxMin[k].w >= 0.5) continue;
            float d = distance(p, lighting.probePos[k].xyz);
            float f = clamp(1.0 - d / lighting.probePos[k].w, 0.0, 1.0);
            wk[k] = f * f + 1e-4 / (1.0 + d * d * d * d);
            sum += wk[k];
        }
        if (sum > 0.0) {
            float s = remaining / sum;
            for (int k = 0; k < count; ++k) probeAccumulate(pb, k, wk[k] * s, n);
        }
    }
    pb.irradiance *= lighting.probeInfo.z;
    return pb;
}

vec3 sampleProbe(int k, vec3 p, vec3 R, float rough, float lod) {
    vec3 bmin = lighting.probeBoxMin[k].xyz, bmax = lighting.probeBoxMax[k].xyz;
    vec3 dir = R;
    if (all(greaterThan(p, bmin)) && all(lessThan(p, bmax))) {
        // Parallax correction against the probe's proxy box (Lagarde 2012).
        vec3 t1 = (bmax - p) / R, t2 = (bmin - p) / R;
        vec3 tf = max(t1, t2);
        float t = min(min(tf.x, tf.y), tf.z);
        vec3 corrected = normalize(p + R * t - lighting.probePos[k].xyz);
        dir = normalize(mix(corrected, R, rough * rough));
    }
    return textureLod(uSpecularProbes, vec4(dir, float(k)), lod).rgb;
}

vec3 probeSpecular(ProbeBlend pb, vec3 p, vec3 R, float rough) {
    float lod = rough * lighting.probeInfo.y;
    vec3 c = vec3(0.0);
    // Weights relative to the third strongest probe: a probe enters / leaves the top two with
    // zero weight, so the selection never pops.
    float a = pb.w0 - pb.w2 + 1e-5, b = max(pb.w1 - pb.w2, 0.0);
    if (pb.i0 >= 0) c += a * sampleProbe(pb.i0, p, R, rough, lod);
    if (pb.i1 >= 0) c += b * sampleProbe(pb.i1, p, R, rough, lod);
    return c / (a + b) * lighting.probeInfo.z;
}

// Planar reflection lookup. Returns rgb radiance and a = confidence (0 = use probes).
// Ng is the geometric normal, N the shading normal (its tilt distorts the lookup).
vec4 samplePlanarReflection(int layer, vec3 posWS, vec3 Ng, vec3 N, vec3 V, float rough) {
    if (lighting.planarInfo[layer].x < 0.5) return vec4(0.0);
    vec4 plane = frame.planarPlanes[layer];
    vec3 pn = plane.xyz;
    // The reflection is only valid on surfaces that lie on the mirror plane, facing like it. A
    // reflector material also covers mouldings, bevels, chamfers and sides (the table top's rounded
    // edge and underside rim, the board frame): there the lookup would fetch unrelated mirrored
    // content, or the cleared texels outside the rendered region, and a clear coat's grazing
    // Fresnel would show it at full strength, like a hole in the edge. They use the probes. Full
    // weight within ~4 degrees and 2 mm of the plane, none beyond ~10 degrees or 6 mm.
    float offPlane = abs(dot(pn, posWS) + plane.w);
    float onPlane = smoothstep(0.985, 0.998, dot(Ng, pn)) * (1.0 - smoothstep(0.002, 0.006, offPlane));
    if (onPlane <= 0.0) return vec4(0.0);
    vec4 clip0 = frame.planarViewProj[layer] * vec4(posWS, 1.0);
    vec2 uv0 = clip0.xy / clip0.w * 0.5 + 0.5;
    // Distance (m) from the plane to what is reflected here (alpha of the reflection, level 0).
    float hit = textureLod(uPlanar, vec3(uv0, float(layer)), 0.0).a;
    if (hit <= 0.0) return vec4(0.0);
    float NoV = max(dot(pn, V), 0.05);
    float path = min(hit, 60.0) / NoV;
    // Normal-map distortion: a tilted micro-normal deflects the reflected ray by ~2x the tilt.
    vec3 nt = N - pn * dot(N, pn);
    vec3 q = posWS + nt * (2.0 * min(path, 4.0));
    vec4 clip = frame.planarViewProj[layer] * vec4(q, 1.0);
    vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
    // Texels the reflection pass did not render (outside its scissor rectangle) are cleared to zero,
    // distance included. When the distorted lookup lands there, keep the undistorted one: the
    // reflector's bounds keep it inside the rendered region (its distance was read above).
    if (textureLod(uPlanar, vec3(uv, float(layer)), 0.0).a <= 0.0) uv = uv0;
    // Roughness-aware blur: reflected GGX lobe footprint at the hit, in reflection-texture pixels.
    float viewDist = distance(frame.cameraPos.xyz, posWS);
    float coneTan = rough * rough * 0.9;
    float footprint = path * coneTan;
    float pxPerM = lighting.planarInfo[layer].w * frame.proj[1][1] * 0.5 / max(viewDist + path, 0.05);
    float lod = clamp(log2(max(footprint * pxPerM, 1e-3)) + 0.5, 0.0, lighting.planarInfo[layer].y);
    vec3 col = textureLod(uPlanar, vec3(uv, float(layer)), lod).rgb;
    vec2 e = min(uv, 1.0 - uv);
    float fade = clamp(min(e.x, e.y) / 0.06, 0.0, 1.0);
    return vec4(col, fade * fade * (3.0 - 2.0 * fade) * onPlane);
}

// Pre-exposed specular radiance arriving along R (planar reflection or probes).
vec3 indirectSpecular(ProbeBlend pb, SurfaceInput i, vec3 N, vec3 R, float rough, float planarLayer, int mode) {
    vec3 probe;
    if (mode == 1) probe = probeSpecular(pb, i.positionWS, R, rough);
    else if (mode == 0) probe = hemisphereAmbient(R);
    else probe = vec3(0.0);
#if !defined(PASS_PLANAR) && !defined(PASS_PROBE)
    if (planarLayer >= 0.0 && frame.passInfo.w > planarLayer) {
        vec4 pl = samplePlanarReflection(int(planarLayer), i.positionWS, i.normalWS, N, i.viewDirWS, rough);
        probe = mix(probe, pl.rgb, pl.a);
    }
#endif
#if defined(PASS_MAIN) && !defined(MATERIAL_TRANSPARENT)
    // Screen-space reflections replace the probe where they found a hit (planar reflectors
    // already have exact reflections). History is reprojected with last frame's camera.
    // Transparent surfaces are not in the prepass: the SSR at their pixels belongs to the opaque surface behind them.
    if (planarLayer < 0.0 || frame.passInfo.w <= planarLayer) {
        vec4 pc = frame.prevViewProj * vec4(i.positionWS, 1.0);
        vec2 uvPrev = pc.xy / max(pc.w, 1e-6) * 0.5 + 0.5;
        if (all(greaterThan(uvPrev, vec2(0.0))) && all(lessThan(uvPrev, vec2(1.0)))) {
            vec4 ssr = textureLod(uSSRHistory, uvPrev, 0.0);
            probe = mix(probe, ssr.rgb, clamp(ssr.a, 0.0, 1.0));
        }
    }
#endif
    return probe;
}

// ------------------------------------------------------------------------------------------------

vec3 shadeSurface(SurfaceInput i, Surface s, float planarLayer) {
    Shading sh = prepareShading(i, s);
    vec3 color = vec3(0.0);

    // Sun
    vec3 Ls = frame.sunDirection.xyz;
    float NoLg = dot(i.normalWS, Ls);
    if (NoLg > -0.35 || sh.sss > 0.0 || sh.thinThickness > 0.0) {
        float blur = sh.sss * sh.sssRadius * 0.5;
        SunShadowResult sr = evalSunShadow(i.positionWS, i.normalWS, i.pixel, blur, sh.sss > 0.0);
        color += evalLight(sh, Ls, sr.visibility, sr.thickness, 0.0) * frame.sunRadiance.rgb;
    }

    // Point and spot lights
    int nl = int(frame.passInfo.z);
#ifdef DBG_NO_LIGHTS
    nl = 0;
#endif
    for (int k = 0; k < nl; ++k) {
        PointLightData pl = pointLights[k];
        vec3 d = pl.position - i.positionWS;
        float dist2 = dot(d, d);
        float falloff = sq(clamp(1.0 - sq(dist2 / (pl.radius * pl.radius)), 0.0, 1.0)) / max(dist2, 1e-4);
        if (falloff <= 0.0) continue;
        vec3 L = d * inversesqrt(dist2);
        if (pl.spotCosOuter > -1.0) {
            float cd = dot(-L, pl.direction);
            float spot = clamp((cd - pl.spotCosOuter) / max(pl.spotCosInner - pl.spotCosOuter, 1e-4), 0.0, 1.0);
            falloff *= spot * spot;
            if (falloff <= 0.0) continue;
        }
        float srcSize = clamp(pl.sourceRadius * inversesqrt(dist2) * 0.5, 0.0, 1.0);
        color += evalLight(sh, L, 1.0, sh.thinThickness > 0.0 ? -1.0 : sh.sssRadius * 3.0, srcSize) *
                 (pl.color * pl.intensity * falloff * frame.exposure.x);
    }

    // Ambient occlusion
    float ao = s.occlusion;
#if defined(PASS_MAIN) && !defined(MATERIAL_TRANSPARENT)
    ao *= texture(uAO, i.screenUV).r;  // of the prepass, which has no transparent surface
#endif

    // Diffuse + specular image based lighting
    int mode = int(lighting.probeInfo.w);
#ifdef DBG_NO_PROBES
    mode = 0;
#endif
    ProbeBlend pb;
    pb.irradiance = vec3(0.0);
    pb.i0 = pb.i1 = -1;
    pb.w0 = pb.w1 = pb.w2 = 0.0;
    if (mode == 1) pb = gatherProbes(i.positionWS, sh.N);
    else if (mode == 0) pb.irradiance = hemisphereAmbient(sh.N);
    float ambientK = lighting.lightingMisc.z;
    vec3 diffuseIBL = sh.diffColor * pb.irradiance * gtaoMultiBounce(ao, sh.diffColor) * ambientK;
    float specOcc = specularOcclusion(sh.NoV, ao, sh.rough);
    float horizon = min(1.0 + dot(sh.R, i.normalWS), 1.0);
    specOcc *= horizon * horizon;
    vec3 E = sh.f0 * sh.dfg.x + sh.dfg.y;
    vec3 specIBL = indirectSpecular(pb, i, sh.N, sh.R, sh.rough, planarLayer, mode) * E * sh.energyComp * specOcc * ambientK;
    vec3 ibl = (diffuseIBL + specIBL) * sh.sheenScale;
    if (sh.sheenDG > 0.0) {
        vec3 Rs = reflect(-sh.V, sh.N);
        ibl += sh.sheenColor * sh.sheenDG * indirectSpecular(pb, i, sh.N, Rs, sh.sheenRough, -1.0, mode) * ao * ambientK;
    }
    if (sh.cc > 0.0) {
        float NoVc = max(dot(sh.ccN, sh.V), 1e-4);
        float Fc = F_Schlick(0.04, 1.0, NoVc) * sh.cc;
        vec3 Rc = reflect(-sh.V, sh.ccN);
        float occC = specularOcclusion(NoVc, ao, sh.ccRough);
        float horC = min(1.0 + dot(Rc, i.normalWS), 1.0);
        ibl = ibl * (1.0 - Fc) + indirectSpecular(pb, i, sh.ccN, Rc, sh.ccRough, planarLayer, mode) * (Fc * occC * horC * horC * ambientK);
    }
    color += ibl;
    color += s.emission * frame.exposure.x;
    return color;
}

// Transparent surfaces with transmission > 0: multiplier applied to what is behind (dual-source
// blending), two interfaces of a thin pane.
vec3 transmittanceOf(SurfaceInput i, Surface s) {
    float NoV = max(abs(dot(s.normalWS, i.viewDirWS)), 1e-4);
    float f0 = sq((s.ior - 1.0) / (s.ior + 1.0));
    float F = F_Schlick(f0, 1.0, NoV);
    vec3 tint = clamp(s.albedo, 0.0, 1.0);   // glass contract: albedo = transmittance tint
    return clamp(s.transmission, 0.0, 1.0) * (1.0 - F) * (1.0 - F) * tint;
}
