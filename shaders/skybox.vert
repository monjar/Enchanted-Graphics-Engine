#version 450

// The fullscreen triangle again, but pinned to the far plane: with the depth
// test at LESS_OR_EQUAL against a buffer cleared to 1.0, the sky shades only
// the pixels no geometry claimed.

layout(location = 0) out vec2 fragNdc;

void main() {
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    fragNdc = uv * 2.0 - 1.0;
    gl_Position = vec4(fragNdc, 1.0, 1.0);
}
