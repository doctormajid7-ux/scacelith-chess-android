// Shared helpers of the Scacelith material library (render-materials package).
// Included by the surface files in shaders/materials/*.glsl (after common/surface/noise.glsl).
//
//  * Analytic anti-aliasing of thin procedural features (veins, joints, threads): box-filtered
//    coverage of a band of a field against the pixel footprint of that field.
//  * Periodic gradient noise (tileable bakes) that is also used, with a huge period, by the
//    fragment shaders so baked and live-evaluated patterns share one implementation.
//  * Object-space helpers: draw matrices of the current draw (uDraw) for normals built in OS.
//  * Pass-dependent detail: planar-reflection and probe passes compile with MAT_LOW_DETAIL.

#if defined(PASS_PLANAR) || defined(PASS_PROBE)
#define MAT_LOW_DETAIL 1
#endif

const vec3 MAT_NO_PERIOD = vec3(1048576.0);

// ---------------------------------------------------------------------------------------------
// Hashes
float mat_hash1(float x) { return hash11(x * 1.3183 + 0.123); }
vec2 mat_hash2(vec2 p) {
    uvec2 q = uvec2(ivec2(floor(p))) * uvec2(1597334673u, 3812015801u);
    uint n = (q.x ^ q.y) * 1597334673u;
    return vec2(uvec2(n, n * 16807u)) * (1.0 / 4294967295.0);
}
vec4 mat_hash4(float seed) {
    uint s = hashU(floatBitsToUint(seed) + 0x9e3779b9u);
    uvec4 q = uvec4(s, hashU(s + 1u), hashU(s + 2u), hashU(s + 3u));
    return vec4(q) * (1.0 / 4294967295.0);
}

// ---------------------------------------------------------------------------------------------
// Periodic quintic gradient noise, ~[-1,1]. 'per' = lattice period (cells) per axis.
float mat_gnoise(vec3 p, vec3 per) {
#ifdef DBG_NO_NOISE
    return 0.0;
#endif
    vec3 i = floor(p), f = fract(p);
    vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    vec3 i0 = mod(i, per), i1 = mod(i + 1.0, per);
    float n000 = dot(hash33(vec3(i0.x, i0.y, i0.z)) * 2.0 - 1.0, f - vec3(0, 0, 0));
    float n100 = dot(hash33(vec3(i1.x, i0.y, i0.z)) * 2.0 - 1.0, f - vec3(1, 0, 0));
    float n010 = dot(hash33(vec3(i0.x, i1.y, i0.z)) * 2.0 - 1.0, f - vec3(0, 1, 0));
    float n110 = dot(hash33(vec3(i1.x, i1.y, i0.z)) * 2.0 - 1.0, f - vec3(1, 1, 0));
    float n001 = dot(hash33(vec3(i0.x, i0.y, i1.z)) * 2.0 - 1.0, f - vec3(0, 0, 1));
    float n101 = dot(hash33(vec3(i1.x, i0.y, i1.z)) * 2.0 - 1.0, f - vec3(1, 0, 1));
    float n011 = dot(hash33(vec3(i0.x, i1.y, i1.z)) * 2.0 - 1.0, f - vec3(0, 1, 1));
    float n111 = dot(hash33(vec3(i1.x, i1.y, i1.z)) * 2.0 - 1.0, f - vec3(1, 1, 1));
    return 1.6 * mix(mix(mix(n000, n100, u.x), mix(n010, n110, u.x), u.y), mix(mix(n001, n101, u.x), mix(n011, n111, u.x), u.y), u.z);
}
float mat_gnoise(vec3 p) { return mat_gnoise(p, MAT_NO_PERIOD); }

// fBm with exact lacunarity 2 and integer octave offsets so it stays periodic with 'per'.
float mat_fbm(vec3 p, vec3 per, int oct, float gain) {
    float a = 1.0, s = 0.0, n = 0.0;
    for (int k = 0; k < oct; ++k) {
        s += a * mat_gnoise(p, per);
        n += a;
        p = p * 2.0 + vec3(17.0, 59.0, 31.0);
        per *= 2.0;
        a *= gain;
    }
    return s / n;
}
float mat_fbm(vec3 p, int oct) { return mat_fbm(p, MAT_NO_PERIOD, oct, 0.5); }

// Value-ish cell hash with wobbly borders: cheap irregular "grains" (crystals, fibres).
float mat_grainId(vec3 p) { return hash13(floor(p + 0.35 * vec3(mat_gnoise(p * 0.5), mat_gnoise(p * 0.5 + 7.0), 0.0))); }

// ---------------------------------------------------------------------------------------------
// Anti-aliasing
// Box-filtered coverage of the band |d| < w, where d is a (locally linear) field and fw its
// footprint over one pixel. Energy preserving: thin bands fade to their true average.
float mat_band(float d, float w, float fw) {
    fw = max(fw, 1e-6);
    float a = max(d - 0.5 * fw, -w), b = min(d + 0.5 * fw, w);
    return saturate((b - a) / fw);
}
// Smooth band with soft edges (feathered veins): average of a hard band and a wider soft one.
float mat_softBand(float d, float w, float soft, float fw) {
    float hard = mat_band(d, w, fw);
    float s = 1.0 - smoothstep(w, w + soft + fw, abs(d));
    return max(hard, s * 0.999);
}
// 0 when a feature of 'size' is well resolved, 1 when it is far below the pixel footprint.
float mat_subpixel(float size, float footprint) { return smoothstep(0.35, 1.2, footprint / max(size, 1e-9)); }

