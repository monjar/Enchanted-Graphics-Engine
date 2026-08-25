#ifndef EGE_SKINNING_GLSL
#define EGE_SKINNING_GLSL

// Where a skinned vertex's deformation comes from.
//
// The palette holds every animated entity's skinning matrices for the frame,
// packed end to end; an entity's run starts at the base its draw pushes. The
// matrices are computed on the CPU by AnimationSampling - global joint
// transform times inverse bind - and a vertex blends the four its stream
// names, weighted, before the ordinary model transform sees it.
//
// Both the skinned depth shader and the skinned scene shader include this
// and apply it identically, for the same reason model_instances.glsl exists:
// the scene pass tests EQUAL against the depth pass's positions, and two
// skinning expressions that differ by one operation order produce depths one
// ulp apart and a character that vanishes.

layout(std430, set = 0, binding = 12) readonly buffer PaletteBuffer {
    mat4 matrices[];
} palette;

// The base is pushed per draw rather than carried per instance, because a
// skinned batch is one entity: two characters sharing a mesh still need two
// different palette runs, so they never merge anyway.
layout(push_constant) uniform SkinPush {
    layout(offset = 64) uint paletteBase;
} skinPush;

mat4 skinMatrix(uvec4 joints, vec4 weights) {
    return weights.x * palette.matrices[skinPush.paletteBase + joints.x] +
           weights.y * palette.matrices[skinPush.paletteBase + joints.y] +
           weights.z * palette.matrices[skinPush.paletteBase + joints.z] +
           weights.w * palette.matrices[skinPush.paletteBase + joints.w];
}

#endif
