#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 color;
layout(location = 2) in vec3 normal;
layout(location = 3) in vec2 uv;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec3 fragPosWorld;
layout(location = 2) out vec3 fragNormalWorld;
layout(location = 3) out vec2 fragUv;

#include "model_instances.glsl"

// The depth pre-pass writes depth with the same expression from the same
// shared file, and the pipeline this feeds tests EQUAL against it. See
// modelClipPosition for why both halves of that are needed.
invariant gl_Position;

void main() {
    Instance instance = instanceBuffer.instances[gl_InstanceIndex];

    vec4 positionWorld = instance.modelMatrix * vec4(position, 1.0);
    gl_Position = modelClipPosition(positionWorld);

    fragNormalWorld = normalize(mat3(instance.normalMatrix) * normal);
    fragPosWorld = positionWorld.xyz;
    fragColor = color;
    fragUv = uv;
}
