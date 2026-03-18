// Slug fragment shader — ported from Eric Lengyel's HLSL reference (MIT License)
// Evaluates quadratic Bezier curve intersections per-pixel for exact font coverage.

#version 450

layout(set=2, binding=0) uniform sampler2D curveTexture;   // RGBA32F: control points
layout(set=2, binding=1) uniform sampler2D bandTexture;    // RGBA32F: uint values stored as floats

layout(location=0) in vec4 v_color;
layout(location=1) in vec2 v_texcoord;
layout(location=2) flat in vec4 v_banding;
layout(location=3) flat in ivec4 v_glyph;

layout(location=0) out vec4 out_color;

#define kLogBandTextureWidth 12

/* Band texture stores uint values as floats — cast back to uint */
uvec4 bandFetch(ivec2 coord) {
    vec4 v = texelFetch(bandTexture, coord, 0);
    return uvec4(uint(v.x + 0.5), uint(v.y + 0.5), uint(v.z + 0.5), uint(v.w + 0.5));
}

uint calc_root_code(float y1, float y2, float y3) {
    uint i1 = floatBitsToUint(y1) >> 31u;
    uint i2 = floatBitsToUint(y2) >> 30u;
    uint i3 = floatBitsToUint(y3) >> 29u;

    uint shift = (i2 & 2u) | (i1 & ~2u);
    shift = (i3 & 4u) | (shift & ~4u);

    return (0x2E74u >> shift) & 0x0101u;
}

vec2 solve_horiz_poly(vec4 p12, vec2 p3) {
    vec2 a = p12.xy - p12.zw * 2.0 + p3;
    vec2 b = p12.xy - p12.zw;
    float ra = 1.0 / a.y;
    float rb = 0.5 / b.y;

    float d = sqrt(max(b.y * b.y - a.y * p12.y, 0.0));
    float t1 = (b.y - d) * ra;
    float t2 = (b.y + d) * ra;

    if (abs(a.y) < 1.0 / 65536.0) { t1 = p12.y * rb; t2 = t1; }

    return vec2((a.x * t1 - b.x * 2.0) * t1 + p12.x,
                (a.x * t2 - b.x * 2.0) * t2 + p12.x);
}

vec2 solve_vert_poly(vec4 p12, vec2 p3) {
    vec2 a = p12.xy - p12.zw * 2.0 + p3;
    vec2 b = p12.xy - p12.zw;
    float ra = 1.0 / a.x;
    float rb = 0.5 / b.x;

    float d = sqrt(max(b.x * b.x - a.x * p12.x, 0.0));
    float t1 = (b.x - d) * ra;
    float t2 = (b.x + d) * ra;

    if (abs(a.x) < 1.0 / 65536.0) { t1 = p12.x * rb; t2 = t1; }

    return vec2((a.y * t1 - b.y * 2.0) * t1 + p12.y,
                (a.y * t2 - b.y * 2.0) * t2 + p12.y);
}

ivec2 calc_band_loc(ivec2 glyph_loc, uint offset) {
    ivec2 band_loc = ivec2(glyph_loc.x + int(offset), glyph_loc.y);
    band_loc.y += band_loc.x >> kLogBandTextureWidth;
    band_loc.x &= (1 << kLogBandTextureWidth) - 1;
    return band_loc;
}

float calc_coverage(float xcov, float ycov, float xwgt, float ywgt) {
    float coverage = max(abs(xcov * xwgt + ycov * ywgt) / max(xwgt + ywgt, 1.0 / 65536.0),
                         min(abs(xcov), abs(ycov)));
    return clamp(coverage, 0.0, 1.0);
}

float slug_render(vec2 render_coord, vec4 band_transform, ivec4 glyph_data) {
    vec2 ems_per_pixel = fwidth(render_coord);
    vec2 pixels_per_em = 1.0 / ems_per_pixel;

    ivec2 band_max = glyph_data.zw;
    band_max.y &= 0x00FF;

    ivec2 band_index = clamp(ivec2(render_coord * band_transform.xy + band_transform.zw),
                              ivec2(0, 0), band_max);
    ivec2 glyph_loc = glyph_data.xy;

    float xcov = 0.0;
    float xwgt = 0.0;

    // Horizontal band processing
    uvec2 hband_data = bandFetch(ivec2(glyph_loc.x + band_index.y, glyph_loc.y)).xy;
    ivec2 hband_loc = calc_band_loc(glyph_loc, hband_data.y);

    for (int ci = 0; ci < int(hband_data.x); ci++) {
        ivec2 curve_loc = ivec2(bandFetch(ivec2(hband_loc.x + ci, hband_loc.y)).xy);

        vec4 p12 = texelFetch(curveTexture, curve_loc, 0) - vec4(render_coord, render_coord);
        vec2 p3 = texelFetch(curveTexture, ivec2(curve_loc.x + 1, curve_loc.y), 0).xy - render_coord;

        if (max(max(p12.x, p12.z), p3.x) * pixels_per_em.x < -0.5) break;

        uint code = calc_root_code(p12.y, p12.w, p3.y);
        if (code != 0u) {
            vec2 r = solve_horiz_poly(p12, p3) * pixels_per_em.x;

            if ((code & 1u) != 0u) {
                xcov += clamp(r.x + 0.5, 0.0, 1.0);
                xwgt = max(xwgt, clamp(1.0 - abs(r.x) * 2.0, 0.0, 1.0));
            }
            if (code > 1u) {
                xcov -= clamp(r.y + 0.5, 0.0, 1.0);
                xwgt = max(xwgt, clamp(1.0 - abs(r.y) * 2.0, 0.0, 1.0));
            }
        }
    }

    float ycov = 0.0;
    float ywgt = 0.0;

    // Vertical band processing
    uvec2 vband_data = bandFetch(ivec2(glyph_loc.x + band_max.y + 1 + band_index.x, glyph_loc.y)).xy;
    ivec2 vband_loc = calc_band_loc(glyph_loc, vband_data.y);

    for (int ci = 0; ci < int(vband_data.x); ci++) {
        ivec2 curve_loc = ivec2(bandFetch(ivec2(vband_loc.x + ci, vband_loc.y)).xy);

        vec4 p12 = texelFetch(curveTexture, curve_loc, 0) - vec4(render_coord, render_coord);
        vec2 p3 = texelFetch(curveTexture, ivec2(curve_loc.x + 1, curve_loc.y), 0).xy - render_coord;

        if (max(max(p12.y, p12.w), p3.y) * pixels_per_em.y < -0.5) break;

        uint code = calc_root_code(p12.x, p12.z, p3.x);
        if (code != 0u) {
            vec2 r = solve_vert_poly(p12, p3) * pixels_per_em.y;

            if ((code & 1u) != 0u) {
                ycov -= clamp(r.x + 0.5, 0.0, 1.0);
                ywgt = max(ywgt, clamp(1.0 - abs(r.x) * 2.0, 0.0, 1.0));
            }
            if (code > 1u) {
                ycov += clamp(r.y + 0.5, 0.0, 1.0);
                ywgt = max(ywgt, clamp(1.0 - abs(r.y) * 2.0, 0.0, 1.0));
            }
        }
    }

    return calc_coverage(xcov, ycov, xwgt, ywgt);
}

void main() {
    float coverage = slug_render(v_texcoord, v_banding, v_glyph);
    coverage = sqrt(coverage);  /* perceptual gamma correction (SLUG_WEIGHT) */
    out_color = vec4(v_color.rgb, v_color.a * coverage);
}
