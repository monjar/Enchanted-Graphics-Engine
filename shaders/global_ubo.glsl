#ifndef EGE_GLOBAL_UBO_GLSL
#define EGE_GLOBAL_UBO_GLSL

// The per-frame global uniform block, shared by every shader that binds set 0.
//
// It lives in one file because it used to live in four, and they drifted: when
// the sun's single shadow matrix became an array of four, two of the copies
// were left describing the old layout. Nothing broke, only because those two
// read fields that sit before the change - which is luck, not design. A block
// declared once cannot drift, and the offsets a shader reads are the offsets
// the C++ side wrote.
//
// Mirrors GlobalUbo in src/core/Application.cpp. std140, so every vec4 and
// mat4 is 16-byte aligned and the trailing ints pack into one such slot.

// A light, point or spot. Both live in one buffer and go through one
// culling pass: a spot is a point light with a direction and a cone, so
// everything up to the shading itself is the same work on the same data.
const int LIGHT_TYPE_POINT = 0;
const int LIGHT_TYPE_SPOT = 1;

struct Light {
    vec4 position;  // xyz world position, w cull radius
    vec4 color;     // rgb color, w intensity
    // xyz: the direction a spot points, ignored for a point light.
    // w: cosine of its outer cone half-angle, past which it is dark.
    vec4 direction;
    // x: which slot of the shadow array holds this light's shadow, or -1 for
    //    a light that casts none.
    // y: cosine of the inner cone half-angle, inside which a spot is at full
    //    brightness.
    // z: the light's type, one of the constants above.
    vec4 params;
};

layout(set = 0, binding = 0) uniform GlobalUbo {
    mat4 projection;
    mat4 view;
    mat4 inverseView;
    mat4 inverseProjection;
    // One per cascade: the matrix each shadow map was rendered through.
    mat4 sunViewProjection[4];
    // Where each cascade ends, as a distance along the camera's forward axis.
    // Beyond the last one nothing is shadowed.
    vec4 cascadeSplits;
    vec4 sunDirection;  // xyz: direction the light travels; w unused
    vec4 sunColor;      // rgb color, w intensity; 0 disables the sun
    vec4 ambientLightColor;
    // Clustered shading. x: depth-slice scale, y: depth-slice bias,
    // z: clustering near plane, w: clustering far plane.
    vec4 clusterParams;
    // xyz: cells per axis, w: how many lights one cluster records.
    uvec4 clusterGrid;
    vec4 screenSize;  // xy: the extent the scene renders at
    // Point-light shadow cubes. x: the near plane every cube is rendered
    // with; the far plane is each light's own range, so it is read from the
    // light rather than from here.
    vec4 pointShadowParams;
    // One per shadow-casting spot: the matrix its map was rendered through.
    mat4 spotShadowMatrices[4];
    int numLights;
    int cascadeCount;
} ubo;

#endif  // EGE_GLOBAL_UBO_GLSL
