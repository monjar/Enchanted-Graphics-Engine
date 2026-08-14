#version 450

layout(location = 0) in vec2 fragNdc;

layout(location = 0) out vec4 outColor;

struct PointLight {
    vec4 position;
    vec4 color;
};

layout(set = 0, binding = 0) uniform GlobalUbo {
    mat4 projection;
    mat4 view;
    mat4 inverseView;
    mat4 inverseProjection;
    mat4 sunViewProjection;
    vec4 sunDirection;
    vec4 sunColor;
    vec4 ambientLightColor;
    PointLight pointLights[16];
    int numLights;
} ubo;

layout(set = 0, binding = 4) uniform samplerCube environmentMap;

void main() {
    // Unproject the pixel back to a view-space ray, rotate it into the world,
    // and look the sky up by direction. No geometry, no special cube mesh -
    // the environment map is the scene here.
    vec4 viewTarget = ubo.inverseProjection * vec4(fragNdc, 1.0, 1.0);
    vec3 viewDir = normalize(viewTarget.xyz / viewTarget.w);
    vec3 worldDir = normalize(mat3(ubo.inverseView) * viewDir);

    // Linear HDR radiance, exactly like the PBR pass: the tonemap pass is
    // the one place display encoding happens.
    outColor = vec4(texture(environmentMap, worldDir).rgb, 1.0);
}
