// Slug vertex shader — simplified for 2D orthographic UI
// Dilation is done CPU-side by expanding quads by 1px before submission.

#version 450

layout(set=1, binding=0) uniform Constants {
    mat4 projection;
    mat4 view;
};

// 5 vertex attributes (pos, tex, jac, bnd, col)
layout(location=0) in vec4 in_pos;   // xy=position (screen space), zw=unused
layout(location=1) in vec4 in_tex;   // xy=em-space coords, z=glyph loc packed, w=band max packed
layout(location=2) in vec4 in_jac;   // unused (reserved for 3D dilation)
layout(location=3) in vec4 in_bnd;   // band scale xy, band offset xy
layout(location=4) in vec4 in_col;   // rgba color

layout(location=0) out vec4 v_color;
layout(location=1) out vec2 v_texcoord;
layout(location=2) flat out vec4 v_banding;
layout(location=3) flat out ivec4 v_glyph;

void main() {
    // Simple 2D ortho transform
    vec4 pos = projection * view * vec4(in_pos.xy, 0.0, 1.0);
    gl_Position = pos;

    v_texcoord = in_tex.xy;
    v_color = in_col;
    v_banding = in_bnd;

    // Unpack glyph data from tex.zw
    uvec2 g = floatBitsToUint(in_tex.zw);
    v_glyph = ivec4(g.x & 0xFFFFu, g.x >> 16u, g.y & 0xFFFFu, (g.y >> 16u) & 0xFFu);
}
