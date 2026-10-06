// Display transform of the simple renderer (RenderSettings::simple): exposure compensation, the
// full renderer's AgX transform with its default grade (PostSettings contrast 1.12, saturation 1.06, split tone 1), sRGB encode, fade. A fragment pass from the
// HDR target straight to the backbuffer (bilinear upscale when the render scale is below 1).
in vec2 vUV;
layout(binding = 0) uniform sampler2D uColor;
layout(location = 1) uniform float uExposure;   // 2^(exposure compensation)
layout(location = 2) uniform float uFade;       // 0 = image, 1 = black
out vec4 outColor;

float saturate(float x) { return clamp(x, 0.0, 1.0); }
vec3 saturate(vec3 x) { return clamp(x, 0.0, 1.0); }

const mat3 SRGB_TO_REC2020 = mat3(vec3(0.6274, 0.0691, 0.0164), vec3(0.3293, 0.9195, 0.0880), vec3(0.0433, 0.0113, 0.8956));
const mat3 REC2020_TO_SRGB = mat3(vec3(1.6605, -0.1246, -0.0182), vec3(-0.5876, 1.1329, -0.1006), vec3(-0.0728, -0.0083, 1.1187));
const mat3 AGX_INSET = mat3(vec3(0.856627153315983, 0.137318972929847, 0.11189821299995),
                            vec3(0.0951212405381588, 0.761241990602591, 0.0767994186031903),
                            vec3(0.0482516061458583, 0.101439036467562, 0.811302368396859));
const mat3 AGX_OUTSET = mat3(vec3(1.1271005818144368, -0.1413297634984383, -0.14132976349843826),
                             vec3(-0.11060664309660323, 1.157823702216272, -0.11060664309660294),
                             vec3(-0.016493938717834573, -0.016493938717834257, 1.2519364065950405));
const float AGX_MIN_EV = -12.47393, AGX_MAX_EV = 4.026069;

vec3 agxContrast(vec3 x) {
    vec3 x2 = x * x, x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

// displayTransform() and srgbEncode() have a CPU port in src/render/post/display_transform.h (the
// brightness calibration draws its patches with it): change both together.
vec3 displayTransform(vec3 c) {
    c = SRGB_TO_REC2020 * c;
    c = AGX_INSET * c;
    c = clamp((log2(max(c, vec3(1e-10))) - AGX_MIN_EV) / (AGX_MAX_EV - AGX_MIN_EV), 0.0, 1.0);
    c = agxContrast(c);
    // Look (in the AgX encoded, perceptual domain). Contrast: S-curve through (0,0), (pivot,
    // pivot), (1,1) with slope grade.x at the pivot, so the highlights still roll off to white
    // instead of clipping.
    float l = dot(c, vec3(0.2626, 0.6780, 0.0593));
    const float pivot = 0.42;
    c = clamp(c, 0.0, 1.0);
    vec3 lo = pivot * pow(c / pivot, vec3(1.12));
    vec3 hi = 1.0 - (1.0 - pivot) * pow((1.0 - c) / (1.0 - pivot), vec3(1.12));
    c = mix(lo, hi, step(pivot, c));
    vec3 cool = vec3(0.985, 1.0, 1.03), warm = vec3(1.03, 1.0, 0.955);
    vec3 tone = mix(cool, warm, smoothstep(0.18, 0.72, l));
    c *= tone;
    l = dot(c, vec3(0.2626, 0.6780, 0.0593));
    c = l + (c - l) * 1.06;
    c = AGX_OUTSET * c;
    c = pow(max(c, vec3(0.0)), vec3(2.2));
    c = REC2020_TO_SRGB * c;
    return clamp(c, 0.0, 1.0);
}

vec3 srgbEncode(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c)); }

void main() {
    vec3 c = textureLod(uColor, vUV, 0.0).rgb * uExposure;
    if (any(isnan(c)) || any(isinf(c))) c = vec3(0.0);
    c = clamp(c, vec3(0.0), vec3(60000.0));
    vec3 enc = srgbEncode(displayTransform(c));
    outColor = vec4(saturate(enc * (1.0 - saturate(uFade))), 1.0);
}
