/*
 * slug_font.h — Slug font curve packer (single-header, #define SLUG_FONT_IMPL)
 *
 * Extracts quadratic Bezier curves from TTF glyphs via stb_truetype,
 * assigns to horizontal/vertical bands, and packs into GPU texture data
 * for the Slug rendering algorithm.
 *
 * Reference: Eric Lengyel, "GPU-Centered Font Rendering Directly from Glyph Outlines"
 * Journal of Computer Graphics Techniques, 2017. MIT License.
 */

#ifndef SLUG_FONT_H
#define SLUG_FONT_H

#include <stdint.h>

/* ── Configuration ──────────────────────────────────────────────────────── */

#define SLUG_BAND_TEX_WIDTH     4096
#define SLUG_LOG_BAND_TEX_WIDTH 12
#define SLUG_MAX_CURVES_PER_GLYPH 512
#define SLUG_DEFAULT_HBANDS     16
#define SLUG_DEFAULT_VBANDS     16

/* ── Data structures ────────────────────────────────────────────────────── */

/* A quadratic Bezier curve: C(t) = (1-t)^2 p1 + 2t(1-t) p2 + t^2 p3 */
typedef struct slug_curve {
    float p1x, p1y;
    float p2x, p2y;
    float p3x, p3y;
} slug_curve;

/* Per-glyph metadata */
typedef struct slug_glyph {
    /* Bounding box in em-space */
    float bbox_x0, bbox_y0, bbox_x1, bbox_y1;
    /* Band parameters: scale and offset to map em-coords to band index */
    float band_scale_x, band_scale_y;
    float band_offset_x, band_offset_y;
    /* Location of this glyph's band data in the band texture (texel coords) */
    uint16_t band_loc_x, band_loc_y;
    /* Max band indices */
    uint8_t band_max_x, band_max_y;
    /* Advance width in em-space */
    float advance;
    /* Left side bearing in em-space */
    float lsb;
    /* Number of curves in this glyph */
    int curve_count;
    /* Set to 1 if glyph has data */
    int valid;
} slug_glyph;

/* Complete packed font data ready for GPU upload */
typedef struct slug_font_data {
    /* Curve texture data: pairs of float4 per curve
       [i*2+0] = (p1.x, p1.y, p2.x, p2.y)
       [i*2+1] = (p3.x, p3.y, 0, 0) */
    float *curve_texels;       /* float4 array, length = curve_texel_count * 4 */
    int curve_texel_count;     /* number of float4 texels */
    int curve_tex_height;      /* computed from data size */

    /* Band texture data: uint4 stored as raw bits in RGBA32F texture */
    uint32_t *band_texels;     /* uint4 array, length = band_texel_count * 4 */
    int band_texel_count;      /* number of uint4 texels */
    int band_tex_height;       /* computed from data size */

    /* Per-glyph metadata */
    slug_glyph glyphs[128];    /* ASCII range (used by slug_build_font) */
    /* Arbitrary codepoint glyphs (used by slug_build_font_for_codepoints) */
    uint32_t *codepoints;      /* parallel to cp_glyphs */
    slug_glyph *cp_glyphs;
    int cp_glyph_count;
    int glyph_count;

    /* Font metrics (em-space, normalized to 1.0 em) */
    float ascent, descent, line_gap;
    int units_per_em;
} slug_font_data;

/* ── API ────────────────────────────────────────────────────────────────── */

/*
 * Extract quadratic Bezier curves from a TTF glyph using stb_truetype.
 * Cubic curves are split into two quadratics.
 * Returns number of curves extracted into out_curves.
 */
static int slug_extract_glyph_curves(const stbtt_fontinfo *font, int glyph_index,
                                      float em_scale, slug_curve *out_curves, int max_curves);

/*
 * Build complete Slug font data from a TTF font.
 * Extracts curves for ASCII 32-126, assigns to bands, packs textures.
 * Caller must free result with slug_font_data_free().
 */
