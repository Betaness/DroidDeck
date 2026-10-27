#version 450
// [mali-g2] pcache_test vertex shader: a skinned-mesh shape compiled in full, run as a
// full-screen triangle (pc.zero.x is 0). SALT_V is patched per pipeline by the test.
layout(push_constant) uniform PC { vec4 zero; vec4 salt; } pc;
layout(set = 0, binding = 0) uniform UBO { mat4 bones[48]; mat4 viewProj; mat4 world; vec4 params[16]; } ubo;
layout(location = 0) out vec4 vColor;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec4 vUV;
layout(location = 3) out vec3 vWorld;
layout(location = 4) out vec4 vShadow;
const vec3 SALT_V = vec3(0.4444444, 0.5555555, 0.6666666);
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0;
    vec4 pos = vec4(p, 0.0, 1.0);
    vec3 n = vec3(0.0, 0.0, 1.0);
    vec4 uv = vec4(p * 0.5 + 0.5, 0.0, 0.0);
    vec3 world = pos.xyz;
    vec4 shadow = vec4(0.0);
    if (pc.zero.x > 0.5) {
        ivec4 idx = ivec4(pc.zero.yzwx * 47.0);
        vec4 w = normalize(pc.zero + vec4(1.0));
        mat4 skin = ubo.bones[idx.x] * w.x + ubo.bones[idx.y] * w.y + ubo.bones[idx.z] * w.z + ubo.bones[idx.w] * w.w;
        vec4 wp = ubo.world * (skin * vec4(p, pc.zero.y, 1.0));
        world = wp.xyz;
        n = normalize(mat3(ubo.world) * mat3(skin) * vec3(pc.zero.xy, 1.0));
        for (int i = 0; i < 4; i++) {
            vec4 m = ubo.params[i];
            world += m.xyz * sin(dot(world, m.xyz) * m.w + ubo.params[i + 4].x);
            uv += ubo.params[i + 8] * dot(n, m.xyz);
        }
        pos = ubo.viewProj * vec4(world, 1.0);
        shadow = ubo.params[12] * vec4(world, 1.0) + ubo.params[13];
        uv.zw = pos.xy / max(pos.w, 1e-3) * ubo.params[14].xy + ubo.params[14].zw;
    }
    gl_Position = pos;
    vColor = vec4(SALT_V, 1.0);
    vNormal = n;
    vUV = uv;
    vWorld = world;
    vShadow = shadow;
}
