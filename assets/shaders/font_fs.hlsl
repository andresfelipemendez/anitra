// GPU vector font fragment shader
// Based on: GreenLightning/gpu-font-rendering (Will Dobbie / Eric Lengyel technique)
// Analytic anti-aliased winding number — iterates all curves per glyph (no grid)

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

// Keep all 4 bindings so pipeline descriptor stays valid
[[vk::binding(0, 2)]]
StructuredBuffer<BezierCurve> curves : register(t0);
[[vk::binding(1, 2)]]
StructuredBuffer<GlyphInfo> glyphs : register(t1);
[[vk::binding(2, 2)]]
StructuredBuffer<uint> _unused_grid_cells : register(t2);
[[vk::binding(3, 2)]]
ByteAddressBuffer _unused_grid_indices : register(t3);

struct VSOutput {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    nointerpolation uint glyph_id : BLENDINDICES0;
};

// Compute analytic coverage for one ray direction.
// p0, p1, p2 are curve points relative to pixel center.
// inverseDiameter = 1 / pixel_size_in_local_coords (for the ray axis).
float computeCoverage(float inverseDiameter, float2 p0, float2 p1, float2 p2) {
    // Early out: all control points on same side of ray
    if (p0.y > 0 && p1.y > 0 && p2.y > 0) return 0.0;
    if (p0.y < 0 && p1.y < 0 && p2.y < 0) return 0.0;

    // Quadratic Bezier: B(t) = (1-t)^2*p0 + 2*(1-t)*t*p1 + t^2*p2
    // Rewritten as: a*t^2 - 2*b*t + c where a = p0-2*p1+p2, b = p0-p1, c = p0
    float2 a = p0 - 2.0 * p1 + p2;
    float2 b = p0 - p1;
    float2 c = p0;

    float t0, t1;
    if (abs(a.y) >= 1e-5) {
        float radicand = b.y * b.y - a.y * c.y;
        if (radicand <= 0.0) return 0.0;
        float s = sqrt(radicand);
        t0 = (b.y - s) / a.y;
        t1 = (b.y + s) / a.y;
    } else {
        // Nearly linear segment
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

float2 rotate90(float2 v) {
    return float2(v.y, -v.x);
}

float4 PSMain(VSOutput input) : SV_Target {
    GlyphInfo glyph = glyphs[input.glyph_id];

    if (glyph.curve_count == 0)
        discard;

    // Map UV [0,1] to glyph-local coordinates
    // Note: stb_truetype Y-up convention — v=0 (top of screen quad) maps to bbox_max_y
    float2 bbox_size = float2(glyph.bbox_max_x - glyph.bbox_min_x,
                              glyph.bbox_max_y - glyph.bbox_min_y);
    float2 local_pos = float2(glyph.bbox_min_x + input.uv.x * bbox_size.x,
                              glyph.bbox_max_y - input.uv.y * bbox_size.y);

    // Pixel size in glyph-local coordinates (for AA window)
    float2 fw = fwidth(local_pos);
    float2 inverseDiameter = 1.0 / fw;

    float alpha = 0.0;

    // Iterate ALL curves for this glyph — required for correct winding numbers.
    // Grid acceleration is incorrect here because the winding number ray extends
    // to +infinity, needing curves far beyond the local grid cell.
    uint count = min(glyph.curve_count, 96u);
    for (uint i = 0; i < count; i++) {
        BezierCurve curve = curves[glyph.curve_offset + i];
        float2 p0 = float2(curve.p0x, curve.p0y) - local_pos;
        float2 p1 = float2(curve.p1x, curve.p1y) - local_pos;
        float2 p2 = float2(curve.p2x, curve.p2y) - local_pos;

        alpha += computeCoverage(inverseDiameter.x, p0, p1, p2);
        alpha += computeCoverage(inverseDiameter.y, rotate90(p0), rotate90(p1), rotate90(p2));
    }

    alpha *= 0.5;
    alpha = clamp(alpha, 0.0, 1.0);

    if (alpha < 0.001)
        discard;

    return float4(input.color.rgb, input.color.a * alpha);
}