static slug_font_data *slug_build_font(const unsigned char *ttf_data, int ttf_len,
                                        int num_hbands, int num_vbands);

static slug_font_data *slug_build_font_for_codepoints(const unsigned char *ttf_data, int ttf_len,
                                                       const uint32_t *codepoints, int cp_count,
                                                       int num_hbands, int num_vbands);

static void slug_font_data_free(slug_font_data *data);

/*
 * Save/load packed font data to/from disk (cache).
 * Uses TTF fingerprint for invalidation.
 */
static int slug_cache_save(const slug_font_data *data, const char *path, uint32_t ttf_fingerprint);
static int slug_cache_load(slug_font_data *data, const char *path, uint32_t ttf_fingerprint);

/* ── Implementation ─────────────────────────────────────────────────────── */

#ifdef SLUG_FONT_IMPL

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

/* ── Curve extraction ───────────────────────────────────────────────────── */

static int slug_extract_glyph_curves(const stbtt_fontinfo *font, int glyph_index,
                                      float em_scale, slug_curve *out_curves, int max_curves) {
    stbtt_vertex *verts = NULL;
    int nv, i, count = 0;
    float cx = 0, cy = 0;

    nv = stbtt_GetGlyphShape(font, glyph_index, &verts);
    if (nv <= 0) return 0;

    for (i = 0; i < nv && count < max_curves; i++) {
        float x = verts[i].x * em_scale;
        float y = verts[i].y * em_scale;

        switch (verts[i].type) {
        case STBTT_vmove:
            cx = x; cy = y;
            break;

        case STBTT_vline:
            /* Line → degenerate quadratic with midpoint as control */
            if (count < max_curves) {
                slug_curve *c = &out_curves[count++];
                c->p1x = cx; c->p1y = cy;
                c->p2x = (cx + x) * 0.5f; c->p2y = (cy + y) * 0.5f;
                c->p3x = x; c->p3y = y;
            }
            cx = x; cy = y;
            break;

        case STBTT_vcurve: {
            /* Quadratic Bezier — direct */
            float cpx = verts[i].cx * em_scale;
            float cpy = verts[i].cy * em_scale;
            if (count < max_curves) {
                slug_curve *c = &out_curves[count++];
                c->p1x = cx; c->p1y = cy;
                c->p2x = cpx; c->p2y = cpy;
                c->p3x = x; c->p3y = y;
            }
            cx = x; cy = y;
            break;
        }

        case STBTT_vcubic: {
            /* Cubic → split into 2 quadratics (same approach as msdf_gen.h) */
            float c1x = verts[i].cx * em_scale, c1y = verts[i].cy * em_scale;
            float c2x = verts[i].cx1 * em_scale, c2y = verts[i].cy1 * em_scale;
            float splitx = (c1x + c2x) * 0.5f;
            float splity = (c1y + c2y) * 0.5f;
            if (count < max_curves) {
                slug_curve *c = &out_curves[count++];
                c->p1x = cx; c->p1y = cy;
                c->p2x = (cx + 3*c1x) * 0.25f;
                c->p2y = (cy + 3*c1y) * 0.25f;
                c->p3x = splitx; c->p3y = splity;
            }
            if (count < max_curves) {
                slug_curve *c = &out_curves[count++];
                c->p1x = splitx; c->p1y = splity;
                c->p2x = (3*c2x + x) * 0.25f;
                c->p2y = (3*c2y + y) * 0.25f;
                c->p3x = x; c->p3y = y;
            }
            cx = x; cy = y;
            break;
        }
        }
    }

    stbtt_FreeShape(font, verts);
    return count;
}

/* ── Band assignment helpers ────────────────────────────────────────────── */

static float slug_minf(float a, float b) { return a < b ? a : b; }
static float slug_maxf(float a, float b) { return a > b ? a : b; }
static float slug_min3f(float a, float b, float c) { return slug_minf(a, slug_minf(b, c)); }
static float slug_max3f(float a, float b, float c) { return slug_maxf(a, slug_maxf(b, c)); }
static int slug_clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* Comparison for sorting curve indices by descending max-x (horizontal bands) */
typedef struct slug_sort_entry { int curve_idx; float sort_key; } slug_sort_entry;

