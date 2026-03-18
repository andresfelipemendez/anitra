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
layout(location = 1) noperspective out vec2 out_screen_a;
layout(location = 2) noperspective out vec2 out_screen_b;
layout(location = 3) noperspective out float out_half_width;

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

    /* Screen-space positions in pixels (Y flipped to match gl_FragCoord: 0 at top) */
    vec2 pixel_a = vec2((ndc_a.x + 1.0) * 0.5 * screen_size.x,
                        (1.0 - ndc_a.y) * 0.5 * screen_size.y);
    vec2 pixel_b = vec2((ndc_b.x + 1.0) * 0.5 * screen_size.x,
                        (1.0 - ndc_b.y) * 0.5 * screen_size.y);

    vec2 dir = pixel_b - pixel_a;
    float len = length(dir);
    vec2 screen_dir = (len < 0.001) ? vec2(1.0, 0.0) : dir / len;
    vec2 perp = vec2(-screen_dir.y, screen_dir.x);

    /* Expand quad: line core + 1.5px fringe each side for AA */
    float expand_half = line_width * 0.5 + 1.5;
    vec2 offset_ndc = perp * in_side * expand_half / (screen_size * 0.5);

    gl_Position = clip_a;
    gl_Position.xy += offset_ndc * clip_a.w;

    out_color = in_color;
    out_screen_a = pixel_a;
    out_screen_b = pixel_b;
    out_half_width = line_width * 0.5;
}
