#version 450

layout(location = 0) in vec3 in_color;
layout(location = 1) in float in_dist;
layout(location = 2) in float in_edge;

layout(location = 0) out vec4 out_color;

void main() {
    float d = abs(in_dist);
    /* Core of the line is fully opaque up to in_edge, then fades to 0 at the quad edge */
    float alpha = 1.0 - smoothstep(in_edge, 1.0, d);
    out_color = vec4(in_color, alpha * 0.8);
}
