#version 450
layout(location = 0) in vec4 vcol;
layout(location = 0) out vec4 o;
layout(push_constant) uniform P { vec4 add; } p;
void main() { o = vcol + p.add; }
