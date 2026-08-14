#version 450

// Diffuse irradiance: for each direction N, the cosine-weighted integral of
// the environment over the hemisphere around N. A Lambertian surface facing N
// receives exactly this, so the PBR shader's diffuse ambient term becomes one
// texture fetch.
//
// Integrated by cosine-importance sampling with a Hammersley sequence: the
// pdf already contains the cosine factor, so the estimator is the plain
// average of the samples. The target is tiny (irradiance is smooth by
// construction), which is what keeps this affordable at startup even on a
// CPU rasterizer.

layout(location = 0) in vec2 fragUv;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform samplerCube environmentMap;

layout(push_constant) uniform Push {
    int faceIndex;
    float roughness;  // unused here; shared push layout across the IBL passes
} push;

const float PI = 3.14159265359;
const uint SAMPLE_COUNT = 512u;

vec3 faceDirection(int face, vec2 uv) {
    vec2 st = uv * 2.0 - 1.0;
    if (face == 0) return vec3(1.0, -st.y, -st.x);   // +X
    if (face == 1) return vec3(-1.0, -st.y, st.x);   // -X
    if (face == 2) return vec3(st.x, 1.0, st.y);     // +Y
    if (face == 3) return vec3(st.x, -1.0, -st.y);   // -Y
    if (face == 4) return vec3(st.x, -st.y, 1.0);    // +Z
    return vec3(-st.x, -st.y, -1.0);                 // -Z
}

// Van der Corput radical inverse, for the Hammersley low-discrepancy set.
float radicalInverseVdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

vec2 hammersley(uint i, uint count) {
    return vec2(float(i) / float(count), radicalInverseVdC(i));
}

void main() {
    vec3 N = normalize(faceDirection(push.faceIndex, fragUv));

    vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, N));
    vec3 bitangent = cross(N, tangent);

    vec3 irradiance = vec3(0.0);
    for (uint i = 0u; i < SAMPLE_COUNT; i++) {
        vec2 xi = hammersley(i, SAMPLE_COUNT);

        // Cosine-weighted hemisphere direction.
        float phi = 2.0 * PI * xi.x;
        float cosTheta = sqrt(1.0 - xi.y);
        float sinTheta = sqrt(xi.y);
        vec3 sampleDir = tangent * (cos(phi) * sinTheta) + bitangent * (sin(phi) * sinTheta) +
                         N * cosTheta;

        // A middle mip: pointwise samples of a sun-sharp environment need
        // prefiltered input or the sun aliases into blotches.
        irradiance += textureLod(environmentMap, sampleDir, 3.0).rgb;
    }

    outColor = vec4(irradiance / float(SAMPLE_COUNT), 1.0);
}
