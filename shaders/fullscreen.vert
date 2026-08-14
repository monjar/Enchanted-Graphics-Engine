#version 450

// A fullscreen triangle from gl_VertexIndex alone - no vertex buffer bound at
// all. One oversized triangle rather than a two-triangle quad, because the
// quad's shared diagonal would run every pixel along it through the fragment
// shader twice.
//
// index 0 -> uv (0,0), position (-1,-1)
// index 1 -> uv (2,0), position ( 3,-1)
// index 2 -> uv (0,2), position (-1, 3)

layout(location = 0) out vec2 fragUv;

void main() {
    fragUv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(fragUv * 2.0 - 1.0, 0.0, 1.0);
}