static int slug_sort_cmp_desc(const void *a, const void *b) {
    float ka = ((const slug_sort_entry *)a)->sort_key;
    float kb = ((const slug_sort_entry *)b)->sort_key;
    return (ka < kb) ? 1 : (ka > kb) ? -1 : 0;
}

/* ── Band assignment and texture packing ────────────────────────────────── */

/*
 * Pack one glyph's curves into bands.
 * Writes band headers + curve location lists into band_out.
 * Writes curve control points into curve_out.
 * Returns number of band texels consumed.
 */
static int slug_pack_glyph(const slug_curve *curves, int curve_count,
                            float bbox_x0, float bbox_y0, float bbox_x1, float bbox_y1,
                            int num_hbands, int num_vbands,
                            /* curve texture output */
                            float *curve_out, int *curve_texel_pos,
                            /* band texture output */
                            uint32_t *band_out, int *band_texel_pos,
                            /* glyph metadata output */
                            slug_glyph *glyph) {
    int ci, bi;
    float bw = bbox_x1 - bbox_x0;
    float bh = bbox_y1 - bbox_y0;
    int band_start;

    /* Avoid division by zero for empty/degenerate glyphs */
    if (bw < 1e-6f) bw = 1e-6f;
    if (bh < 1e-6f) bh = 1e-6f;

    /* Ensure glyph's band headers + curve lists don't cross a row boundary.
       The shader fetches headers at glyph_loc.x + offset WITHOUT wrapping,
       so the entire glyph's band data must fit within one texture row. */
    {
        int total_band_texels = num_hbands + num_vbands; /* headers only for the row check */
        int x_pos = *band_texel_pos % SLUG_BAND_TEX_WIDTH;
        /* Estimate total data: headers + max possible curve list entries */
        int est_total = num_hbands + num_vbands + curve_count * (num_hbands + num_vbands);
        if (est_total > SLUG_BAND_TEX_WIDTH) est_total = SLUG_BAND_TEX_WIDTH; /* cap — should never happen */
        if (x_pos + est_total > SLUG_BAND_TEX_WIDTH) {
            /* Pad to next row */
            *band_texel_pos += (SLUG_BAND_TEX_WIDTH - x_pos);
        }
    }
    band_start = *band_texel_pos;

    glyph->bbox_x0 = bbox_x0; glyph->bbox_y0 = bbox_y0;
    glyph->bbox_x1 = bbox_x1; glyph->bbox_y1 = bbox_y1;
    glyph->band_scale_x = (float)num_vbands / bw;
    glyph->band_scale_y = (float)num_hbands / bh;
    glyph->band_offset_x = -bbox_x0 * glyph->band_scale_x;
    glyph->band_offset_y = -bbox_y0 * glyph->band_scale_y;
    glyph->band_max_x = (uint8_t)(num_vbands - 1);
    glyph->band_max_y = (uint8_t)(num_hbands - 1);
    glyph->band_loc_x = (uint16_t)(*band_texel_pos % SLUG_BAND_TEX_WIDTH);
    glyph->band_loc_y = (uint16_t)(*band_texel_pos / SLUG_BAND_TEX_WIDTH);
    glyph->curve_count = curve_count;

    /* Record per-curve texel positions (needed for band lists to reference them) */
    int *curve_positions = (int *)calloc((size_t)curve_count, sizeof(int));

    /* Write curve control points to curve texture.
       Each curve needs 2 adjacent texels on the same row (shader does curveLoc.x + 1). */
    for (ci = 0; ci < curve_count; ci++) {
        int pos = *curve_texel_pos;
        /* Ensure both texels fit on the same row */
        if ((pos % SLUG_BAND_TEX_WIDTH) + 1 >= SLUG_BAND_TEX_WIDTH) {
            pos = ((pos / SLUG_BAND_TEX_WIDTH) + 1) * SLUG_BAND_TEX_WIDTH;
            *curve_texel_pos = pos;
        }
        curve_positions[ci] = pos;
        /* texel 0: (p1.x, p1.y, p2.x, p2.y) */
        curve_out[pos * 4 + 0] = curves[ci].p1x;
        curve_out[pos * 4 + 1] = curves[ci].p1y;
        curve_out[pos * 4 + 2] = curves[ci].p2x;
        curve_out[pos * 4 + 3] = curves[ci].p2y;
        /* texel 1: (p3.x, p3.y, 0, 0) */
        curve_out[(pos + 1) * 4 + 0] = curves[ci].p3x;
        curve_out[(pos + 1) * 4 + 1] = curves[ci].p3y;
        curve_out[(pos + 1) * 4 + 2] = 0.0f;
        curve_out[(pos + 1) * 4 + 3] = 0.0f;
        *curve_texel_pos += 2;
    }

    /* Build per-band curve lists */
    {
        /* Temporary storage for band assignments */
        slug_sort_entry *h_entries = (slug_sort_entry *)calloc((size_t)(num_hbands * curve_count), sizeof(slug_sort_entry));
        int *h_counts = (int *)calloc((size_t)num_hbands, sizeof(int));
        slug_sort_entry *v_entries = (slug_sort_entry *)calloc((size_t)(num_vbands * curve_count), sizeof(slug_sort_entry));
        int *v_counts = (int *)calloc((size_t)num_vbands, sizeof(int));

        /* Assign curves to bands */
        for (ci = 0; ci < curve_count; ci++) {
            float cy_min = slug_min3f(curves[ci].p1y, curves[ci].p2y, curves[ci].p3y);
            float cy_max = slug_max3f(curves[ci].p1y, curves[ci].p2y, curves[ci].p3y);
            float cx_min = slug_min3f(curves[ci].p1x, curves[ci].p2x, curves[ci].p3x);
            float cx_max = slug_max3f(curves[ci].p1x, curves[ci].p2x, curves[ci].p3x);

            /* Horizontal bands (indexed by y, curves sorted by descending max-x) */
            {
                int b0 = slug_clampi((int)((cy_min - bbox_y0) / bh * num_hbands), 0, num_hbands - 1);
                int b1 = slug_clampi((int)((cy_max - bbox_y0) / bh * num_hbands), 0, num_hbands - 1);
                for (bi = b0; bi <= b1; bi++) {
                    int idx = bi * curve_count + h_counts[bi];
                    h_entries[idx].curve_idx = ci;
                    h_entries[idx].sort_key = cx_max;
                    h_counts[bi]++;
                }
            }

            /* Vertical bands (indexed by x, curves sorted by descending max-y) */
            {
                int b0 = slug_clampi((int)((cx_min - bbox_x0) / bw * num_vbands), 0, num_vbands - 1);
                int b1 = slug_clampi((int)((cx_max - bbox_x0) / bw * num_vbands), 0, num_vbands - 1);
                for (bi = b0; bi <= b1; bi++) {
                    int idx = bi * curve_count + v_counts[bi];
                    v_entries[idx].curve_idx = ci;
                    v_entries[idx].sort_key = cy_max;
                    v_counts[bi]++;
                }
            }
        }

        /* Sort each band's curves by descending sort key */
        for (bi = 0; bi < num_hbands; bi++) {
            if (h_counts[bi] > 1)
                qsort(&h_entries[bi * curve_count], (size_t)h_counts[bi],
                      sizeof(slug_sort_entry), slug_sort_cmp_desc);
        }
        for (bi = 0; bi < num_vbands; bi++) {
            if (v_counts[bi] > 1)
                qsort(&v_entries[bi * curve_count], (size_t)v_counts[bi],
                      sizeof(slug_sort_entry), slug_sort_cmp_desc);
        }

        /* Write band headers: horizontal bands first, then vertical bands.
           Each header is one uint4 texel: (curve_count, curve_list_offset, 0, 0).
           Curve list offset is relative to glyph's band_loc. */
        {
            int header_count = num_hbands + num_vbands;
            int list_offset = header_count; /* curve lists start after all headers */

            /* Horizontal band headers */
            for (bi = 0; bi < num_hbands; bi++) {
                int pos = *band_texel_pos;
                band_out[pos * 4 + 0] = (uint32_t)h_counts[bi];
                band_out[pos * 4 + 1] = (uint32_t)list_offset;
                band_out[pos * 4 + 2] = 0;
                band_out[pos * 4 + 3] = 0;
                (*band_texel_pos)++;

                /* Write curve location list for this band (deferred, after all headers) */
                list_offset += h_counts[bi];
            }

            /* Vertical band headers */
            for (bi = 0; bi < num_vbands; bi++) {
                int pos = *band_texel_pos;
                band_out[pos * 4 + 0] = (float)v_counts[bi];
                band_out[pos * 4 + 1] = (float)list_offset;
                band_out[pos * 4 + 2] = 0.0f;
                band_out[pos * 4 + 3] = 0.0f;
                (*band_texel_pos)++;
                list_offset += v_counts[bi];
            }

            /* Write curve location lists (using recorded curve_positions) */
            /* Horizontal bands */
            for (bi = 0; bi < num_hbands; bi++) {
                int j;
                for (j = 0; j < h_counts[bi]; j++) {
                    int ci2 = h_entries[bi * curve_count + j].curve_idx;
                    int abs_pos = curve_positions[ci2];
                    int pos = *band_texel_pos;
                    band_out[pos * 4 + 0] = (uint32_t)(abs_pos % SLUG_BAND_TEX_WIDTH);
                    band_out[pos * 4 + 1] = (uint32_t)(abs_pos / SLUG_BAND_TEX_WIDTH);
                    band_out[pos * 4 + 2] = 0;
                    band_out[pos * 4 + 3] = 0;
                    (*band_texel_pos)++;
                }
            }
            /* Vertical bands */
            for (bi = 0; bi < num_vbands; bi++) {
                int j;
                for (j = 0; j < v_counts[bi]; j++) {
                    int ci2 = v_entries[bi * curve_count + j].curve_idx;
                    int abs_pos = curve_positions[ci2];
                    int pos = *band_texel_pos;
                    band_out[pos * 4 + 0] = (uint32_t)(abs_pos % SLUG_BAND_TEX_WIDTH);
                    band_out[pos * 4 + 1] = (uint32_t)(abs_pos / SLUG_BAND_TEX_WIDTH);
                    band_out[pos * 4 + 2] = 0;
                    band_out[pos * 4 + 3] = 0;
                    (*band_texel_pos)++;
                }
            }
        }

        free(h_entries); free(h_counts);
        free(v_entries); free(v_counts);
    }

    free(curve_positions);
    return *band_texel_pos - band_start;
}

