#version 450
// Writes a clip and a cull distance, which the Mali shader compiler crashes on (spirv_clip_cull.h).
out gl_PerVertex {
    vec4 gl_Position;
    float gl_ClipDistance[2];
    float gl_CullDistance[1];
};
layout(location = 0) in vec4 position;
void main() {
    gl_Position = position;
    gl_ClipDistance[0] = position.x;
    gl_ClipDistance[1] = position.y;
    gl_CullDistance[0] = position.z;
}
