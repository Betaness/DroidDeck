#version 450
// A pass-through geometry shader without stream output.
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
layout(location = 0) in vec4 inPos[];
layout(location = 0) out vec4 outPos;
void main() {
    for (int i = 0; i < 3; i++) {
        outPos = inPos[i];
        gl_Position = gl_in[i].gl_Position;
        EmitVertex();
    }
    EndPrimitive();
}