/* ── Build complete font data ───────────────────────────────────────────── */

static slug_font_data *slug_build_font(const unsigned char *ttf_data, int ttf_len,
                                        int num_hbands, int num_vbands) {
    stbtt_fontinfo font;
    slug_font_data *data;
    int ch, ascent_i, descent_i, line_gap_i;
    float em_scale;
    slug_curve curves[SLUG_MAX_CURVES_PER_GLYPH];
    /* Generous pre-allocation for texture data */
    int max_curve_texels = 128 * SLUG_MAX_CURVES_PER_GLYPH * 2;
    int max_band_texels = 128 * (num_hbands + num_vbands + SLUG_MAX_CURVES_PER_GLYPH * 2);
    int curve_texel_pos = 0, band_texel_pos = 0;

    (void)ttf_len;

    if (!stbtt_InitFont(&font, ttf_data, stbtt_GetFontOffsetForIndex(ttf_data, 0)))
        return NULL;

    data = (slug_font_data *)calloc(1, sizeof(slug_font_data));
    if (!data) return NULL;

    data->curve_texels = (float *)calloc((size_t)max_curve_texels * 4, sizeof(float));
    data->band_texels = (uint32_t *)calloc((size_t)max_band_texels * 4, sizeof(uint32_t));
    if (!data->curve_texels || !data->band_texels) {
        slug_font_data_free(data);
        return NULL;
    }

    /* Font metrics — normalize to em-space where ascent = 1.0 */
    stbtt_GetFontVMetrics(&font, &ascent_i, &descent_i, &line_gap_i);
    if (ascent_i == 0) ascent_i = 1;
    em_scale = 1.0f / (float)ascent_i;
    data->ascent = 1.0f;
    data->descent = (float)descent_i * em_scale;
    data->line_gap = (float)line_gap_i * em_scale;
    data->units_per_em = ascent_i;

    /* Process ASCII glyphs 32-126 */
    data->glyph_count = 0;
    for (ch = 32; ch < 127; ch++) {
        int glyph_index = stbtt_FindGlyphIndex(&font, ch);
        int advance_i, lsb_i;
        int curve_count;
        int x0, y0, x1, y1;

        stbtt_GetGlyphHMetrics(&font, glyph_index, &advance_i, &lsb_i);

        curve_count = slug_extract_glyph_curves(&font, glyph_index, em_scale, curves, SLUG_MAX_CURVES_PER_GLYPH);

        data->glyphs[ch].advance = (float)advance_i * em_scale;
        data->glyphs[ch].lsb = (float)lsb_i * em_scale;

        if (curve_count == 0) {
            /* Space or empty glyph — no curves, just advance */
            data->glyphs[ch].valid = 1;
            data->glyphs[ch].curve_count = 0;
            data->glyph_count++;
            continue;
        }

        /* Compute em-space bounding box from curves */
        {
            float bx0 = 1e10f, by0 = 1e10f, bx1 = -1e10f, by1 = -1e10f;
            int ci;
            for (ci = 0; ci < curve_count; ci++) {
                bx0 = slug_minf(bx0, slug_min3f(curves[ci].p1x, curves[ci].p2x, curves[ci].p3x));
                by0 = slug_minf(by0, slug_min3f(curves[ci].p1y, curves[ci].p2y, curves[ci].p3y));
                bx1 = slug_maxf(bx1, slug_max3f(curves[ci].p1x, curves[ci].p2x, curves[ci].p3x));
                by1 = slug_maxf(by1, slug_max3f(curves[ci].p1y, curves[ci].p2y, curves[ci].p3y));
            }
            /* Get integer bbox from stbtt for comparison */
            stbtt_GetGlyphBox(&font, glyph_index, &x0, &y0, &x1, &y1);

            slug_pack_glyph(curves, curve_count,
                           bx0, by0, bx1, by1,
                           num_hbands, num_vbands,
                           data->curve_texels, &curve_texel_pos,
                           data->band_texels, &band_texel_pos,
                           &data->glyphs[ch]);
        }

        data->glyphs[ch].valid = 1;
        data->glyph_count++;
    }

    /* Finalize texture dimensions */
    data->curve_texel_count = curve_texel_pos;
    data->curve_tex_height = (curve_texel_pos + SLUG_BAND_TEX_WIDTH - 1) / SLUG_BAND_TEX_WIDTH;
    if (data->curve_tex_height < 1) data->curve_tex_height = 1;

    data->band_texel_count = band_texel_pos;
    data->band_tex_height = (band_texel_pos + SLUG_BAND_TEX_WIDTH - 1) / SLUG_BAND_TEX_WIDTH;
    if (data->band_tex_height < 1) data->band_tex_height = 1;

    fprintf(stderr, "[slug] built font: %d glyphs, %d curves, curve_tex=%dx%d, band_tex=%dx%d\n",
            data->glyph_count, curve_texel_pos / 2,
            SLUG_BAND_TEX_WIDTH, data->curve_tex_height,
            SLUG_BAND_TEX_WIDTH, data->band_tex_height);

    return data;
}

