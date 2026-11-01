#version 450
layout(location = 0) in vec4 inPos;
layout(location = 0) out vec4 outPos;
void main() {
    outPos = inPos;
    gl_Position = inPos;
}
