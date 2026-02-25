#version 450

layout(set = 1, binding = 0) uniform Constants {
    mat4 projection;
    mat4 view;
};

layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec3 in_color;

layout(location = 0) out vec3 out_color;

void main() {
    gl_Position = projection * view * vec4(in_pos, 0.0, 1.0);
    out_color = in_color;
}