#ifndef MAT_BAKE
// Pixel footprint (world units) of a position.
float mat_footprint(vec3 p) { return max(length(dFdx(p)), length(dFdy(p))) + 1e-7; }
#endif

// ---------------------------------------------------------------------------------------------
// Geometry helpers
#ifndef MAT_BAKE
mat3 mat_modelRot() { return mat3(draws[uDraw].model); }
vec3 mat_normalToWorld(vec3 nOS) { return normalize(mat3(draws[uDraw].normalMatrix) * nOS); }
// Screen-space curvature estimate (>0 convex, <0 concave), in 1/m.
float mat_curvature(vec3 n, vec3 p) {
    vec3 dnx = dFdx(n), dny = dFdy(n), dpx = dFdx(p), dpy = dFdy(p);
    float kx = dot(dnx, dpx) / max(dot(dpx, dpx), 1e-12);
    float ky = dot(dny, dpy) / max(dot(dpy, dpy), 1e-12);
    return 0.5 * (kx + ky);
}
#endif
mat2 mat_rot2(float a) { float c = cos(a), s = sin(a); return mat2(c, s, -s, c); }
// Random rotation from a seed in [0,1) (axis-angle, uniform-ish).
mat3 mat_randomRotation(float seed) {
    vec4 h = mat_hash4(seed);
    float z = h.x * 2.0 - 1.0, a = h.y * TAU;
    vec3 axis = vec3(sqrt(1.0 - z * z) * vec2(cos(a), sin(a)), z);
    float ang = h.z * TAU, c = cos(ang), s = sin(ang), t = 1.0 - c;
    vec3 k = axis;
    return mat3(t * k.x * k.x + c, t * k.x * k.y + s * k.z, t * k.x * k.z - s * k.y,
                t * k.x * k.y - s * k.z, t * k.y * k.y + c, t * k.y * k.z + s * k.x,
                t * k.x * k.z + s * k.y, t * k.y * k.z - s * k.x, t * k.z * k.z + c);
}
// Tilts a normal by a tangent-plane offset (small angles).
vec3 mat_tilt(vec3 n, vec3 t, vec3 b, vec2 d) { return normalize(n + t * d.x + b * d.y); }

// ---------------------------------------------------------------------------------------------
// Colour helpers
vec3 mat_srgb(vec3 c) { return pow(c, vec3(2.2)); }  // authoring helper: sRGB-ish -> linear
vec3 mat_hueShift(vec3 c, float warm) { return c * vec3(1.0 + warm, 1.0, 1.0 - warm); }
float mat_ior2specular(float ior) { float f0 = sq((ior - 1.0) / (ior + 1.0)); return sqrt(f0 / 0.16); }

// ---------------------------------------------------------------------------------------------
// Polish texture (baked by bake/polish.comp, bound by the library on unit 7 for glossy
// materials): rg = scratch/swirl slope (signed, 0.5 = flat), b = coat roughness boost, a = smudge.
// Tileable, 1024^2 over 0.5 m.
#ifdef MAT_USE_POLISH
layout(binding = 7) uniform sampler2D uPolish;
vec4 mat_polish2D(vec2 uvMeters) {
    vec4 t = texture(uPolish, uvMeters * 2.0);
    return vec4(t.rg * 2.0 - 1.0, t.b, t.a);
}
// Triplanar variant for curved objects (object space, meters). Returns an OS perturbation.
vec4 mat_polishTriplanar(vec3 p, vec3 n, out vec3 slopeOS) {
    vec3 w = pow(abs(n), vec3(4.0));
    w /= (w.x + w.y + w.z);
    vec4 tx = mat_polish2D(p.zy * 3.0 + 0.31), ty = mat_polish2D(p.xz * 3.0 + 0.57), tz = mat_polish2D(p.xy * 3.0 + 0.13);
    slopeOS = w.x * vec3(0.0, tx.y, tx.x) + w.y * vec3(ty.x, 0.0, ty.y) + w.z * vec3(tz.x, tz.y, 0.0);
    return w.x * tx + w.y * ty + w.z * tz;
}
#endif

#ifndef MAT_BAKE
// Viewer debug: show the albedo as unlit emission (materials viewer --albedo).
void mat_debugAlbedo(inout Surface s) {
#ifdef MAT_DEBUG_ALBEDO
    s.emission = s.albedo * 3000.0;
    s.albedo = vec3(0.0);
    s.clearcoat = 0.0;
    s.specular = 0.0;
    s.metallic = 0.0;
    s.sheenColor = vec3(0.0);
    s.subsurface = 0.0;
#endif
}
#endif
