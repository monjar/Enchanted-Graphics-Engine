#ifndef EGE_MODEL_PUSH_GLSL
#define EGE_MODEL_PUSH_GLSL

// The per-object push constant block, and where a vertex of that object lands
// on screen.
//
// Three stages read this block - the depth pre-pass's vertex shader, the
// shading vertex shader and the shading fragment shader - and they read it
// through one pipeline layout, so a copy that drifts is a copy that reads the
// wrong offsets. Declared once here for the same reason the global block is.
//
// Mirrors PushConstants in src/render/PbrRenderSystem.cpp.

#include "global_ubo.glsl"

layout(push_constant) uniform Push {
    mat4 modelMatrix;
    mat4 normalMatrix;
    vec4 baseColorFactor;
    vec4 emissiveAndMetallic;       // rgb emissive, a metallic
    vec4 roughnessNormalOcclusion;  // r roughness, g normal scale, b occlusion
} push;

// Where a vertex lands in clip space.
//
// The depth pre-pass and the shading pass have to agree on this to the bit:
// the second tests EQUAL against the depth the first wrote, and a fragment
// whose depth is one ulp off fails that test and vanishes. Sharing the
// expression is half of the guarantee - the other half is `invariant
// gl_Position` in both stages, which is what forbids a compiler from
// reassociating these multiplies differently in one of them than the other.
vec4 modelClipPosition(vec4 positionWorld) {
    return ubo.projection * ubo.view * positionWorld;
}

#endif  // EGE_MODEL_PUSH_GLSL
