#version 450

layout(set = 2, binding = 0) uniform texture2D tex;
layout(set = 2, binding = 0) uniform sampler samp;

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_tint;

layout(location = 0) out vec4 out_color;

void main() {
    vec4 texColor = texture(sampler2D(tex, samp), in_uv);
    out_color = texColor * in_tint;
}
