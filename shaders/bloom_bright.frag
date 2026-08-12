#version 450

// The bright-pass: keeps only the energy above a threshold, weighted so the
// cutoff is a soft shoulder rather than a hard edge. Runs at half resolution
// - bloom is blurred anyway, and half the pixels means a quarter of the blur
// work.

layout(location = 0) in vec2 fragUv;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D sceneColor;

void main() {
    vec3 color = texture(sceneColor, fragUv).rgb;

    // Anything below 1.0 in linear radiance is representable without bloom;
    // what exceeds it is what reads as glow.
    const float threshold = 1.0;
    float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
    float weight = max(luminance - threshold, 0.0) / max(luminance, 1e-4);

    outColor = vec4(color * weight, 1.0);
}
