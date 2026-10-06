// Shared geometry-stage code: transforms an object-space vertex into all interpolants the
// fragment stages expect. Used by mesh.vert (untessellated) and mesh.tese.
#include "shaders/include/common.glsl"

out VertexData {
    vec3 posWS;
    vec3 posOS;
    vec3 normalWS;
    vec3 normalOS;
    vec4 tangentWS;
    vec2 uv;
    vec4 curClip;   // unjittered current clip position (motion vectors)
    vec4 prevClip;  // previous frame clip position
    flat int draw;
#ifdef SCACELITH_GLES
    // GL ES has no gl_ClipDistance, and no gl_PerVertex block to redeclare it in: the same value
    // rides in the stage interface and the fragment stage discards on it (fragment_input.glsl).
    float clipDist;
#endif
} vout;

#ifndef SCACELITH_GLES
out gl_PerVertex {
    vec4 gl_Position;
    float gl_ClipDistance[1];
};
#endif
invariant gl_Position;

#pragma displacement

void emitVertex(vec3 posOS, vec3 nOS, vec4 tOS, vec2 uv, int drawIdx) {
    // Fields read one by one, never the whole DrawData copied out of the SSBO: Adreno's compiler
    // mangles a copy of a buffer struct that holds arrays (the materials then read zeros).
#define dd draws[drawIdx]
#ifdef MATERIAL_HAS_DISPLACEMENT
    posOS = displace(posOS, nOS, uv, drawIdx);
#endif
    vec4 wp = dd.model * vec4(posOS, 1.0);
    vout.posWS = wp.xyz;
    vout.posOS = posOS;
    mat3 nm = mat3(dd.normalMatrix);
    vout.normalWS = normalize(nm * nOS);
    vout.normalOS = nOS;
    vout.tangentWS = vec4(normalize(mat3(dd.model) * tOS.xyz), tOS.w);
    vout.uv = uv;
    vout.curClip = frame.viewProjNoJitter * wp;
    vout.prevClip = frame.prevViewProj * (dd.prevModel * vec4(posOS, 1.0));
    vout.draw = drawIdx;
    gl_Position = frame.viewProj * wp;
#undef dd
#ifdef SCACELITH_GLES
    vout.clipDist = dot(wp.xyz, frame.clipPlane.xyz) + frame.clipPlane.w;
#else
    gl_ClipDistance[0] = dot(wp.xyz, frame.clipPlane.xyz) + frame.clipPlane.w;
#endif
}
