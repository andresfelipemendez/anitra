#version 450

layout(set = 2, binding = 0) uniform sampler2D atlas;

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_color;

layout(location = 0) out vec4 out_color;

float msdf_median(float r, float g, float b) {
    return max(min(r, g), min(max(r, g), b));
}

void main() {
    vec3 s = texture(atlas, in_uv).rgb;
    float sd = msdf_median(s.r, s.g, s.b) - 0.5;

    /* Compute screen_px_range: how many screen pixels span the MSDF pixel range.
       This is Chlumsky's msdfgen technique — gives a precise 1px transition. */
    const float pixel_range = 14.0;
    vec2 unit_range = vec2(pixel_range) / vec2(textureSize(atlas, 0));
    vec2 screen_tex_size = vec2(1.0) / fwidth(in_uv);
    float screen_px_range = max(0.5 * dot(unit_range, screen_tex_size), 1.0);

    float alpha = clamp(sd * screen_px_range + 0.5, 0.0, 1.0);

    if (alpha < 0.001)
        discard;

    out_color = vec4(in_color.rgb, in_color.a * alpha);
}
