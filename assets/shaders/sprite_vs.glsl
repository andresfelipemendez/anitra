#version 450

layout(set = 1, binding = 0) uniform Constants {
    mat4 projection;
    mat4 view;
};

layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_tint;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_tint;

void main() {
    gl_Position = projection * view * vec4(in_pos, 0.0, 1.0);
    out_uv = in_uv;
    out_tint = in_tint;
}
