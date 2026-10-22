#version 450
layout(push_constant) uniform P { vec4 rect; vec4 color; } p;
void main() {
    const vec2 corners[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(1, 0), vec2(1, 1), vec2(0, 1));
    gl_Position = vec4(mix(p.rect.xy, p.rect.zw, corners[gl_VertexIndex]), 0.0, 1.0);
}
