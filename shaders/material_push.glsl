#ifndef EGE_MATERIAL_PUSH_GLSL
#define EGE_MATERIAL_PUSH_GLSL

// The per-material scalars, pushed with the draw that uses them.
//
// Only the fragment stage reads these, and only three vec4 of them survive:
// the two matrices that used to sit in front took the block to 176 bytes,
// past the 128 every Vulkan implementation is required to offer. They live in
// a buffer now, indexed by the instance, and what is left fits with room to
// spare.
//
// Mirrors MaterialPush in src/render/PbrRenderSystem.cpp.

layout(push_constant) uniform Push {
    vec4 baseColorFactor;
    vec4 emissiveAndMetallic;       // rgb emissive, a metallic
    vec4 roughnessNormalOcclusion;  // r roughness, g normal scale, b occlusion
} push;

#endif  // EGE_MATERIAL_PUSH_GLSL
