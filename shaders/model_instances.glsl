#ifndef EGE_MODEL_INSTANCES_GLSL
#define EGE_MODEL_INSTANCES_GLSL

// Where a drawn object's transform comes from, and where a vertex of it lands.
//
// The transform used to arrive in push constants, one object per draw call.
// That made every object its own draw whatever it was: a hundred crates
// sharing a mesh and a material still cost a hundred submissions, a hundred
// push constant updates and a hundred vertex buffer binds, all to change
// sixty four bytes. The transforms now live in one buffer indexed by the
// instance, so consecutive objects sharing a mesh and a material are one
// instanced draw and the difference between them is a read the shader was
// doing anyway.
//
// gl_InstanceIndex counts from the draw's firstInstance rather than from
// zero - which is the difference between Vulkan's index and OpenGL's
// gl_InstanceID, and is what lets a batch point at its own run of this
// buffer without a per-draw offset to add.
//
// Mirrors GpuInstance in src/render/PbrRenderSystem.hpp.

#include "global_ubo.glsl"

struct Instance {
    mat4 modelMatrix;
    // A mat4 holding a mat3. Sending three vec4 columns instead would save
    // sixteen bytes per object and cost the reader a padding rule that is
    // easy to get wrong in one of the two places it is written.
    mat4 normalMatrix;
};

layout(std430, set = 0, binding = 11) readonly buffer InstanceBuffer {
    Instance instances[];
} instanceBuffer;

// Where a vertex lands in clip space.
//
// The depth pre-pass and the shading pass have to agree on this to the bit:
// the second tests EQUAL against the depth the first wrote, and a fragment
// whose depth is one ulp off fails that test and vanishes. Sharing the
// expression is half of the guarantee - the other half is `invariant
// gl_Position` in both stages, which is what forbids a compiler from
// reassociating these multiplies differently in one of them than the other.
//
// Both passes read the same instance from the same buffer, so the transform
// that reaches this is the same one as well.
vec4 modelClipPosition(vec4 positionWorld) {
    return ubo.projection * ubo.view * positionWorld;
}

#endif  // EGE_MODEL_INSTANCES_GLSL
