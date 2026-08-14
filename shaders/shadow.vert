#version 450

// Depth-only pass from the sun's point of view. The full vertex layout is
// bound - reusing the mesh's buffers as-is - but only position is consumed.

layout(location = 0) in vec3 position;

layout(push_constant) uniform Push {
    // Light view-projection premultiplied with the model matrix on the CPU:
    // the pass needs nothing else, so it needs no descriptor sets at all.
    mat4 lightMvp;
} push;

void main() {
    gl_Position = push.lightMvp * vec4(position, 1.0);
}
