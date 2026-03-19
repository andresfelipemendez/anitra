// Slug fragment shader — ported from Eric Lengyel's HLSL reference (MIT License)
// Evaluates quadratic Bezier curve intersections per-pixel for exact font coverage.
// Stroke mode: when v_stroke_width > 0, computes distance-to-curve for constant-pixel-width strokes.

#version 450

layout(set=2, binding=0) uniform sampler2D curveTexture;   // RGBA32F: control points
layout(set=2, binding=1) uniform sampler2D bandTexture;    // RGBA32F: uint values stored as floats

layout(location=0) in vec4 v_color;
layout(location=1) in vec2 v_texcoord;
layout(location=2) flat in vec4 v_banding;
layout(location=3) flat in ivec4 v_glyph;
layout(location=4) flat in float v_stroke_width; /* stroke width in pixels, 0=filled */
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

/* Unsigned distance from point to quadratic Bezier curve.
   Based on Inigo Quilez's analytical solution.
   Handles degenerate case (straight line) when b ≈ 0. */
float ud_bezier(vec2 pos, vec2 A, vec2 B, vec2 C) {
    vec2 a = B - A;
    vec2 b = A - 2.0*B + C;
    vec2 c = a * 2.0;
    vec2 d = A - pos;

    float bb = dot(b, b);
    /* Degenerate: curve is a straight line segment A→C */
    if (bb < 1e-8) {
        vec2 ac = C - A;
        float acl = dot(ac, ac);
        if (acl < 1e-8) return length(A - pos); /* point */
        float t = clamp(dot(pos - A, ac) / acl, 0.0, 1.0);
        return length(pos - A - ac * t);
    }

    float kk = 1.0 / bb;
    float kx = kk * dot(a, b);
    float ky = kk * (2.0*dot(a,a) + dot(d,b)) / 3.0;
    float kz = kk * dot(d, a);

    float p = ky - kx*kx;
    float q = kx*(2.0*kx*kx - 3.0*ky) + kz;
    float p3 = p*p*p;
    float h = q*q + 4.0*p3;

    float res;
    if (h >= 0.0) {
        h = sqrt(h);
        vec2 x = (vec2(h, -h) - q) / 2.0;
        vec2 uv = sign(x) * pow(abs(x), vec2(1.0/3.0));
        float t = clamp(uv.x + uv.y - kx, 0.0, 1.0);
        vec2 qp = d + (c + b*t)*t;
        res = dot(qp, qp);
    } else {
        float z = sqrt(-p);
        float v = acos(q/(p*z*2.0)) / 3.0;
        float m = cos(v);
        float n = sin(v) * 1.732050808;
        vec3 t3 = clamp(vec3(m+m, -n-m, n-m)*z - kx, 0.0, 1.0);
        vec2 qp1 = d + (c + b*t3.x)*t3.x;
        vec2 qp2 = d + (c + b*t3.y)*t3.y;
        res = min(dot(qp1, qp1), dot(qp2, qp2));
    }
    return sqrt(res);
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

/* Distance-based stroke: compute min distance to any curve, threshold at pixel width */
float stroke_render(vec2 render_coord, vec4 band_transform, ivec4 glyph_data, float stroke_px) {
    vec2 ems_per_pixel = fwidth(render_coord);
    vec2 pixels_per_em = 1.0 / ems_per_pixel;
    float px_per_em = (pixels_per_em.x + pixels_per_em.y) * 0.5;

    ivec2 band_max = glyph_data.zw;
    band_max.y &= 0x00FF;
    ivec2 band_index = clamp(ivec2(render_coord * band_transform.xy + band_transform.zw),
                              ivec2(0, 0), band_max);
    ivec2 glyph_loc = glyph_data.xy;

    float min_dist = 1e10;

    // Check curves from horizontal band
    uvec2 hband_data = bandFetch(ivec2(glyph_loc.x + band_index.y, glyph_loc.y)).xy;
    ivec2 hband_loc = calc_band_loc(glyph_loc, hband_data.y);
    for (int ci = 0; ci < int(hband_data.x); ci++) {
        ivec2 curve_loc = ivec2(bandFetch(ivec2(hband_loc.x + ci, hband_loc.y)).xy);
        vec4 p12 = texelFetch(curveTexture, curve_loc, 0);
        vec2 p3 = texelFetch(curveTexture, ivec2(curve_loc.x + 1, curve_loc.y), 0).xy;
        min_dist = min(min_dist, ud_bezier(render_coord, p12.xy, p12.zw, p3));
    }

    // Check curves from vertical band
    uvec2 vband_data = bandFetch(ivec2(glyph_loc.x + band_max.y + 1 + band_index.x, glyph_loc.y)).xy;
    ivec2 vband_loc = calc_band_loc(glyph_loc, vband_data.y);
    for (int ci = 0; ci < int(vband_data.x); ci++) {
        ivec2 curve_loc = ivec2(bandFetch(ivec2(vband_loc.x + ci, vband_loc.y)).xy);
        vec4 p12 = texelFetch(curveTexture, curve_loc, 0);
        vec2 p3 = texelFetch(curveTexture, ivec2(curve_loc.x + 1, curve_loc.y), 0).xy;
        min_dist = min(min_dist, ud_bezier(render_coord, p12.xy, p12.zw, p3));
    }

    float dist_px = min_dist * px_per_em;
    float half_stroke = stroke_px * 0.5;
    return 1.0 - smoothstep(half_stroke - 0.7, half_stroke + 0.7, dist_px);
}

void main() {
    float coverage;
    if (v_stroke_width > 0.0) {
        coverage = stroke_render(v_texcoord, v_banding, v_glyph, v_stroke_width);
    } else {
        coverage = slug_render(v_texcoord, v_banding, v_glyph);
        coverage = sqrt(coverage);  /* perceptual gamma correction (SLUG_WEIGHT) */
    }
    out_color = vec4(v_color.rgb, v_color.a * coverage);
}