static slug_font_data *slug_build_font_for_codepoints(const unsigned char *ttf_data, int ttf_len,
                                                       const uint32_t *codepoints, int cp_count,
                                                       int num_hbands, int num_vbands) {
    stbtt_fontinfo font;
    slug_font_data *data;
    int ci2, ascent_i, descent_i, line_gap_i;
    float em_scale;
    slug_curve curves[SLUG_MAX_CURVES_PER_GLYPH];
    int max_curve_texels = cp_count * SLUG_MAX_CURVES_PER_GLYPH * 2;
    int max_band_texels = cp_count * (num_hbands + num_vbands + SLUG_MAX_CURVES_PER_GLYPH * 2);
    int curve_texel_pos = 0, band_texel_pos = 0;

    (void)ttf_len;

    if (!stbtt_InitFont(&font, ttf_data, stbtt_GetFontOffsetForIndex(ttf_data, 0)))
        return NULL;

    data = (slug_font_data *)calloc(1, sizeof(slug_font_data));
    if (!data) return NULL;

    data->curve_texels = (float *)calloc((size_t)max_curve_texels * 4, sizeof(float));
    data->band_texels = (uint32_t *)calloc((size_t)max_band_texels * 4, sizeof(uint32_t));
    data->codepoints = (uint32_t *)calloc((size_t)cp_count, sizeof(uint32_t));
    data->cp_glyphs = (slug_glyph *)calloc((size_t)cp_count, sizeof(slug_glyph));
    if (!data->curve_texels || !data->band_texels || !data->codepoints || !data->cp_glyphs) {
        slug_font_data_free(data);
        return NULL;
    }

    stbtt_GetFontVMetrics(&font, &ascent_i, &descent_i, &line_gap_i);
    if (ascent_i == 0) ascent_i = 1;
    em_scale = 1.0f / (float)ascent_i;
    data->ascent = 1.0f;
    data->descent = (float)descent_i * em_scale;
    data->line_gap = (float)line_gap_i * em_scale;
    data->units_per_em = ascent_i;

    data->cp_glyph_count = 0;
    for (ci2 = 0; ci2 < cp_count; ci2++) {
        uint32_t cp = codepoints[ci2];
        int glyph_index = stbtt_FindGlyphIndex(&font, (int)cp);
        int advance_i, lsb_i, curve_count;
        slug_glyph *sg;

        stbtt_GetGlyphHMetrics(&font, glyph_index, &advance_i, &lsb_i);
        curve_count = slug_extract_glyph_curves(&font, glyph_index, em_scale, curves, SLUG_MAX_CURVES_PER_GLYPH);

        data->codepoints[data->cp_glyph_count] = cp;
        sg = &data->cp_glyphs[data->cp_glyph_count];
        sg->advance = (float)advance_i * em_scale;
        sg->lsb = (float)lsb_i * em_scale;

        if (curve_count == 0) {
            sg->valid = 1;
            sg->curve_count = 0;
            data->cp_glyph_count++;
            continue;
        }

        {
            float bx0 = 1e10f, by0 = 1e10f, bx1 = -1e10f, by1 = -1e10f;
            int k;
            for (k = 0; k < curve_count; k++) {
                bx0 = slug_minf(bx0, slug_min3f(curves[k].p1x, curves[k].p2x, curves[k].p3x));
                by0 = slug_minf(by0, slug_min3f(curves[k].p1y, curves[k].p2y, curves[k].p3y));
                bx1 = slug_maxf(bx1, slug_max3f(curves[k].p1x, curves[k].p2x, curves[k].p3x));
                by1 = slug_maxf(by1, slug_max3f(curves[k].p1y, curves[k].p2y, curves[k].p3y));
            }
            slug_pack_glyph(curves, curve_count, bx0, by0, bx1, by1,
                           num_hbands, num_vbands,
                           data->curve_texels, &curve_texel_pos,
                           data->band_texels, &band_texel_pos, sg);
        }
        sg->valid = 1;
        data->cp_glyph_count++;
    }

    data->curve_texel_count = curve_texel_pos;
    data->curve_tex_height = (curve_texel_pos + SLUG_BAND_TEX_WIDTH - 1) / SLUG_BAND_TEX_WIDTH;
    if (data->curve_tex_height < 1) data->curve_tex_height = 1;
    data->band_texel_count = band_texel_pos;
    data->band_tex_height = (band_texel_pos + SLUG_BAND_TEX_WIDTH - 1) / SLUG_BAND_TEX_WIDTH;
    if (data->band_tex_height < 1) data->band_tex_height = 1;

    fprintf(stderr, "[slug] built icon font: %d glyphs, %d curves\n",
            data->cp_glyph_count, curve_texel_pos / 2);
    return data;
}

