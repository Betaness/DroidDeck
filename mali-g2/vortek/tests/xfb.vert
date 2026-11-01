#version 450
// A vertex shader that writes its position to transform feedback buffer 0 (D3D11 stream output as
// DXVK compiles it: ExecutionMode Xfb, XfbBuffer/XfbStride/Offset on the output).
layout(location = 0) in vec4 inPos;
layout(xfb_buffer = 0, xfb_stride = 16, xfb_offset = 0, location = 0) out vec4 outPos;
void main() {
    outPos = inPos * 2.0;
    gl_Position = inPos;
}
