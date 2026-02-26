#version 450

layout(set = 1, binding = 0) uniform Uniforms {
    mat4 projection;
    mat4 view;
    mat4 model;
};

layout(std430, set = 0, binding = 0) readonly buffer BoneBuffer {
    mat4 bones[];
};

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in uvec4 in_bone_ids;
layout(location = 4) in vec4 in_bone_weights;

layout(location = 0) out vec3 out_normal;
layout(location = 1) out vec2 out_uv;

void main() {
    mat4 skin_mat = bones[in_bone_ids.x] * in_bone_weights.x
                  + bones[in_bone_ids.y] * in_bone_weights.y
                  + bones[in_bone_ids.z] * in_bone_weights.z
                  + bones[in_bone_ids.w] * in_bone_weights.w;

    vec4 skinned_pos = skin_mat * vec4(in_position, 1.0);
    out_normal = mat3(model) * mat3(skin_mat) * in_normal;
    out_uv = in_uv;
    gl_Position = projection * view * model * skinned_pos;
}