static void slug_font_data_free(slug_font_data *data) {
    if (!data) return;
    free(data->curve_texels);
    free(data->band_texels);
    free(data->codepoints);
    free(data->cp_glyphs);
    free(data);
}

/* ── Cache save/load ────────────────────────────────────────────────────── */

#define SLUG_CACHE_MAGIC 0x47554C59 /* "SLUG" v7 — 16 bands, no weight boost */

static int slug_cache_save(const slug_font_data *data, const char *path, uint32_t ttf_fingerprint) {
    FILE *f = fopen(path, "wb");
    uint32_t magic = SLUG_CACHE_MAGIC;
    if (!f) return 0;

    fwrite(&magic, 4, 1, f);
    fwrite(&ttf_fingerprint, 4, 1, f);
    fwrite(&data->glyph_count, 4, 1, f);
    fwrite(&data->curve_texel_count, 4, 1, f);
    fwrite(&data->band_texel_count, 4, 1, f);
    fwrite(&data->curve_tex_height, 4, 1, f);
    fwrite(&data->band_tex_height, 4, 1, f);
    fwrite(&data->ascent, 4, 1, f);
    fwrite(&data->descent, 4, 1, f);
    fwrite(&data->line_gap, 4, 1, f);
    fwrite(&data->units_per_em, 4, 1, f);
    fwrite(data->glyphs, sizeof(slug_glyph), 128, f);
    fwrite(data->curve_texels, sizeof(float) * 4, (size_t)data->curve_texel_count, f);
    fwrite(data->band_texels, sizeof(uint32_t) * 4, (size_t)data->band_texel_count, f);

    fclose(f);
    return 1;
}

