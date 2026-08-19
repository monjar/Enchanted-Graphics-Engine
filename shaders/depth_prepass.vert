#version 450
#extension GL_GOOGLE_include_directive : require

// Depth only, from the camera's point of view, before anything shades.
//
// Clustered forward shading runs the whole PBR fragment shader - image-based
// ambient, cascade lookup, every light in the fragment's cluster - for every
// fragment that survives the depth test. Whether a fragment survives depends
// on what has been drawn before it, so a scene drawn back to front shades
// each pixel as many times as it has layers of geometry. Laying depth down
// first and then shading with EQUAL makes that exactly once, whatever order
// the draws arrive in.
//
// The full vertex layout is bound - the mesh's buffers are used as they are -
// but only position is declared, because position is all that decides depth.

layout(location = 0) in vec3 position;

#include "model_push.glsl"

// See modelClipPosition: this is what lets the shading pass compare EQUAL
// against what this pass wrote.
invariant gl_Position;

void main() {
    vec4 positionWorld = push.modelMatrix * vec4(position, 1.0);
    gl_Position = modelClipPosition(positionWorld);
}
