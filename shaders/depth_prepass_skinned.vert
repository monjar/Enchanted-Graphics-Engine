#version 450
#extension GL_GOOGLE_include_directive : require

// Depth for skinned meshes: position through the palette, nothing else.
// Everything here must match pbr_skinned.vert to the bit - same includes,
// same expression, same invariant declaration - because the scene pass
// tests EQUAL against what this writes.

layout(location = 0) in vec3 position;
layout(location = 4) in uvec4 jointIndices;
layout(location = 5) in vec4 jointWeights;

#include "model_instances.glsl"
#include "skinning.glsl"

invariant gl_Position;

void main() {
    Instance instance = instanceBuffer.instances[gl_InstanceIndex];
    mat4 skin = skinMatrix(jointIndices, jointWeights);

    vec4 positionWorld = instance.modelMatrix * (skin * vec4(position, 1.0));
    gl_Position = modelClipPosition(positionWorld);
}