static int slug_cache_load(slug_font_data *data, const char *path, uint32_t ttf_fingerprint) {
    FILE *f = fopen(path, "rb");
    uint32_t magic, fp;
    if (!f) return 0;

    fread(&magic, 4, 1, f);
    fread(&fp, 4, 1, f);
    if (magic != SLUG_CACHE_MAGIC || fp != ttf_fingerprint) {
        fclose(f);
        return 0;
    }

    fread(&data->glyph_count, 4, 1, f);
    fread(&data->curve_texel_count, 4, 1, f);
    fread(&data->band_texel_count, 4, 1, f);
    fread(&data->curve_tex_height, 4, 1, f);
    fread(&data->band_tex_height, 4, 1, f);
    fread(&data->ascent, 4, 1, f);
    fread(&data->descent, 4, 1, f);
    fread(&data->line_gap, 4, 1, f);
    fread(&data->units_per_em, 4, 1, f);
    fread(data->glyphs, sizeof(slug_glyph), 128, f);

    data->curve_texels = (float *)calloc((size_t)data->curve_texel_count * 4, sizeof(float));
    data->band_texels = (uint32_t *)calloc((size_t)data->band_texel_count * 4, sizeof(uint32_t));
    if (!data->curve_texels || !data->band_texels) {
        fclose(f);
        return 0;
    }

    fread(data->curve_texels, sizeof(float) * 4, (size_t)data->curve_texel_count, f);
    fread(data->band_texels, sizeof(uint32_t) * 4, (size_t)data->band_texel_count, f);

    fclose(f);
    return 1;
}

#endif /* SLUG_FONT_IMPL */
#endif /* SLUG_FONT_H */
