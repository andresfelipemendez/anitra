#version 450

layout(location = 0) in vec3 in_color;
layout(location = 1) noperspective in vec2 in_screen_a;
layout(location = 2) noperspective in vec2 in_screen_b;
layout(location = 3) noperspective in float in_half_width;

layout(location = 0) out vec4 out_color;

void main() {
    /* Exact perpendicular distance from fragment to line segment in pixels */
    vec2 p = gl_FragCoord.xy;
    vec2 ab = in_screen_b - in_screen_a;
    float len2 = dot(ab, ab);
    float t = (len2 > 0.0001) ? clamp(dot(p - in_screen_a, ab) / len2, 0.0, 1.0) : 0.0;
    vec2 closest = in_screen_a + t * ab;
    float dist = length(p - closest);

    /* Smooth 1px transition from line edge */
    float alpha = 1.0 - smoothstep(in_half_width, in_half_width + 1.0, dist);
    out_color = vec4(in_color, alpha * 0.8);
}
