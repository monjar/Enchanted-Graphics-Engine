#version 450

// The blur that pays for the rotation.
//
// Turning the occlusion kernel by a different angle per pixel is what stops
// its error showing up as bands, but it leaves the estimate noisy at the scale
// of the rotation tile. A box exactly as wide as that tile averages each pixel
// over one full period of the pattern, which cancels it: a narrower one leaves
// the pattern visible, and a wider one smears occlusion across edges that
// should stay sharp.

layout(location = 0) in vec2 fragUv;

layout(location = 0) out float outOcclusion;

layout(set = 0, binding = 0) uniform sampler2D occlusionMap;

// Mirrors ssaoNoiseSize in src/render/Ssao.hpp.
const int SSAO_NOISE_SIZE = 4;

void main() {
    vec2 texelSize = 1.0 / vec2(textureSize(occlusionMap, 0));

    float total = 0.0;
    for (int x = 0; x < SSAO_NOISE_SIZE; x++) {
        for (int y = 0; y < SSAO_NOISE_SIZE; y++) {
            vec2 offset = (vec2(x, y) - float(SSAO_NOISE_SIZE) * 0.5 + 0.5) * texelSize;
            total += texture(occlusionMap, fragUv + offset).r;
        }
    }

    outOcclusion = total / float(SSAO_NOISE_SIZE * SSAO_NOISE_SIZE);
}
