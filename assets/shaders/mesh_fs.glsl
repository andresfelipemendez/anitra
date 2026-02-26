#version 450

layout(set = 2, binding = 0) uniform texture2D base_color_tex;
layout(set = 2, binding = 0) uniform sampler base_color_samp;

layout(location = 0) in vec3 in_normal;
layout(location = 1) in vec2 in_uv;

layout(location = 0) out vec4 out_color;

void main() {
    vec3 N = normalize(in_normal);
    vec3 light_dir = normalize(vec3(0.5, 1.0, 0.3));
    float ndotl = max(dot(N, light_dir), 0.0);
    float lighting = 0.3 + 0.7 * ndotl;

    vec4 albedo = texture(sampler2D(base_color_tex, base_color_samp), in_uv);
    out_color = vec4(albedo.rgb * lighting, albedo.a);
}
