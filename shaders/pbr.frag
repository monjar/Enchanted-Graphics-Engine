#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec3 fragPosWorld;
layout(location = 2) in vec3 fragNormalWorld;
layout(location = 3) in vec2 fragUv;

layout(location = 0) out vec4 outColor;

#include "global_ubo.glsl"

layout(set = 0, binding = 1) uniform samplerCube irradianceMap;
layout(set = 0, binding = 2) uniform samplerCube prefilteredMap;
layout(set = 0, binding = 3) uniform sampler2D brdfLut;
layout(set = 0, binding = 5) uniform sampler2DArrayShadow shadowMap;
// One cube per shadow-casting point light, sampled by direction rather than
// by index - the hardware picks the face and filters across the seams.
layout(set = 0, binding = 8) uniform samplerCubeArrayShadow pointShadowMaps;
// One map per shadow-casting spot. A spot has a single direction and a
// bounded angle, so unlike a point light it needs no cube and unlike the sun
// it needs no cascades - one ordinary projective lookup covers it.
layout(set = 0, binding = 9) uniform sampler2DArrayShadow spotShadowMaps;

layout(std430, set = 0, binding = 6) readonly buffer LightBuffer {
    Light lights[];
} lightBuffer;

// Per-cluster light lists, filled by light_cull.comp: a count followed by
// that many indices, repeated at a fixed stride.
layout(std430, set = 0, binding = 7) readonly buffer ClusterBuffer {
    uint data[];
} clusterBuffer;

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

// How much of a point light reaches this fragment: 0 fully shadowed, 1 fully
// lit. Mirrors pointShadowReferenceDepth in render/PointShadows.cpp, which is
// where the formula is derived and tested.
float pointShadowFactor(Light light, vec3 worldPos) {
    int cube = int(light.params.x);
    if (cube < 0) {
        return 1.0;  // this light casts none
    }

    vec3 toFragment = worldPos - light.position.xyz;

    // The distance along the face's own axis, not the straight-line distance:
    // a perspective depth buffer stores the former, and comparing against the
    // latter makes the corner of every face shadow itself.
    vec3 magnitude = abs(toFragment);
    float axisDistance = max(magnitude.x, max(magnitude.y, magnitude.z));

    float near = ubo.pointShadowParams.x;
    float far = max(light.position.w, near + 1e-4);
    if (axisDistance <= near) {
        return 1.0;  // closer than the cube's near plane; nothing was recorded
    }

    float reference = far * (axisDistance - near) / ((far - near) * axisDistance);
    // A small offset along the depth range, for the same reason the sun's map
    // uses a polygon-offset bias: a surface compared against its own recorded
    // depth lands on either side of it by a texel's worth of slope.
    reference -= 0.0015;

    return texture(pointShadowMaps, vec4(toFragment, float(cube)), reference);
}

// How much of a spot's cone reaches this fragment: 1 inside the inner angle,
// 0 outside the outer one, smooth between. Mirrors spotConeAttenuation in
// render/SpotShadows.cpp, where the falloff's direction is pinned by a test -
// running it backwards gives a light bright at its rim and dark in its middle,
// which reads as a strange material rather than as a broken light.
float spotConeFactor(Light light, vec3 toFragment) {
    float cosOuter = light.direction.w;
    float cosInner = light.params.y;
    float cosAngle = dot(light.direction.xyz, normalize(toFragment));

    // A larger angle is a smaller cosine, so the inner one is the upper bound.
    float span = cosInner - cosOuter;
    if (span <= 1e-5) {
        return cosAngle >= cosOuter ? 1.0 : 0.0;
    }
    return clamp((cosAngle - cosOuter) / span, 0.0, 1.0);
}

// How much of a spot reaches this fragment through its shadow map: an
// ordinary projective lookup, the same shape as one cascade of the sun's.
float spotShadowFactor(Light light, vec3 worldPos) {
    int slot = int(light.params.x);
    if (slot < 0) {
        return 1.0;  // this light casts none
    }

    vec4 lightClip = ubo.spotShadowMatrices[slot] * vec4(worldPos, 1.0);
    if (lightClip.w <= 0.0) {
        return 1.0;  // behind the light, where its cone does not reach anyway
    }
    vec3 lightNdc = lightClip.xyz / lightClip.w;
    if (lightNdc.z < 0.0 || lightNdc.z > 1.0) {
        return 1.0;  // outside the map's depth range
    }

    vec2 uv = lightNdc.xy * 0.5 + 0.5;
    vec2 texelSize = 1.0 / vec2(textureSize(spotShadowMaps, 0).xy);

    // The same 3x3 kernel the sun's cascades use, each tap already 2x2
    // filtered by the hardware.
    float shadow = 0.0;
    for (int x = -1; x <= 1; x++) {
        for (int y = -1; y <= 1; y++) {
            shadow += texture(
                spotShadowMaps,
                vec4(uv + vec2(x, y) * texelSize, float(slot), lightNdc.z - 0.0015));
        }
    }
    return shadow / 9.0;
}

// Which cluster this fragment sits in.
//
// The tile comes from the pixel's own position on screen: gl_FragCoord has
// its origin at the top-left and Vulkan's NDC y points down, so the screen
// fraction here is the same one the culling shader turned into NDC. The slice
// comes from view depth through the scale-and-bias form of the exponential
// spacing, which is two instructions rather than a division and two
// logarithms. See ClusterGrid.cpp, where both are pinned by tests.
uint clusterForFragment(vec3 worldPos) {
    vec2 fraction = gl_FragCoord.xy / max(ubo.screenSize.xy, vec2(1.0));
    uvec2 tile = uvec2(fraction * vec2(ubo.clusterGrid.xy));
    tile = min(tile, ubo.clusterGrid.xy - 1u);

    // Positive distance along the camera's forward axis - the same measure
    // the cascades use, and for the same reason it is not negated here.
    float viewDepth = (ubo.view * vec4(worldPos, 1.0)).z;
    float slice =
        clamp(log2(max(viewDepth, 1e-6)) * ubo.clusterParams.x + ubo.clusterParams.y,
              0.0,
              float(ubo.clusterGrid.z - 1u));

    return tile.x + ubo.clusterGrid.x * (tile.y + ubo.clusterGrid.y * uint(slice));
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

    // Point lights, from this fragment's own cluster rather than from the
    // whole scene. The loop bound is how many lights reach here, not how many
    // exist - which is the entire reason the grid exists.
    uint cluster = clusterForFragment(fragPosWorld);
    uint base = cluster * (ubo.clusterGrid.w + 1u);
    uint count = min(clusterBuffer.data[base], ubo.clusterGrid.w);

    for (uint slot = 0u; slot < count; slot++) {
        Light light = lightBuffer.lights[clusterBuffer.data[base + 1u + slot]];

        vec3 toLight = light.position.xyz - fragPosWorld;
        float distanceSquared = max(dot(toLight, toLight), 1e-6);
        vec3 L = toLight * inversesqrt(distanceSquared);

        // Inverse-square falloff is shared; what differs between a point light
        // and a spot is the cone and which map its shadow lives in.
        float attenuation = 1.0 / distanceSquared;
        if (int(light.params.z) == LIGHT_TYPE_SPOT) {
            // The cone is tested against the direction from the light to the
            // fragment, which is the negation of the one used for shading.
            attenuation *= spotConeFactor(light, -toLight);
            if (attenuation > 0.0) {
                attenuation *= spotShadowFactor(light, fragPosWorld);
            }
        } else {
            attenuation *= pointShadowFactor(light, fragPosWorld);
        }

        vec3 radiance = light.color.rgb * light.color.w * attenuation;

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
