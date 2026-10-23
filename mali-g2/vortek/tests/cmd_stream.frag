#version 450
layout(push_constant) uniform P { vec4 rect; vec4 color; } p;
layout(location = 0) out vec4 o;
void main() { o = p.color; }
