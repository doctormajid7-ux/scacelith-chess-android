// Texture bake of the simple renderer (Renderer::bakeSimpleMaterial): a material's real procedural
// surface, evaluated once on a plane and kept in a texture the simple renderer then reads (one
// fetch per axis instead of the whole procedural surface per pixel, which a phone GPU cannot
// afford). Output: rgb = albedo (into an sRGB target), a = perceptual roughness.
//
// The plane, by uBakeAxis:
//   0, 1, 2  the plane facing +X, +Y, +Z (the triplanar projections of simple.frag): object (or
//            world) space position = uBakeOrigin + the plane's coordinates (vUV * uBakeTile)
//   3        uv space: i.uv = vUV, for the materials laid out in their mesh's uv (ceiling,
//            tapestry); position = the +Y plane over the same square
// The material's parameters and the per-object ones are those of draw uDraw (an item using it).
#include "shaders/include/common.glsl"
#include "shaders/include/surface.glsl"
#include "shaders/include/noise.glsl"
#pragma material

in vec2 vUV;
layout(location = 0) out vec4 outBake;
layout(location = 1) uniform int uBakeAxis;
layout(location = 2) uniform vec2 uBakeTile;     // plane size (m), or 1 x 1 for uv space
layout(location = 3) uniform vec3 uBakeOrigin;   // position of the plane's corner

void main() {
    vec2 t = vUV * uBakeTile;
    vec3 p, n, tg, bt;
    if (uBakeAxis == 0) { p = vec3(0.0, t.y, t.x); n = vec3(1, 0, 0); tg = vec3(0, 0, 1); bt = vec3(0, 1, 0); }
    else if (uBakeAxis == 2) { p = vec3(t.x, t.y, 0.0); n = vec3(0, 0, 1); tg = vec3(1, 0, 0); bt = vec3(0, 1, 0); }
    else { p = vec3(t.x, 0.0, t.y); n = vec3(0, 1, 0); tg = vec3(1, 0, 0); bt = vec3(0, 0, 1); }
    p += uBakeOrigin;

    SurfaceInput i;
    i.positionWS = p;
    i.positionOS = p;
    i.normalWS = n;
    i.normalOS = n;
    i.tangentWS = tg;
    i.bitangentWS = bt;
    i.uv = uBakeAxis == 3 ? vUV : t;
    i.viewDirWS = n;            // seen face on
    i.viewDistance = 1.0;
    for (int k = 0; k < 8; ++k) i.matParams[k] = draws[uDraw].matParams[k];
    for (int k = 0; k < 4; ++k) i.instParams[k] = draws[uDraw].instParams[k];
    i.objectSeed = draws[uDraw].info.x;
    i.objectId = draws[uDraw].info.w;
    i.frontFacing = true;
    i.pixel = gl_FragCoord.xy;
    i.screenUV = vUV;
    i.time = 0.0;
    i.passId = PASS_ID_MAIN;

    Surface s = defaultSurface(i);
    surface(i, s);
    outBake = vec4(clamp(s.albedo, 0.0, 1.0), clamp(s.roughness, 0.02, 1.0));
}
