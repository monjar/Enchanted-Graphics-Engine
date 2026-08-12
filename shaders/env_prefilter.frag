#version 450

// Specular prefiltering, the first half of the split-sum approximation: each
// mip of the output holds the environment convolved with the GGX lobe for one
// roughness, so a reflection at runtime is a single textureLod at
// roughness-scaled level. Samples follow GGX importance sampling; each sample
// reads the environment mip matched to its solid angle (Karis), which is what
// stops the sun from shattering into fireflies at higher roughness.

layout(location = 0) in vec2 fragUv;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform samplerCube environmentMap;

layout(push_constant) uniform Push {
    int faceIndex;
    float roughness;
} push;

const float PI = 3.14159265359;
const uint SAMPLE_COUNT = 128u;

vec3 faceDirection(int face, vec2 uv) {
    vec2 st = uv * 2.0 - 1.0;
    if (face == 0) return vec3(1.0, -st.y, -st.x);   // +X
    if (face == 1) return vec3(-1.0, -st.y, st.x);   // -X
    if (face == 2) return vec3(st.x, 1.0, st.y);     // +Y
    if (face == 3) return vec3(st.x, -1.0, -st.y);   // -Y
    if (face == 4) return vec3(st.x, -st.y, 1.0);    // +Z
    return vec3(-st.x, -st.y, -1.0);                 // -Z
}

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

// A half-vector distributed by GGX around N for the given roughness.
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

float distributionGGX(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 1e-7);
}

void main() {
    // The split-sum approximation: N = V = R, so the lobe is symmetric around
    // the reflection vector. This is what makes one map serve every view
    // angle, at the cost of stretched highlights at grazing incidence.
    vec3 N = normalize(faceDirection(push.faceIndex, fragUv));
    vec3 R = N;
    vec3 V = N;

    float envResolution = float(textureSize(environmentMap, 0).x);

    vec3 accumulated = vec3(0.0);
    float totalWeight = 0.0;

    for (uint i = 0u; i < SAMPLE_COUNT; i++) {
        vec2 xi = hammersley(i, SAMPLE_COUNT);
        vec3 H = importanceSampleGGX(xi, N, push.roughness);
        vec3 L = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = max(dot(N, L), 0.0);
        if (NdotL <= 0.0) {
            continue;
        }

        // Match each sample's footprint to an environment mip so sparse
        // samples of a bright sun average instead of speckling.
        float NdotH = max(dot(N, H), 0.0);
        float HdotV = max(dot(H, V), 0.0);
        float pdf = distributionGGX(NdotH, push.roughness) * NdotH / max(4.0 * HdotV, 1e-4);
        float saTexel = 4.0 * PI / (6.0 * envResolution * envResolution);
        float saSample = 1.0 / (float(SAMPLE_COUNT) * pdf + 1e-4);
        float mipLevel = push.roughness == 0.0 ? 0.0 : 0.5 * log2(saSample / saTexel);

        accumulated += textureLod(environmentMap, L, mipLevel).rgb * NdotL;
        totalWeight += NdotL;
    }

    outColor = vec4(accumulated / max(totalWeight, 1e-4), 1.0);
}
