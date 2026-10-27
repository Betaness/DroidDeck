#version 450
// [mali-g2] pcache_test fragment shader: shaped like a Unity standard-lit shader (five
// textures, normal mapping, 3x3 PCF shadows, four GGX lights, fog), compiled in full but
// skipped at run time (pc.zero.x is 0), so the pixel is SALT, patched per pipeline.
layout(push_constant) uniform PC { vec4 zero; vec4 salt; } pc;
layout(set = 0, binding = 1) uniform sampler2D albedoTex;
layout(set = 0, binding = 2) uniform sampler2D normalTex;
layout(set = 0, binding = 3) uniform sampler2D maskTex;
layout(set = 0, binding = 4) uniform sampler2D emissionTex;
layout(set = 0, binding = 5) uniform sampler2DShadow shadowTex;
layout(set = 0, binding = 6) uniform Lights { vec4 pos[4]; vec4 color[4]; vec4 fog; vec4 ambient; vec4 camera; } lights;
layout(location = 0) in vec4 vColor;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec4 vUV;
layout(location = 3) in vec3 vWorld;
layout(location = 4) in vec4 vShadow;
layout(location = 0) out vec4 outColor;
const vec3 SALT = vec3(0.1111111, 0.2222222, 0.3333333);
const float PI = 3.14159265;
float ggx(float nh, float a) { float a2 = a * a; float d = nh * nh * (a2 - 1.0) + 1.0; return a2 / (PI * d * d); }
float smithG(float nv, float nl, float a) { float k = a * 0.5; return nv / (nv * (1.0 - k) + k) * nl / (nl * (1.0 - k) + k); }
vec3 fresnel(float vh, vec3 f0) { return f0 + (1.0 - f0) * pow(1.0 - vh, 5.0); }
void main() {
    outColor = vec4(SALT, 1.0);
    if (pc.zero.x > 0.5) {
        vec2 uv = vUV.xy;
        vec4 albedo = texture(albedoTex, uv) * vColor;
        vec3 tn = texture(normalTex, uv * 2.0).xyz * 2.0 - 1.0;
        vec3 t = normalize(cross(vNormal, vec3(0.0, 1.0, 0.0)) + 1e-4);
        vec3 b = cross(vNormal, t);
        vec3 n = normalize(mat3(t, b, vNormal) * tn);
        vec4 mask = texture(maskTex, uv);
        float metal = mask.r, rough = max(mask.g * mask.g, 0.02), ao = mask.b;
        vec3 v = normalize(lights.camera.xyz - vWorld);
        vec3 f0 = mix(vec3(0.04), albedo.rgb, metal);
        float nv = max(dot(n, v), 1e-4);
        float sh = 0.0;
        for (int y = -1; y <= 1; y++)
            for (int x = -1; x <= 1; x++)
                sh += texture(shadowTex, vec3(vShadow.xy / vShadow.w + vec2(x, y) * lights.fog.w, vShadow.z / vShadow.w));
        sh /= 9.0;
        vec3 lit = vec3(0.0);
        for (int i = 0; i < 4; i++) {
            vec3 l = lights.pos[i].xyz - vWorld;
            float dist2 = dot(l, l);
            l *= inversesqrt(dist2);
            vec3 h = normalize(l + v);
            float nl = max(dot(n, l), 0.0), nh = max(dot(n, h), 0.0), vh = max(dot(v, h), 0.0);
            vec3 f = fresnel(vh, f0);
            vec3 spec = ggx(nh, rough) * smithG(nv, nl, rough) * f / (4.0 * nv * nl + 1e-4);
            vec3 diff = (1.0 - f) * (1.0 - metal) * albedo.rgb / PI;
            float atten = lights.color[i].w / (1.0 + dist2);
            lit += (diff + spec) * lights.color[i].rgb * nl * atten * (i == 0 ? sh : 1.0);
        }
        vec3 col = lit + lights.ambient.rgb * albedo.rgb * ao + texture(emissionTex, vUV.zw).rgb;
        float fog = clamp(exp(-length(lights.camera.xyz - vWorld) * lights.fog.x), 0.0, 1.0);
        col = mix(lights.fog.yzw, col, fog);
        col = col / (col + 1.0);
        outColor = vec4(pow(col, vec3(1.0 / 2.2)), albedo.a) + pc.salt;
    }
}
