#version 450

// One direction of a separable Gaussian. The same shader runs twice per
// frame - horizontally then vertically - because an NxN Gaussian is the
// product of two N-tap ones, and 18 taps beat 81.

layout(location = 0) in vec2 fragUv;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D source;

layout(push_constant) uniform Push {
    // (1,0) for the horizontal pass, (0,1) for the vertical one.
    vec2 direction;
} push;

void main() {
    vec2 texelStep = push.direction / vec2(textureSize(source, 0));

    // 9-tap Gaussian, sigma ~1.8, weights normalised.
    const float weights[5] = float[](0.2270, 0.1946, 0.1216, 0.0540, 0.0162);

    vec3 sum = texture(source, fragUv).rgb * weights[0];
    for (int i = 1; i < 5; i++) {
        vec2 offset = texelStep * float(i);
        sum += texture(source, fragUv + offset).rgb * weights[i];
        sum += texture(source, fragUv - offset).rgb * weights[i];
    }

    outColor = vec4(sum, 1.0);
}
