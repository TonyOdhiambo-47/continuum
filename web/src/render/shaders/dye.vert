#version 300 es
precision highp float;

// Fullscreen triangle: positions are (-1,-1), (3,-1), (-1,3) in clip space.
// We derive UVs from clip-space so we don't need a vertex buffer at all.
out vec2 v_uv;
void main() {
    vec2 pos = vec2((gl_VertexID == 1) ? 3.0 : -1.0,
                    (gl_VertexID == 2) ? 3.0 : -1.0);
    v_uv = (pos + 1.0) * 0.5;
    gl_Position = vec4(pos, 0.0, 1.0);
}
