#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec3 fragPosWorld;
layout(location = 2) in vec3 fragNormalWorld;
layout(location = 3) in vec2 fragUv;

layout(location = 0) out vec4 outColor;

struct PointLight {
    vec4 position;
    vec4 color;
};

layout(set = 0, binding = 0) uniform GlobalUbo {
    mat4 projection;
    mat4 view;
    mat4 inverseView;
    vec4 ambientLightColor;
    PointLight pointLights[16];
    int numLights;
} ubo;

layout(set = 1, binding = 0) uniform sampler2D baseColorMap;
layout(set = 1, binding = 1) uniform sampler2D normalMap;
layout(set = 1, binding = 2) uniform sampler2D metallicRoughnessMap;
layout(set = 1, binding = 3) uniform sampler2D emissiveMap;

layout(push_constant) uniform Push {
    mat4 modelMatrix;
    mat4 normalMatrix;
    vec4 baseColorFactor;
    vec4 emissiveAndMetallic;
    vec4 roughnessNormalOcclusion;
} push;

const float PI = 3.14159265359;

// Trowbridge-Reitz GGX. Describes what fraction of microfacets are oriented
// along the halfway vector, which is what gives a rough surface its wide
// highlight and a smooth one its tight highlight.
float distributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 1e-7);
}

// Smith's method with the Schlick-GGX approximation. Accounts for microfacets
// shadowing and masking each other at grazing angles.
float geometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    return geometrySchlickGGX(max(dot(N, V), 0.0), roughness) *
           geometrySchlickGGX(max(dot(N, L), 0.0), roughness);
}

// Fresnel: how reflective a surface becomes at grazing angles. Every surface
// approaches a mirror at 90 degrees, which is what makes this matter.
vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Tangent frame derived from screen-space derivatives, so a normal map works
// without tangents in the vertex data. Replaced by real tangents when glTF
// import lands, since derivatives break on mirrored UVs.
vec3 applyNormalMap(vec3 N, vec3 worldPos, vec2 uv, float scale) {
    vec3 tangentNormal = texture(normalMap, uv).xyz * 2.0 - 1.0;
    tangentNormal.xy *= scale;

    vec3 Q1 = dFdx(worldPos);
    vec3 Q2 = dFdy(worldPos);
    vec2 st1 = dFdx(uv);
    vec2 st2 = dFdy(uv);

    vec3 T = Q1 * st2.t - Q2 * st1.t;
    if (dot(T, T) < 1e-12) {
        return N;  // degenerate UVs; keep the geometric normal
    }
    T = normalize(T - N * dot(N, T));
    vec3 B = normalize(cross(N, T));
    return normalize(mat3(T, B, N) * tangentNormal);
}

void main() {
    vec4 baseSample = texture(baseColorMap, fragUv) * push.baseColorFactor;
    vec3 albedo = baseSample.rgb * fragColor;

    vec3 mrSample = texture(metallicRoughnessMap, fragUv).rgb;
    // glTF packing: roughness in green, metallic in blue.
    float roughness = clamp(mrSample.g * push.roughnessNormalOcclusion.r, 0.04, 1.0);
    float metallic = clamp(mrSample.b * push.emissiveAndMetallic.a, 0.0, 1.0);
    float occlusion = mix(1.0, mrSample.r, push.roughnessNormalOcclusion.b);

    vec3 N = normalize(fragNormalWorld);
    N = applyNormalMap(N, fragPosWorld, fragUv, push.roughnessNormalOcclusion.g);

    vec3 cameraPos = ubo.inverseView[3].xyz;
    vec3 V = normalize(cameraPos - fragPosWorld);

    // Dielectrics reflect about 4% at normal incidence; metals use their
    // albedo as the reflectance and have no diffuse term at all.
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    vec3 Lo = vec3(0.0);
    for (int i = 0; i < ubo.numLights; i++) {
        vec3 toLight = ubo.pointLights[i].position.xyz - fragPosWorld;
        float distanceSquared = max(dot(toLight, toLight), 1e-6);
        vec3 L = toLight * inversesqrt(distanceSquared);
        vec3 H = normalize(V + L);

        float attenuation = 1.0 / distanceSquared;
        vec3 radiance = ubo.pointLights[i].color.rgb * ubo.pointLights[i].color.w * attenuation;

        float NDF = distributionGGX(N, H, roughness);
        float G = geometrySmith(N, V, L, roughness);
        vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);

        vec3 numerator = NDF * G * F;
        float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 1e-4;
        vec3 specular = numerator / denominator;

        // Energy conservation: what is reflected specularly is not available
        // to be refracted diffusely, and metals refract nothing.
        vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);

        float NdotL = max(dot(N, L), 0.0);
        Lo += (kD * albedo / PI + specular) * radiance * NdotL;
    }

    vec3 ambient = ubo.ambientLightColor.rgb * ubo.ambientLightColor.w * albedo * occlusion;
    vec3 emissive = texture(emissiveMap, fragUv).rgb * push.emissiveAndMetallic.rgb;

    vec3 color = ambient + Lo + emissive;

    // Reinhard tonemapping and a gamma encode. A proper HDR target with ACES
    // belongs in the post-processing stack; this keeps the swapchain write
    // sane until then.
    color = color / (color + vec3(1.0));
    color = pow(color, vec3(1.0 / 2.2));

    outColor = vec4(color, baseSample.a);
}
