#version 450
// Full-screen triangle at a constant depth (push constant), for m7ul_test.c.
layout(push_constant) uniform PC { float depth; } pc;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, pc.depth, 1.0);
}
