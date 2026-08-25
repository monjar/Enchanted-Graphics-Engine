#version 450
#extension GL_GOOGLE_include_directive : require

// The scene vertex shader for skinned meshes: pbr.vert with one extra
// matrix. The skinning happens in mesh space, before the model transform,
// so a character deforms the same way wherever it stands.

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 color;
layout(location = 2) in vec3 normal;
layout(location = 3) in vec2 uv;
layout(location = 4) in uvec4 jointIndices;
layout(location = 5) in vec4 jointWeights;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec3 fragPosWorld;
layout(location = 2) out vec3 fragNormalWorld;
layout(location = 3) out vec2 fragUv;

#include "model_instances.glsl"
#include "skinning.glsl"

// Tested EQUAL against what depth_prepass_skinned.vert wrote; see
// modelClipPosition for why both shaders must say this.
invariant gl_Position;

void main() {
    Instance instance = instanceBuffer.instances[gl_InstanceIndex];
    mat4 skin = skinMatrix(jointIndices, jointWeights);

    vec4 positionWorld = instance.modelMatrix * (skin * vec4(position, 1.0));
    gl_Position = modelClipPosition(positionWorld);

    // The skin matrix rotates normals too. Its inverse-transpose would be
    // exact under non-uniform joint scale; joints in practice rotate and
    // translate, where the matrix is its own inverse-transpose, and a
    // normalize absorbs uniform scale. The exotic case shades slightly
    // wrong rather than costing every vertex an inversion.
    fragNormalWorld = normalize(mat3(instance.normalMatrix) * mat3(skin) * normal);
    fragPosWorld = positionWorld.xyz;
    fragColor = color;
    fragUv = uv;
}
