#version 450

layout(location = 0) in vec2 in_pos;   /* NDC-space position */
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;  /* tint multiplier */

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_color;

void main() {
    gl_Position = vec4(in_pos, 0.0, 1.0);
    out_uv = in_uv;
    out_color = in_color;
}
