#version 450

// The second half of the split-sum approximation: the GGX environment BRDF
// integrated over the hemisphere, parameterised by (NdotV, roughness) and
// stored as a scale and bias for F0. Depends on nothing but the BRDF itself,
// so one LUT serves every environment.

layout(location = 0) in vec2 fragUv;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform Push {
    int faceIndex;    // unused here; shared push layout across the IBL passes
    float roughness;  // unused
} push;

const float PI = 3.14159265359;
const uint SAMPLE_COUNT = 512u;

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

vec3 importanceSampleGGX(vec2 xi, vec3 N, float roughness) {
    float a = roughness * roughness;

    float phi = 2.0 * PI * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    vec3 H = vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);

    vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, N));
    vec3 bitangent = cross(N, tangent);
    return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

// Smith geometry with the IBL k remapping (k = a^2 / 2), not the analytic
// light one - using the direct-lighting k here is a classic subtle error.
float geometrySchlickGGX(float NdotV, float roughness) {
    float a = roughness * roughness;
    float k = (a * a) / 2.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float geometrySmith(float NdotV, float NdotL, float roughness) {
    return geometrySchlickGGX(NdotV, roughness) * geometrySchlickGGX(NdotL, roughness);
}

void main() {
    float NdotV = max(fragUv.x, 1e-3);
    float roughness = fragUv.y;

    vec3 V = vec3(sqrt(1.0 - NdotV * NdotV), 0.0, NdotV);
    vec3 N = vec3(0.0, 0.0, 1.0);

    float scale = 0.0;
    float bias = 0.0;

    for (uint i = 0u; i < SAMPLE_COUNT; i++) {
        vec2 xi = hammersley(i, SAMPLE_COUNT);
        vec3 H = importanceSampleGGX(xi, N, roughness);
        vec3 L = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = max(L.z, 0.0);
        if (NdotL <= 0.0) {
            continue;
        }
        float NdotH = max(H.z, 0.0);
        float VdotH = max(dot(V, H), 0.0);

        float G = geometrySmith(NdotV, NdotL, roughness);
        float GVis = (G * VdotH) / (NdotH * NdotV);
        float Fc = pow(1.0 - VdotH, 5.0);

        scale += (1.0 - Fc) * GVis;
        bias += Fc * GVis;
    }

    outColor = vec4(scale / float(SAMPLE_COUNT), bias / float(SAMPLE_COUNT), 0.0, 1.0);
}
