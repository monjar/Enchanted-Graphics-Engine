#version 450

// The procedural HDR environment: a clear sky with a sun, and a dark ground
// plane below the horizon. Generated at startup so a clean checkout has an
// environment to light with and reflect, without shipping a single asset
// file. An imported HDR environment replaces this when the asset pipeline
// lands.
//
// This scene's up axis is -Y (inherited from the tutorial camera), so
// "height above the horizon" is -direction.y.

layout(location = 0) in vec2 fragUv;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform Push {
    int faceIndex;
    float roughness;  // unused here; shared push layout across the IBL passes
} push;

// The Vulkan cube face direction convention: uv in [0,1] on face N maps to
// this world direction. Writing and reading with the same convention is what
// makes the maps consistent.
vec3 faceDirection(int face, vec2 uv) {
    vec2 st = uv * 2.0 - 1.0;
    if (face == 0) return vec3(1.0, -st.y, -st.x);   // +X
    if (face == 1) return vec3(-1.0, -st.y, st.x);   // -X
    if (face == 2) return vec3(st.x, 1.0, st.y);     // +Y
    if (face == 3) return vec3(st.x, -1.0, -st.y);   // -Y
    if (face == 4) return vec3(st.x, -st.y, 1.0);    // +Z
    return vec3(-st.x, -st.y, -1.0);                 // -Z
}

void main() {
    vec3 dir = normalize(faceDirection(push.faceIndex, fragUv));

    // Height above the horizon in [-1, 1]; -Y is up.
    float up = -dir.y;

    // Matches the demo's warm key light, which sits up and to the -X/-Z side.
    const vec3 sunDir = normalize(vec3(-0.6, -0.64, -0.48));
    const vec3 sunColor = vec3(1.0, 0.93, 0.82);

    vec3 radiance;
    if (up >= 0.0) {
        // Sky: horizon haze fading into a deeper zenith blue.
        const vec3 horizon = vec3(0.85, 0.90, 1.05);
        const vec3 zenith = vec3(0.18, 0.32, 0.62);
        radiance = mix(horizon, zenith, pow(up, 0.6));

        float cosSun = dot(dir, sunDir);
        // The disk carries most of the energy; the halo gives the prefiltered
        // mips something smooth to pick up at mid roughness.
        radiance += sunColor * 64.0 * smoothstep(0.9995, 0.9999, cosSun);
        radiance += sunColor * 0.5 * pow(max(cosSun, 0.0), 48.0);
    } else {
        // Ground: dark and slightly warm, brighter towards the horizon where
        // it catches more sky.
        const vec3 groundNear = vec3(0.35, 0.31, 0.27);
        const vec3 groundDeep = vec3(0.11, 0.10, 0.09);
        radiance = mix(groundNear, groundDeep, pow(-up, 0.5)) * 0.6;
    }

    outColor = vec4(radiance, 1.0);
}
