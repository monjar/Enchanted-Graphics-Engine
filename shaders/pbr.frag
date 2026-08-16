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
    mat4 inverseProjection;
    // One per cascade: the matrix each shadow map was rendered through.
    mat4 sunViewProjection[4];
    // Where each cascade ends, as a distance along the camera's forward
    // axis. Beyond the last one nothing is shadowed.
    vec4 cascadeSplits;
    vec4 sunDirection;  // xyz: direction the light travels; w unused
    vec4 sunColor;      // rgb color, w intensity; 0 disables the sun
    vec4 ambientLightColor;
    PointLight pointLights[16];
    int numLights;
    int cascadeCount;
} ubo;

layout(set = 0, binding = 1) uniform samplerCube irradianceMap;
layout(set = 0, binding = 2) uniform samplerCube prefilteredMap;
layout(set = 0, binding = 3) uniform sampler2D brdfLut;
layout(set = 0, binding = 5) uniform sampler2DArrayShadow shadowMap;

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

// The Fresnel term for ambient light has no single half-vector; damping the
// grazing response by roughness (Fdez-Aguera) keeps rough surfaces from
// picking up a bright rim they should have scattered away.
vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 +
           (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// How much sun reaches this point: 0 fully shadowed, 1 fully lit. A 3x3 PCF
// kernel over the comparison sampler, each tap already 2x2 filtered by the
// hardware, so the penumbra is 4x4-soft for nine taps.
float sampleCascade(int cascade, vec3 worldPos) {
    vec4 lightClip = ubo.sunViewProjection[cascade] * vec4(worldPos, 1.0);
    vec3 lightNdc = lightClip.xyz / lightClip.w;

    // Behind the light's far plane or in front of its near plane: call it
    // lit rather than extend the frustum. Outside x/y the border color of
    // the sampler already answers "lit".
    if (lightNdc.z < 0.0 || lightNdc.z > 1.0) {
        return 1.0;
    }

    vec2 uv = lightNdc.xy * 0.5 + 0.5;
    vec2 texelSize = 1.0 / vec2(textureSize(shadowMap, 0).xy);

    float shadow = 0.0;
    for (int x = -1; x <= 1; x++) {
        for (int y = -1; y <= 1; y++) {
            shadow += texture(
                shadowMap, vec4(uv + vec2(x, y) * texelSize, float(cascade), lightNdc.z));
        }
    }
    return shadow / 9.0;
}

// Which cascade covers this fragment, chosen by how far down the camera's
// forward axis it sits - the same measure the splits were computed against.
int cascadeFor(float viewDepth) {
    for (int i = 0; i < ubo.cascadeCount - 1; i++) {
        if (viewDepth < ubo.cascadeSplits[i]) {
            return i;
        }
    }
    return ubo.cascadeCount - 1;
}

float sunShadowFactor(vec3 worldPos) {
    // View depth: distance along the camera's forward axis, which is the
    // measure the splits were computed in. This engine's camera looks down
    // +Z - its projection puts w = z rather than w = -z - so the view-space
    // z is already that distance. Negating it, as the more common -Z
    // convention would need, makes every depth negative, sends every fragment
    // to cascade 0, and quietly unshadows everything past the first split.
    float viewDepth = (ubo.view * vec4(worldPos, 1.0)).z;

    int cascade = cascadeFor(viewDepth);
    float shadow = sampleCascade(cascade, worldPos);

    // Fade across the seam into the next cascade. Two maps of different texel
    // density meeting at a hard line puts a visible edge across the ground
    // that has nothing to do with the scene; blending over the last tenth of
    // a cascade hides the transition at the cost of two lookups in a thin
    // band.
    if (cascade < ubo.cascadeCount - 1) {
        float end = ubo.cascadeSplits[cascade];
        float start = cascade == 0 ? 0.0 : ubo.cascadeSplits[cascade - 1];
        float band = (end - start) * 0.1;
        float into = (viewDepth - (end - band)) / max(band, 1e-4);
        if (into > 0.0) {
            shadow = mix(shadow, sampleCascade(cascade + 1, worldPos), clamp(into, 0.0, 1.0));
        }
    }
    return shadow;
}

// One direction's worth of Cook-Torrance, shared by the sun and the point
// light loop.
vec3 directLight(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 albedo, float metallic,
                 float roughness, vec3 F0) {
    vec3 H = normalize(V + L);

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
    return (kD * albedo / PI + specular) * radiance * NdotL;
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

    // The sun: parallel light, shadowed by the depth map.
    if (ubo.sunColor.w > 0.0) {
        vec3 L = -normalize(ubo.sunDirection.xyz);
        vec3 radiance = ubo.sunColor.rgb * ubo.sunColor.w * sunShadowFactor(fragPosWorld);
        Lo += directLight(N, V, L, radiance, albedo, metallic, roughness, F0);
    }

    for (int i = 0; i < ubo.numLights; i++) {
        vec3 toLight = ubo.pointLights[i].position.xyz - fragPosWorld;
        float distanceSquared = max(dot(toLight, toLight), 1e-6);
        vec3 L = toLight * inversesqrt(distanceSquared);

        float attenuation = 1.0 / distanceSquared;
        vec3 radiance = ubo.pointLights[i].color.rgb * ubo.pointLights[i].color.w * attenuation;

        Lo += directLight(N, V, L, radiance, albedo, metallic, roughness, F0);
    }

    // Image-based ambient: the split-sum approximation. Diffuse comes from
    // the irradiance map; specular from the prefiltered environment at
    // roughness-scaled mip, shaped by the BRDF LUT's F0 scale and bias.
    // ambientLightColor survives as a tint and overall scale on it.
    float NdotV = max(dot(N, V), 0.0);
    vec3 F = fresnelSchlickRoughness(NdotV, F0, roughness);
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);

    vec3 irradiance = texture(irradianceMap, N).rgb;
    vec3 diffuseAmbient = kD * irradiance * albedo;

    vec3 R = reflect(-V, N);
    float prefilteredMips = float(textureQueryLevels(prefilteredMap) - 1);
    vec3 prefiltered = textureLod(prefilteredMap, R, roughness * prefilteredMips).rgb;
    vec2 brdf = texture(brdfLut, vec2(NdotV, roughness)).rg;
    vec3 specularAmbient = prefiltered * (F0 * brdf.x + brdf.y);

    vec3 ambient = (diffuseAmbient + specularAmbient) * ubo.ambientLightColor.rgb *
                   ubo.ambientLightColor.w * occlusion;

    vec3 emissive = texture(emissiveMap, fragUv).rgb * push.emissiveAndMetallic.rgb;

    // Linear HDR radiance, written to a float target. Tonemapping and the
    // sRGB encode are the post pass's job - doing either here would bake the
    // display transform into a buffer later passes still want to read as
    // physical light.
    outColor = vec4(ambient + Lo + emissive, baseSample.a);
}
