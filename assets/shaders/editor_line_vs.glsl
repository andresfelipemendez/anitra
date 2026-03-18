#version 450

layout(set = 1, binding = 0) uniform Constants {
    mat4 projection;
    mat4 view;
};

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_color;
layout(location = 2) in vec3 in_other_pos;
layout(location = 3) in float in_side;

layout(location = 0) out vec3 out_color;
layout(location = 1) out float out_dist;
layout(location = 2) out float out_edge;

layout(set = 1, binding = 1) uniform LineParams {
    vec2 screen_size;
    float line_width;
    float _pad;
};

void main() {
    mat4 vp = projection * view;
    vec4 clip_a = vp * vec4(in_pos, 1.0);
    vec4 clip_b = vp * vec4(in_other_pos, 1.0);

    vec2 ndc_a = clip_a.xy / clip_a.w;
    vec2 ndc_b = clip_b.xy / clip_b.w;

    vec2 dir = ndc_b - ndc_a;
    vec2 screen_dir = dir * screen_size * 0.5;
    float len = length(screen_dir);
    if (len < 0.001) screen_dir = vec2(1.0, 0.0);
    else screen_dir /= len;

    vec2 perp = vec2(-screen_dir.y, screen_dir.x);

    /* Expand by line_width + 1px AA fringe on each side */
    float expand_width = line_width + 1.0;
    vec2 offset = perp * in_side * expand_width * 0.5 / (screen_size * 0.5);

    gl_Position = clip_a;
    gl_Position.xy += offset * clip_a.w;

    out_color = in_color;
    out_dist = in_side;
    /* Edge threshold: the line core occupies this fraction of the quad */
    out_edge = line_width / expand_width;
}
