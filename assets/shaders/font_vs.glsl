#version 450

layout(set = 1, binding = 0) uniform Constants {
    mat4 projection;
    mat4 view;
};

layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;
layout(location = 3) in uint in_glyph_id;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_color;
layout(location = 2) flat out uint out_glyph_id;

void main() {
    gl_Position = projection * view * vec4(in_pos, 0.0, 1.0);
    out_uv = in_uv;
    out_color = in_color;
    out_glyph_id = in_glyph_id;
}
