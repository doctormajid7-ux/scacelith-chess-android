// Fragment-side interpolants and SurfaceInput construction shared by the mesh passes.
in VertexData {
    vec3 posWS;
    vec3 posOS;
    vec3 normalWS;
    vec3 normalOS;
    vec4 tangentWS;
    vec2 uv;
    vec4 curClip;
    vec4 prevClip;
    flat int draw;
#ifdef SCACELITH_GLES
    float clipDist;   // emulated gl_ClipDistance[0] (mesh_common.glsl)
#endif
} vin;

#ifdef DBG_UNIFORM_DRAW
#define DRAW_INDEX_FS uDraw
#else
#define DRAW_INDEX_FS vin.draw
#endif

SurfaceInput buildSurfaceInput() {
#if defined(SCACELITH_GLES) && defined(PASS_PLANAR)
    // What the hardware clip distance does on desktop. Only the planar reflection passes set a
    // clip plane (planar.cpp; every other pass has (0,0,0,1)), so only they pay for the discard:
    // on Adreno a shader that can discard loses early depth testing, and the main pass would
    // then shade every hidden fragment of the hall (~1 s a frame instead of ~30 ms).
    if (vin.clipDist < 0.0) discard;
#endif
    SurfaceInput i;
    // Field by field, not a copy of the whole DrawData (see mesh_common.glsl: Adreno).
#define dd draws[DRAW_INDEX_FS]
    i.positionWS = vin.posWS;
    i.positionOS = vin.posOS;
    vec3 n = normalize(vin.normalWS);
#ifdef MATERIAL_DOUBLE_SIDED
    if (!gl_FrontFacing) n = -n;
#endif
    i.normalWS = n;
    i.normalOS = normalize(vin.normalOS);
    vec3 t = vin.tangentWS.xyz - n * dot(n, vin.tangentWS.xyz);
    t = dot(t, t) > 1e-10 ? normalize(t) : normalize(cross(n, abs(n.y) < 0.9 ? vec3(0, 1, 0) : vec3(1, 0, 0)));
    i.tangentWS = t;
    i.bitangentWS = cross(n, t) * (vin.tangentWS.w < 0.0 ? -1.0 : 1.0);
    i.uv = vin.uv;
    vec3 toCam = frame.cameraPos.xyz - vin.posWS;
    i.viewDistance = length(toCam);
    i.viewDirWS = toCam / max(i.viewDistance, 1e-6);
    for (int k = 0; k < 8; ++k) i.matParams[k] = dd.matParams[k];
    for (int k = 0; k < 4; ++k) i.instParams[k] = dd.instParams[k];
    i.objectSeed = dd.info.x;
    i.objectId = dd.info.w;
    i.frontFacing = gl_FrontFacing;
    i.pixel = gl_FragCoord.xy;
    i.screenUV = gl_FragCoord.xy * frame.resolution.zw;
    i.time = frame.cameraPos.w;
    i.passId = int(frame.passInfo.x);
#undef dd
    return i;
}

vec2 motionVector() {
    vec2 cur = vin.curClip.xy / vin.curClip.w;
    vec2 prev = vin.prevClip.xy / vin.prevClip.w;
    return (cur - prev) * 0.5;
}

// Screen-door transparency (DrawItem::opacity, materials with MATERIAL_SCREEN_DOOR): true when
// this pixel of a see-through draw is left out. It only depends on the pixel and the frame, so
// the prepass and the main pass leave out exactly the same pixels (the main pass tests GEQUAL
// against the prepass depth). Threshold = 4x4 ordered dither: 4 x fine rank (2x2 phase) + coarse
// rank. Without TAA the pattern stays still (a fine, steady mesh rather than a shimmer). With TAA
// the fine ranks are permuted every frame: at an opacity of k/4 every 2x2 block keeps exactly k
// pixels in every frame and every pixel is kept 2k frames out of 8, so any history footprint
// holds both layers (no disocclusion rejection) and TAA blends them. The 8 permutations follow
// the renderer's 8-frame Halton jitter: a plain rotation lines up with it (taa.comp's
// reconstruction filter then favours some pixel phases: stripes); this schedule was searched so
// that every phase gets the same average and the smallest ripple through that filter and the
// history feedback. Entry n: 2 bits per phase (fx + 2 fy) = its rank in frame n.
const uint kScreenDoorRanks[8] = uint[](147u, 198u, 108u, 147u, 57u, 108u, 57u, 198u);

bool screenDoorHidden() {
    vec4 fade = draws[DRAW_INDEX_FS].fade;
    if (fade.x >= 1.0) return false;
    uvec2 q = uvec2(gl_FragCoord.xy);
    uvec2 f = q & 1u, c = (q >> 1u) & 1u;
    uint fine = ((f.x ^ f.y) << 1u) | f.y;   // 2x2 Bayer: 0 2 / 3 1
    uint coarse = ((c.x ^ c.y) << 1u) | c.y;
    if (fade.y > 0.5) {
        uint n = uint(frame.skyParams.w);
        fine = (kScreenDoorRanks[n & 7u] >> (2u * (f.x + 2u * f.y))) & 3u;
        coarse = (coarse + (n >> 3u)) & 3u;
    }
    return (float(fine * 4u + coarse) + 0.5) * (1.0 / 16.0) >= fade.x;
}
