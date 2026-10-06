// Procedural noise toolkit shared by materials and texture generators.
// All functions are deterministic and tile-free (hash based).

#ifdef DBG_FLOAT_HASH
vec3 hash33(vec3 p) {
    vec3 p3 = fract(floor(p) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yxz + 33.33);
    return fract((p3.xxy + p3.yxx) * p3.zyx);
}
#else
vec3 hash33(vec3 p) {
    uvec3 q = uvec3(ivec3(floor(p))) * uvec3(1597334673u, 3812015801u, 2798796415u);
    q = (q.x ^ q.y ^ q.z) * uvec3(1597334673u, 3812015801u, 2798796415u);
    return vec3(q) * (1.0 / 4294967295.0);
}
#endif

// Quintic-interpolated gradient noise, range ~[-1,1].
float gnoise(vec3 p) {
#ifdef DBG_NO_NOISE
    return 0.0;
#endif
    vec3 i = floor(p), f = fract(p);
    vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float r = 0.0;
    float n000 = dot(hash33(i + vec3(0, 0, 0)) * 2.0 - 1.0, f - vec3(0, 0, 0));
    float n100 = dot(hash33(i + vec3(1, 0, 0)) * 2.0 - 1.0, f - vec3(1, 0, 0));
    float n010 = dot(hash33(i + vec3(0, 1, 0)) * 2.0 - 1.0, f - vec3(0, 1, 0));
    float n110 = dot(hash33(i + vec3(1, 1, 0)) * 2.0 - 1.0, f - vec3(1, 1, 0));
    float n001 = dot(hash33(i + vec3(0, 0, 1)) * 2.0 - 1.0, f - vec3(0, 0, 1));
    float n101 = dot(hash33(i + vec3(1, 0, 1)) * 2.0 - 1.0, f - vec3(1, 0, 1));
    float n011 = dot(hash33(i + vec3(0, 1, 1)) * 2.0 - 1.0, f - vec3(0, 1, 1));
    float n111 = dot(hash33(i + vec3(1, 1, 1)) * 2.0 - 1.0, f - vec3(1, 1, 1));
    r = mix(mix(mix(n000, n100, u.x), mix(n010, n110, u.x), u.y), mix(mix(n001, n101, u.x), mix(n011, n111, u.x), u.y), u.z);
    return r * 1.6;
}

float gnoise(vec2 p) { return gnoise(vec3(p, 0.37)); }

float fbm(vec3 p, int octaves, float lacunarity, float gain) {
    float a = 0.5, s = 0.0, n = 0.0;
    for (int i = 0; i < octaves; ++i) {
        s += a * gnoise(p);
        n += a;
        p = p * lacunarity + vec3(1.7, 9.2, 3.1);
        a *= gain;
    }
    return s / n;
}
float fbm(vec3 p, int octaves) { return fbm(p, octaves, 2.03, 0.5); }

// Ridged multifractal in [0,1], good for veins.
float ridged(vec3 p, int octaves) {
    float a = 0.5, s = 0.0, n = 0.0, prev = 1.0;
    for (int i = 0; i < octaves; ++i) {
        float r = 1.0 - abs(gnoise(p));
        r *= r;
        s += a * r * prev;
        n += a;
        prev = r;
        p = p * 2.07 + vec3(3.1, 1.3, 7.7);
        a *= 0.5;
    }
    return s / n;
}

// Cellular (Worley) noise: x = F1, y = F2 distances, z = cell id hash.
vec3 voronoi(vec3 p) {
#ifdef DBG_NO_NOISE
    return vec3(0.5, 0.8, 0.5);
#endif
    vec3 i = floor(p), f = fract(p);
    float f1 = 8.0, f2 = 8.0, id = 0.0;
    for (int z = -1; z <= 1; ++z)
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x) {
                vec3 g = vec3(x, y, z);
                vec3 o = hash33(i + g);
                vec3 r = g + o - f;
                float d = dot(r, r);
                if (d < f1) { f2 = f1; f1 = d; id = hash13(i + g); }
                else if (d < f2) f2 = d;
            }
    return vec3(sqrt(f1), sqrt(f2), id);
}

// Domain warp helper.
vec3 warp(vec3 p, float amount, int octaves) {
    return p + amount * vec3(fbm(p + vec3(0.0, 0.0, 0.0), octaves), fbm(p + vec3(5.2, 1.3, 2.8), octaves),
                             fbm(p + vec3(1.7, 9.2, 4.4), octaves));
}
