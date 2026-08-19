#version 450
#extension GL_GOOGLE_include_directive : require

// Screen-space ambient occlusion, from the depth buffer alone.
//
// Around each fragment, points are sampled in the hemisphere over its surface
// and tested against the depth the pre-pass recorded. A sample that turns out
// to sit behind the geometry already there is one whose direction is blocked;
// the fraction blocked is how much of its surroundings the point cannot see,
// and the shading pass uses it to dim the ambient term.
//
// See src/render/Ssao.hpp, where the reconstruction below is defined and
// tested against the engine's own camera. It is the part that most published
// SSAO gets wrong here: this engine's view space looks down +Z, not -Z, and
// Vulkan's depth range is [0, 1], not [-1, 1].

layout(location = 0) in vec2 fragUv;

layout(location = 0) out float outOcclusion;

#include "global_ubo.glsl"

// Depth as the pre-pass left it, resolved to one sample per pixel. Sampled
// with a nearest filter: an averaged depth is a surface that was never there.
layout(set = 0, binding = 1) uniform sampler2D depthMap;

// A small tile of rotations, repeated across the screen.
layout(set = 0, binding = 2) uniform sampler2D noiseMap;

// Mirrors ssaoMaxSamples and ssaoNoiseSize in src/render/Ssao.hpp.
const int SSAO_MAX_SAMPLES = 64;
const int SSAO_NOISE_SIZE = 4;

layout(set = 0, binding = 3) uniform SsaoKernel {
    // Offsets in tangent space, +Z being the surface normal.
    vec4 samples[SSAO_MAX_SAMPLES];
    // x: radius in world units, y: depth bias, z: strength exponent,
    // w: how many of the samples above are in use.
    vec4 params;
} kernel;

// Where the depth at a point on screen sits in view space. Mirrors
// viewPositionFromDepth in render/Ssao.cpp - ndc.z is the stored depth used
// exactly as it comes, and the divide by w is what undoes the projection.
vec3 viewPositionAt(vec2 uv, float depth) {
    vec4 position = ubo.inverseProjection * vec4(uv * 2.0 - 1.0, depth, 1.0);
    return position.xyz / position.w;
}

vec3 viewPositionAt(vec2 uv) {
    return viewPositionAt(uv, texture(depthMap, uv).r);
}

void main() {
    float centreDepth = texture(depthMap, fragUv).r;
    // Nothing was drawn here, so this is sky. The sky is not a surface and
    // has nothing to be occluded.
    if (centreDepth >= 1.0) {
        outOcclusion = 1.0;
        return;
    }

    vec3 P = viewPositionAt(fragUv, centreDepth);

    // The surface normal, from how the reconstructed position changes across
    // the pixel quad. There is no normal buffer to read - the pre-pass writes
    // depth and nothing else - and derivatives of a position are exactly the
    // two tangents of the surface it lies on.
    vec3 N = normalize(cross(dFdx(P), dFdy(P)));
    // The camera sits at the view-space origin, so the direction from this
    // surface to it is -P. Whichever way the cross product came out, the
    // normal is the side facing the camera: this is the one line that would
    // otherwise depend on which way view space points.
    if (dot(N, P) > 0.0) {
        N = -N;
    }

    // A tangent frame chosen without reference to the noise, so that it cannot
    // come out degenerate however the rotation lands. Any vector not parallel
    // to the normal will do to start from.
    vec3 reference = abs(N.z) < 0.9 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 T = normalize(cross(reference, N));
    vec3 B = cross(N, T);
    mat3 tangentToView = mat3(T, B, N);

    // The kernel is the same for every pixel, so without this the same few
    // directions are tested everywhere and the estimate's error shows up as
    // bands. Turning it by a per-pixel angle turns those bands into noise,
    // which the blur that follows averages away.
    vec2 noiseUv = fragUv * ubo.screenSize.xy / float(SSAO_NOISE_SIZE);
    vec2 rotation = texture(noiseMap, noiseUv).xy * 2.0 - 1.0;

    float radius = kernel.params.x;
    float bias = kernel.params.y;
    int count = min(int(kernel.params.w), SSAO_MAX_SAMPLES);

    float occluded = 0.0;
    for (int i = 0; i < count; i++) {
        // Turned about the normal by this pixel's angle, which in tangent
        // space is a rotation in the xy plane and leaves z - the component
        // along the normal - alone.
        vec3 offset = kernel.samples[i].xyz;
        offset = vec3(
            offset.x * rotation.x - offset.y * rotation.y,
            offset.x * rotation.y + offset.y * rotation.x,
            offset.z);

        vec3 samplePosition = P + tangentToView * offset * radius;

        vec4 clip = ubo.projection * vec4(samplePosition, 1.0);
        if (clip.w <= 0.0) {
            continue;  // behind the camera, where the depth buffer says nothing
        }
        vec2 sampleUv = (clip.xy / clip.w) * 0.5 + 0.5;
        if (any(lessThan(sampleUv, vec2(0.0))) || any(greaterThan(sampleUv, vec2(1.0)))) {
            continue;  // off screen, and off screen is not the same as open
        }

        float sceneDepth = viewPositionAt(sampleUv).z;

        // Nearer to the camera than the point being tested means something
        // stands between it and the surface. Depth grows away from the camera
        // in this engine's view space, which is what makes this a <= and not
        // a >= - see the ordering test in tests/test_ssao.cpp.
        if (sceneDepth <= samplePosition.z - bias) {
            // An occluder much closer than the radius is a different object
            // altogether, not part of this point's neighbourhood. Without
            // this, every foreground silhouette paints a dark halo onto the
            // wall behind it.
            float withinRange = smoothstep(0.0, 1.0, radius / max(abs(P.z - sceneDepth), 1e-4));
            occluded += withinRange;
        }
    }

    float visibility = 1.0 - occluded / max(float(count), 1.0);
    outOcclusion = pow(clamp(visibility, 0.0, 1.0), kernel.params.z);
}
