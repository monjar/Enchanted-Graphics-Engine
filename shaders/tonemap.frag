#version 450

layout(location = 0) in vec2 fragUv;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D sceneColor;

// Narkowicz's fit of the ACES filmic curve. Compared to the Reinhard operator
// it replaces, it keeps saturation in bright regions instead of washing them
// out towards white, and it has an actual toe, so shadows go dark rather than
// grey.
vec3 acesTonemap(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 hdr = texture(sceneColor, fragUv).rgb;

    // No gamma encode here: the swapchain image is an sRGB format, so the
    // hardware applies the transfer curve when this linear value is written.
    // Encoding manually as well would apply the curve twice.
    outColor = vec4(acesTonemap(hdr), 1.0);
}
