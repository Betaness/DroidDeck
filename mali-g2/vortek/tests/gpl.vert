#version 450
layout(location = 0) in vec2 pos;
layout(location = 1) in vec4 col;
layout(location = 0) out vec4 vcol;
void main() { vcol = col; gl_Position = vec4(pos, 0.0, 1.0); }
