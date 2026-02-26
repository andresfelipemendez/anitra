#version 450

struct BezierCurve {
    float p0x, p0y;
    float p1x, p1y;
    float p2x, p2y;
};

struct GlyphInfo {
    uint curve_offset;
    uint curve_count;
    float bbox_min_x, bbox_min_y;
    float bbox_max_x, bbox_max_y;
    float advance_width;
    float left_bearing;
    uint grid_offset;
    uint grid_cols;
    uint grid_rows;
};

layout(std430, set = 2, binding = 0) readonly buffer CurveBuffer {
    BezierCurve curves[];
};

layout(std430, set = 2, binding = 1) readonly buffer GlyphBuffer {
    GlyphInfo glyphs[];
};

layout(std430, set = 2, binding = 2) readonly buffer GridCellBuffer {
    uint grid_cells[];
};

layout(std430, set = 2, binding = 3) readonly buffer GridIndexBuffer {
    uint grid_indices[];
};

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_color;
layout(location = 2) flat in uint in_glyph_id;

layout(location = 0) out vec4 out_color;

float computeCoverage(float inverseDiameter, vec2 p0, vec2 p1, vec2 p2) {
    if (p0.y > 0.0 && p1.y > 0.0 && p2.y > 0.0) return 0.0;
    if (p0.y < 0.0 && p1.y < 0.0 && p2.y < 0.0) return 0.0;

    vec2 a = p0 - 2.0 * p1 + p2;
    vec2 b = p0 - p1;
    vec2 c = p0;

    float t0, t1;
    if (abs(a.y) >= 1e-5) {
        float radicand = b.y * b.y - a.y * c.y;
        if (radicand <= 0.0) return 0.0;
        float s = sqrt(radicand);
        t0 = (b.y - s) / a.y;
        t1 = (b.y + s) / a.y;
    } else {
        float t = p0.y / (p0.y - p2.y);
        if (p0.y < p2.y) {
            t0 = -1.0;
            t1 = t;
        } else {
            t0 = t;
            t1 = -1.0;
        }
    }

    float alpha = 0.0;

    if (t0 >= 0.0 && t0 < 1.0) {
        float x = (a.x * t0 - 2.0 * b.x) * t0 + c.x;
        alpha += clamp(x * inverseDiameter + 0.5, 0.0, 1.0);
    }

    if (t1 >= 0.0 && t1 < 1.0) {
        float x = (a.x * t1 - 2.0 * b.x) * t1 + c.x;
        alpha -= clamp(x * inverseDiameter + 0.5, 0.0, 1.0);
    }

    return alpha;
}

vec2 rotate90(vec2 v) {
    return vec2(v.y, -v.x);
}

void main() {
    GlyphInfo glyph = glyphs[in_glyph_id];

    if (glyph.curve_count == 0u)
        discard;

    vec2 bbox_size = vec2(glyph.bbox_max_x - glyph.bbox_min_x,
                          glyph.bbox_max_y - glyph.bbox_min_y);
    vec2 local_pos = vec2(glyph.bbox_min_x + in_uv.x * bbox_size.x,
                          glyph.bbox_max_y - in_uv.y * bbox_size.y);

    vec2 fw = fwidth(local_pos);

    // Stem darkening: tighten AA window so thin strokes appear bolder at small sizes.
    // At small px sizes fw is large (wide AA band) → scale up inverseDiameter to compensate.
    // The darkening factor ramps from 1.8x at tiny sizes down to 1.0x at large sizes.
    float avg_fw = 0.5 * (fw.x + fw.y);
    float darken = mix(1.8, 1.0, smoothstep(0.0, 0.02, avg_fw));
    vec2 inverseDiameter = darken / fw;

    float alpha = 0.0;

    uint count = min(glyph.curve_count, 96u);
    for (uint i = 0u; i < count; i++) {
        BezierCurve curve = curves[glyph.curve_offset + i];
        vec2 p0 = vec2(curve.p0x, curve.p0y) - local_pos;
        vec2 p1 = vec2(curve.p1x, curve.p1y) - local_pos;
        vec2 p2 = vec2(curve.p2x, curve.p2y) - local_pos;

        alpha += computeCoverage(inverseDiameter.x, p0, p1, p2);
        alpha += computeCoverage(inverseDiameter.y, rotate90(p0), rotate90(p1), rotate90(p2));
    }

    alpha *= 0.5;
    alpha = clamp(alpha, 0.0, 1.0);

    // Gamma correction: push mid-range alpha higher for perceptually bolder strokes
    alpha = pow(alpha, 0.6);

    if (alpha < 0.001)
        discard;

    out_color = vec4(in_color.rgb, in_color.a * alpha);
}
