/*
 * msdf_gen.h — Pure C MSDF (Multi-channel Signed Distance Field) generator
 *
 * Single-header, included only by externals.c. TCC-compatible (no C++).
 * Generates an RGBA8 atlas texture from stb_truetype glyph outlines.
 *
 * Algorithm adapted from Godot's msdfgen usage and Chlumsky's msdfgen.
 */

#ifndef MSDF_GEN_H
#define MSDF_GEN_H

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <float.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
/* stb_truetype.h must be included before this header */

/* ── Configuration ──────────────────────────────────────────────────────── */

#define MSDF_SOURCE_SIZE   48      /* font size for MSDF generation (px) */
#define MSDF_PIXEL_RANGE   14.0f  /* distance field range in pixels (matches Godot) */
#define MSDF_RECT_MARGIN   2      /* padding around each glyph in atlas */
#define MSDF_ATLAS_WIDTH   1024
#define MSDF_ATLAS_HEIGHT  1024

/* ── Data structures ────────────────────────────────────────────────────── */

typedef struct msdf_glyph {
    int atlas_x, atlas_y;   /* position in atlas */
    int width, height;      /* tile dimensions (pixels) */
    float xoff, yoff;       /* glyph offset in font-size-normalized units (includes margin) */
    float advance;          /* horizontal advance in font-size-normalized units */
    float u0, v0, u1, v1;  /* normalized UV in atlas [0..1] */
} msdf_glyph;

typedef struct msdf_atlas {
    unsigned char *pixels;  /* RGBA8, MSDF_ATLAS_WIDTH * MSDF_ATLAS_HEIGHT * 4 */
    int width, height;
    /* Shelf packer state */
    int shelf_x;            /* current x cursor on active shelf */
    int shelf_y;            /* current shelf top y */
    int shelf_h;            /* current shelf height */
} msdf_atlas;

/* ── Edge types for MSDF coloring ───────────────────────────────────────── */

#define MSDF_RED   1
#define MSDF_GREEN 2
#define MSDF_BLUE  4

typedef enum { MSDF_EDGE_LINE, MSDF_EDGE_QUADRATIC } msdf_edge_type;

typedef struct msdf_edge {
    msdf_edge_type type;
    int color;              /* bitmask: MSDF_RED, MSDF_GREEN, MSDF_BLUE */
    float p0x, p0y;         /* start */
    float p1x, p1y;         /* control (quadratic) or end (line) */
    float p2x, p2y;         /* end (quadratic only) */
} msdf_edge;

typedef struct msdf_contour {
    msdf_edge *edges;
    int edge_count;
    int edge_cap;
} msdf_contour;

typedef struct msdf_shape {
    msdf_contour *contours;
    int contour_count;
    int contour_cap;
} msdf_shape;

/* ── Math helpers ───────────────────────────────────────────────────────── */

static float msdf_clampf(float x, float lo, float hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

static float msdf_cross2(float ax, float ay, float bx, float by) {
    return ax * by - ay * bx;
}

static float msdf_dot2(float ax, float ay, float bx, float by) {
    return ax * bx + ay * by;
}

/* ── Equation solver (ported verbatim from msdfgen equation-solver.cpp) ── */

/* Solve ax^2 + bx + c = 0. Returns 0-2 real roots, or -1 for identity. */
static int msdf_solve_quadratic(double x[2], double a, double b, double c) {
    double dscr, sq;
    if (a == 0 || fabs(b) > 1e12 * fabs(a)) {
        if (b == 0) {
            if (c == 0) return -1;
            return 0;
        }
        x[0] = -c / b;
        return 1;
    }
    dscr = b * b - 4 * a * c;
    if (dscr > 0) {
        sq = sqrt(dscr);
        x[0] = (-b + sq) / (2 * a);
        x[1] = (-b - sq) / (2 * a);
        return 2;
    } else if (dscr == 0) {
        x[0] = -b / (2 * a);
        return 1;
    }
    return 0;
}

static int msdf_solve_cubic_normed(double x[3], double a, double b, double c) {
    double a2 = a * a;
    double q = (a2 - 3 * b) / 9.0;
    double r = (a * (2 * a2 - 9 * b) + 27 * c) / 54.0;
    double r2 = r * r;
    double q3 = q * q * q;
    double A = a / 3.0;
    if (r2 < q3) {
        double t = r / sqrt(q3);
        if (t < -1) t = -1;
        if (t > 1) t = 1;
        t = acos(t);
        q = -2 * sqrt(q);
        x[0] = q * cos(t / 3.0) - A;
        x[1] = q * cos((t + 2 * M_PI) / 3.0) - A;
        x[2] = q * cos((t - 2 * M_PI) / 3.0) - A;
        return 3;
    } else {
        double u = (r < 0 ? 1 : -1) * pow(fabs(r) + sqrt(r2 - q3), 1.0 / 3.0);
        double v = u == 0 ? 0 : q / u;
        x[0] = (u + v) - A;
        if (u == v || fabs(u - v) < 1e-12 * fabs(u + v)) {
            x[1] = -0.5 * (u + v) - A;
            return 2;
        }
        return 1;
    }
}

/* Solve ax^3 + bx^2 + cx + d = 0. Returns number of real roots. */
static int msdf_solve_cubic(double x[3], double a, double b, double c, double d) {
    if (a != 0) {
        double bn = b / a;
        if (fabs(bn) < 1e6)
            return msdf_solve_cubic_normed(x, bn, c / a, d / a);
    }
    return msdf_solve_quadratic(x, b, c, d);
}

/* ── SignedDistance with dot tiebreaker (from msdfgen SignedDistance.hpp) ── */

typedef struct msdf_signed_distance {
    double distance;
    double dot;
} msdf_signed_distance;

/* Returns 1 if a is closer than b: smaller |distance|, or same |distance| with smaller dot */
static int msdf_sd_lt(msdf_signed_distance a, msdf_signed_distance b) {
    return fabs(a.distance) < fabs(b.distance) ||
           (fabs(a.distance) == fabs(b.distance) && a.dot < b.dot);
}

/* Returns 1 for positive, -1 for zero or negative (from msdfgen arithmetics.hpp) */
static int msdf_nonzero_sign(double n) {
    return 2 * (n > 0) - 1;
}

/* ── Linear signed distance (from msdfgen LinearSegment::signedDistance) ── */

static msdf_signed_distance msdf_line_sd(
    double ox, double oy,
    double p0x, double p0y, double p1x, double p1y)
{
    msdf_signed_distance result;
    double aqx = ox - p0x, aqy = oy - p0y;
    double abx = p1x - p0x, aby = p1y - p0y;
    double param = (aqx * abx + aqy * aby) / (abx * abx + aby * aby);
    double eqx, eqy, ep_dist, ab_len, ortho;

    if (param > 0.5) { eqx = p1x - ox; eqy = p1y - oy; }
    else             { eqx = p0x - ox; eqy = p0y - oy; }
    ep_dist = sqrt(eqx * eqx + eqy * eqy);

    if (param > 0 && param < 1) {
        ab_len = sqrt(abx * abx + aby * aby);
        ortho = (aqx * aby - aqy * abx) / ab_len;
        if (fabs(ortho) < ep_dist) {
            result.distance = ortho;
            result.dot = 0;
            return result;
        }
    }

    {
        double eq_len, ab_n, eq_nx, eq_ny;
        ab_len = sqrt(abx * abx + aby * aby);
        ab_n = 1.0 / ab_len;
        eq_len = sqrt(eqx * eqx + eqy * eqy);
        eq_nx = eq_len > 0 ? eqx / eq_len : 0;
        eq_ny = eq_len > 0 ? eqy / eq_len : 1;
        result.distance = msdf_nonzero_sign(aqx * aby - aqy * abx) * ep_dist;
        result.dot = fabs((abx * ab_n) * eq_nx + (aby * ab_n) * eq_ny);
    }
    return result;
}

/* ── Quadratic signed distance (from msdfgen QuadraticSegment::signedDistance) ── */

static msdf_signed_distance msdf_quad_sd(
    double ox, double oy,
    double p0x, double p0y, double p1x, double p1y, double p2x, double p2y)
{
    msdf_signed_distance result;
    double qax = p0x - ox, qay = p0y - oy;
    double abx = p1x - p0x, aby = p1y - p0y;
    double brx = p2x - p1x - abx, bry = p2y - p1y - aby;
    double a = brx * brx + bry * bry;
    double b = 3 * (abx * brx + aby * bry);
    double c = 2 * (abx * abx + aby * aby) + (qax * brx + qay * bry);
    double d = qax * abx + qay * aby;
    double t[3];
    int solutions, i;
    double min_dist, param, qa_len;

    /* direction(0) with degenerate fallback */
    double d0x = p1x - p0x, d0y = p1y - p0y;
    if (d0x == 0 && d0y == 0) { d0x = p2x - p0x; d0y = p2y - p0y; }
    /* direction(1) with degenerate fallback */
    double d1x = p2x - p1x, d1y = p2y - p1y;
    if (d1x == 0 && d1y == 0) { d1x = p2x - p0x; d1y = p2y - p0y; }

    solutions = msdf_solve_cubic(t, a, b, c, d);
    qa_len = sqrt(qax * qax + qay * qay);

    /* Initialize from endpoint A */
    min_dist = msdf_nonzero_sign(d0x * qay - d0y * qax) * qa_len;
    param = -(qax * d0x + qay * d0y) / (d0x * d0x + d0y * d0y);

    /* Check endpoint B */
    {
        double bqx = p2x - ox, bqy = p2y - oy;
        double bq_len = sqrt(bqx * bqx + bqy * bqy);
        if (bq_len < fabs(min_dist)) {
            min_dist = msdf_nonzero_sign(d1x * bqy - d1y * bqx) * bq_len;
            param = ((ox - p1x) * d1x + (oy - p1y) * d1y) / (d1x * d1x + d1y * d1y);
        }
    }

    /* Check cubic solutions — inner points only, with <= for tiebreaking */
    for (i = 0; i < solutions; i++) {
        if (t[i] > 0 && t[i] < 1) {
            double qex = qax + 2 * t[i] * abx + t[i] * t[i] * brx;
            double qey = qay + 2 * t[i] * aby + t[i] * t[i] * bry;
            double dist = sqrt(qex * qex + qey * qey);
            if (dist <= fabs(min_dist)) {
                double tdx = abx + t[i] * brx, tdy = aby + t[i] * bry;
                min_dist = msdf_nonzero_sign(tdx * qey - tdy * qex) * dist;
                param = t[i];
            }
        }
    }

    /* Return with dot tiebreaker */
    result.distance = min_dist;
    if (param >= 0 && param <= 1) {
        result.dot = 0;
        return result;
    }
    if (param < 0.5) {
        double d0_len = sqrt(d0x * d0x + d0y * d0y);
        double qa_n = qa_len > 0 ? 1.0 / qa_len : 0;
        result.dot = fabs((d0x / d0_len) * (qax * qa_n) + (d0y / d0_len) * (qay * qa_n));
    } else {
        double d1_len = sqrt(d1x * d1x + d1y * d1y);
        double bqx = p2x - ox, bqy = p2y - oy;
        double bq_len = sqrt(bqx * bqx + bqy * bqy);
        double bq_n = bq_len > 0 ? 1.0 / bq_len : 0;
        result.dot = fabs((d1x / d1_len) * (bqx * bq_n) + (d1y / d1_len) * (bqy * bq_n));
    }
    return result;
}

/* ── Signed distance from point to edge ────────────────────────────────── */

static msdf_signed_distance msdf_edge_sd(const msdf_edge *e, double ox, double oy) {
    if (e->type == MSDF_EDGE_LINE)
        return msdf_line_sd(ox, oy, e->p0x, e->p0y, e->p1x, e->p1y);
    else
        return msdf_quad_sd(ox, oy, e->p0x, e->p0y, e->p1x, e->p1y, e->p2x, e->p2y);
}

/* ── Winding number (ray-casting) for inside/outside determination ──────── */

/*
 * Count winding number at point (px, py) by casting a ray in the +x direction.
 * Uses non-zero winding rule (same as TrueType).
 * Operates in font coordinate space (y-up).
 */

/* Line segment contribution to winding number */
static int msdf_winding_line(float px, float py,
                              float ax, float ay, float bx, float by)
{
    if ((ay <= py && by > py) || (by <= py && ay > py)) {
        float t = (py - ay) / (by - ay);
        float cross_x = ax + t * (bx - ax);
        if (cross_x > px)
            return (by > ay) ? 1 : -1;
    }
    return 0;
}

/* Quadratic bezier contribution to winding number.
   B(t) = (1-t)^2*A + 2(1-t)t*B + t^2*C
   B(t).y = (ay - 2*by + cy)*t^2 + (-2*ay + 2*by)*t + ay */
static int msdf_winding_quadratic(float px, float py,
                                   float ax, float ay,
                                   float bx, float by,
                                   float cx, float cy)
{
    /* Coefficients for y(t) = qa*t^2 + qb*t + qc */
    float qa = ay - 2*by + cy;
    float qb = -2*ay + 2*by;
    float qc = ay - py;
    int winding = 0;

    /* Find t values where y(t) = py, i.e. qa*t^2 + qb*t + qc = 0 */
    float t_vals[2];
    int t_count = 0;

    if (fabsf(qa) < 1e-9f) {
        /* Linear: qb*t + qc = 0 */
        if (fabsf(qb) > 1e-9f) {
            float t = -qc / qb;
            if (t > 0.0f && t <= 1.0f)
                t_vals[t_count++] = t;
        }
    } else {
        float disc = qb * qb - 4.0f * qa * qc;
        if (disc >= 0) {
            float sq = sqrtf(disc);
            float t0 = (-qb - sq) / (2.0f * qa);
            float t1 = (-qb + sq) / (2.0f * qa);
            if (t0 > 0.0f && t0 <= 1.0f) t_vals[t_count++] = t0;
            if (t1 > 0.0f && t1 <= 1.0f) t_vals[t_count++] = t1;
        }
    }

    {
        int i;
        for (i = 0; i < t_count; i++) {
            float t = t_vals[i];
            float omt = 1.0f - t;
            /* x position on curve at this t */
            float cross_x = omt*omt*ax + 2*omt*t*bx + t*t*cx;
            if (cross_x > px) {
                /* y-direction at crossing: derivative dy/dt = 2*qa*t + qb */
                float dy_dt = 2.0f * qa * t + qb;
                winding += (dy_dt > 0) ? 1 : -1;
            }
        }
    }

    return winding;
}

/* Compute winding number for a point against an entire shape */
static int msdf_shape_winding(const msdf_shape *shape, float px, float py) {
    int winding = 0;
    int ci, ei;
    for (ci = 0; ci < shape->contour_count; ci++) {
        const msdf_contour *c = &shape->contours[ci];
        for (ei = 0; ei < c->edge_count; ei++) {
            const msdf_edge *e = &c->edges[ei];
            if (e->type == MSDF_EDGE_LINE)
                winding += msdf_winding_line(px, py, e->p0x, e->p0y, e->p1x, e->p1y);
            else
                winding += msdf_winding_quadratic(px, py, e->p0x, e->p0y, e->p1x, e->p1y, e->p2x, e->p2y);
        }
    }
    return winding;
}

/* ── Outline extraction from stb_truetype ───────────────────────────────── */

static void msdf_shape_init(msdf_shape *s) {
    memset(s, 0, sizeof(*s));
}

static void msdf_shape_free(msdf_shape *s) {
    int i;
    for (i = 0; i < s->contour_count; i++)
        free(s->contours[i].edges);
    free(s->contours);
    memset(s, 0, sizeof(*s));
}

static msdf_contour *msdf_shape_add_contour(msdf_shape *s) {
    if (s->contour_count >= s->contour_cap) {
        s->contour_cap = s->contour_cap ? s->contour_cap * 2 : 4;
        s->contours = (msdf_contour *)realloc(s->contours, (size_t)s->contour_cap * sizeof(msdf_contour));
    }
    msdf_contour *c = &s->contours[s->contour_count++];
    memset(c, 0, sizeof(*c));
    return c;
}

static void msdf_contour_add_edge(msdf_contour *c, msdf_edge e) {
    if (c->edge_count >= c->edge_cap) {
        c->edge_cap = c->edge_cap ? c->edge_cap * 2 : 8;
        c->edges = (msdf_edge *)realloc(c->edges, (size_t)c->edge_cap * sizeof(msdf_edge));
    }
    c->edges[c->edge_count++] = e;
}

static void msdf_extract_shape(msdf_shape *shape, stbtt_fontinfo *info,
                                int glyph_index, float scale)
{
    stbtt_vertex *verts = NULL;
    int nv = stbtt_GetGlyphShape(info, glyph_index, &verts);
    msdf_contour *cur = NULL;
    float cx = 0, cy = 0;
    int i;

    msdf_shape_init(shape);

    for (i = 0; i < nv; i++) {
        float vx = verts[i].x * scale;
        float vy = verts[i].y * scale;

        switch (verts[i].type) {
        case STBTT_vmove:
            cur = msdf_shape_add_contour(shape);
            cx = vx; cy = vy;
            break;
        case STBTT_vline: {
            if (!cur) break;
            msdf_edge e;
            e.type = MSDF_EDGE_LINE;
            e.color = 0;
            e.p0x = cx; e.p0y = cy;
            e.p1x = vx; e.p1y = vy;
            e.p2x = 0;  e.p2y = 0;
            msdf_contour_add_edge(cur, e);
            cx = vx; cy = vy;
            break;
        }
        case STBTT_vcurve: {
            if (!cur) break;
            msdf_edge e;
            e.type = MSDF_EDGE_QUADRATIC;
            e.color = 0;
            e.p0x = cx; e.p0y = cy;
            e.p1x = verts[i].cx * scale;
            e.p1y = verts[i].cy * scale;
            e.p2x = vx; e.p2y = vy;
            msdf_contour_add_edge(cur, e);
            cx = vx; cy = vy;
            break;
        }
        case STBTT_vcubic: {
            if (!cur) break;
            float c1x = verts[i].cx * scale, c1y = verts[i].cy * scale;
            float c2x = verts[i].cx1 * scale, c2y = verts[i].cy1 * scale;
            float splitx = (c1x + c2x) * 0.5f;
            float splity = (c1y + c2y) * 0.5f;

            msdf_edge e1;
            e1.type = MSDF_EDGE_QUADRATIC;
            e1.color = 0;
            e1.p0x = cx; e1.p0y = cy;
            e1.p1x = (cx + 3*c1x) * 0.25f;
            e1.p1y = (cy + 3*c1y) * 0.25f;
            e1.p2x = splitx; e1.p2y = splity;
            msdf_contour_add_edge(cur, e1);

            msdf_edge e2;
            e2.type = MSDF_EDGE_QUADRATIC;
            e2.color = 0;
            e2.p0x = splitx; e2.p0y = splity;
            e2.p1x = (3*c2x + vx) * 0.25f;
            e2.p1y = (3*c2y + vy) * 0.25f;
            e2.p2x = vx; e2.p2y = vy;
            msdf_contour_add_edge(cur, e2);

            cx = vx; cy = vy;
            break;
        }
        }
    }
    stbtt_FreeShape(info, verts);
}

/* ── Edge coloring ──────────────────────────────────────────────────────── */

static void msdf_edge_direction(const msdf_edge *e, int at_end, float *dx, float *dy) {
    if (e->type == MSDF_EDGE_LINE) {
        *dx = e->p1x - e->p0x;
        *dy = e->p1y - e->p0y;
    } else {
        if (!at_end) {
            *dx = e->p1x - e->p0x;
            *dy = e->p1y - e->p0y;
        } else {
            *dx = e->p2x - e->p1x;
            *dy = e->p2y - e->p1y;
        }
    }
    float len = sqrtf((*dx)*(*dx) + (*dy)*(*dy));
    if (len > 1e-9f) { *dx /= len; *dy /= len; }
}

static void msdf_color_edges(msdf_shape *shape) {
    int ci, ei;
    static const int colors[] = {
        MSDF_RED | MSDF_GREEN,
        MSDF_RED | MSDF_BLUE,
        MSDF_GREEN | MSDF_BLUE,
    };

    for (ci = 0; ci < shape->contour_count; ci++) {
        msdf_contour *c = &shape->contours[ci];
        if (c->edge_count == 0) continue;

        if (c->edge_count == 1) {
            c->edges[0].color = MSDF_RED | MSDF_GREEN | MSDF_BLUE;
            continue;
        }

        if (c->edge_count == 2) {
            c->edges[0].color = MSDF_RED | MSDF_GREEN;
            c->edges[1].color = MSDF_GREEN | MSDF_BLUE;
            continue;
        }

        int color_idx = 0;
        c->edges[0].color = colors[color_idx];

        for (ei = 1; ei < c->edge_count; ei++) {
            float d0x, d0y, d1x, d1y;
            msdf_edge_direction(&c->edges[ei - 1], 1, &d0x, &d0y);
            msdf_edge_direction(&c->edges[ei], 0, &d1x, &d1y);

            float cross = msdf_cross2(d0x, d0y, d1x, d1y);
            float dot = msdf_dot2(d0x, d0y, d1x, d1y);

            if (fabsf(cross) > 0.05f || dot < 0.95f)
                color_idx = (color_idx + 1) % 3;
            c->edges[ei].color = colors[color_idx];
        }

        if (c->edge_count >= 3 && c->edges[0].color == c->edges[c->edge_count - 1].color) {
            float d0x, d0y, d1x, d1y;
            msdf_edge_direction(&c->edges[c->edge_count - 1], 1, &d0x, &d0y);
            msdf_edge_direction(&c->edges[0], 0, &d1x, &d1y);
            float cross = msdf_cross2(d0x, d0y, d1x, d1y);
            float dot = msdf_dot2(d0x, d0y, d1x, d1y);
            if (fabsf(cross) > 0.05f || dot < 0.95f) {
                int idx;
                for (idx = 0; idx < 3; idx++) {
                    if (colors[idx] != c->edges[c->edge_count - 1].color) {
                        c->edges[0].color = colors[idx];
                        break;
                    }
                }
            }
        }
    }
}

/* ── Atlas packing (shelf algorithm) ────────────────────────────────────── */

static void msdf_atlas_init(msdf_atlas *atlas) {
    atlas->width = MSDF_ATLAS_WIDTH;
    atlas->height = MSDF_ATLAS_HEIGHT;
    atlas->pixels = (unsigned char *)calloc((size_t)(atlas->width * atlas->height * 4), 1);
    atlas->shelf_x = 0;
    atlas->shelf_y = 0;
    atlas->shelf_h = 0;
}

static void msdf_atlas_free(msdf_atlas *atlas) {
    free(atlas->pixels);
    memset(atlas, 0, sizeof(*atlas));
}

static int msdf_atlas_pack(msdf_atlas *atlas, const unsigned char *tile,
                            int tw, int th, int *out_x, int *out_y)
{
    if (atlas->shelf_x + tw > atlas->width) {
        atlas->shelf_y += atlas->shelf_h;
        atlas->shelf_x = 0;
        atlas->shelf_h = 0;
    }

    if (atlas->shelf_y + th > atlas->height) {
        fprintf(stderr, "MSDF atlas full!\n");
        return -1;
    }

    *out_x = atlas->shelf_x;
    *out_y = atlas->shelf_y;

    for (int y = 0; y < th; y++) {
        memcpy(atlas->pixels + ((atlas->shelf_y + y) * atlas->width + atlas->shelf_x) * 4,
               tile + y * tw * 4,
               (size_t)(tw * 4));
    }

    atlas->shelf_x += tw;
    if (th > atlas->shelf_h) atlas->shelf_h = th;

    return 0;
}

/* ── MSDF error correction (full msdfgen algorithm) ────────────────────── */

/*
 * Port of msdfgen's MSDFErrorCorrection (EDGE_PRIORITY + DO_NOT_CHECK_DISTANCE).
 * The GPU bilinearly interpolates R, G, B independently, but median(lerp(R), lerp(G),
 * lerp(B)) != lerp(median(R, G, B)). This creates artifacts at channel crossing points.
 *
 * The algorithm:
 *  Phase 1: PROTECT corners (color-change junctions) and edge-contributing texels
 *  Phase 2: DETECT artifacts by simulating linear/bilinear interpolation between
 *           neighbor pairs — find channel crossings where the median deviates or inverts
 *  Phase 3: APPLY — set R=G=B=median at all flagged artifact texels
 */

#define MSDF_EC_ERROR     1
#define MSDF_EC_PROTECTED 2
#define MSDF_EC_T_EPSILON 0.01f
#define MSDF_EC_MIN_DEVIATION_RATIO (10.0f / 9.0f)
#define MSDF_EC_PROT_RADIUS_TOLERANCE 1.001f

static float msdf_medianf(float r, float g, float b) {
    return fmaxf(fminf(r, g), fminf(fmaxf(r, g), b));
}

static float msdf_mixf(float a, float b, float t) {
    return (1.0f - t) * a + t * b;
}

/* Solve at^2 + bt + c = 0. Returns 0–2 real roots in roots[]. */
static int msdf_ec_solve_quadratic(float a, float b, float c, float *roots) {
    float disc, sq;
    if (fabsf(a) < 1e-12f) {
        if (fabsf(b) < 1e-12f) return 0;
        roots[0] = -c / b;
        return 1;
    }
    disc = b * b - 4.0f * a * c;
    if (disc < 0) return 0;
    sq = sqrtf(fabsf(disc));
    roots[0] = (-b + sq) / (2.0f * a);
    roots[1] = (-b - sq) / (2.0f * a);
    return disc > 0 ? 2 : 1;
}

/*
 * BaseArtifactClassifier range test.
 * Checks whether the interpolated median xm at position xt (between endpoints at/bt)
 * constitutes an artifact relative to endpoint medians am/bm.
 *   - Inversions (both endpoints same side of 0.5, crossing opposite) always checked.
 *   - Deviations (median outside [min,max] of endpoints) only checked for unprotected.
 *   - Span tolerance: natural distance change per pixel prevents false positives.
 */
static int msdf_ec_range_test(float at, float bt, float xt,
                               float am, float bm, float xm,
                               float span, int is_protected) {
    int is_inversion, is_deviation;
    float axSpan, bxSpan;

    is_inversion = ((am > 0.5f && bm > 0.5f && xm <= 0.5f) ||
                    (am < 0.5f && bm < 0.5f && xm >= 0.5f));
    is_deviation = (!is_protected) &&
                   (xm < fminf(am, bm) || xm > fmaxf(am, bm));

    if (!is_inversion && !is_deviation)
        return 0;

    /* Span tolerance: expected maximum change from endpoints */
    axSpan = (xt - at) * span;
    bxSpan = (bt - xt) * span;

    if (xm >= am - axSpan && xm <= am + axSpan &&
        xm >= bm - bxSpan && xm <= bm + bxSpan)
        return 0;  /* Within tolerance — not a real artifact */

    return 1;
}

/*
 * Check for linear interpolation artifact between two adjacent texels a[3] and b[3].
 * Finds t values where two channels cross, computes interpolated median at crossing.
 */
static int msdf_ec_has_linear_artifact(const float *a, const float *b,
                                        float span, int is_protected) {
    float am, bm, dA, dB, denom, t, xm;
    int p, c1, c2;

    am = msdf_medianf(a[0], a[1], a[2]);
    bm = msdf_medianf(b[0], b[1], b[2]);

    for (p = 0; p < 3; p++) {
        c1 = p;
        c2 = (p + 1) % 3;
        dA = a[c1] - a[c2];
        dB = b[c1] - b[c2];
        denom = dA - dB;
        if (fabsf(denom) < 1e-12f) continue;

        t = dA / denom;
        if (t <= MSDF_EC_T_EPSILON || t >= 1.0f - MSDF_EC_T_EPSILON)
            continue;

        xm = msdf_medianf(
            msdf_mixf(a[0], b[0], t),
            msdf_mixf(a[1], b[1], t),
            msdf_mixf(a[2], b[2], t)
        );

        if (msdf_ec_range_test(0.0f, 1.0f, t, am, bm, xm, span, is_protected))
            return 1;
    }
    return 0;
}

/*
 * Check for diagonal (bilinear) interpolation artifact.
 * a and d are diagonal corners; b and c are the shared neighbors.
 * Along the diagonal, interpolation is quadratic: pixel(t) = q*t^2 + l*t + a
 */
static int msdf_ec_has_diagonal_artifact(const float *a, const float *b,
                                          const float *c, const float *d,
                                          float span, int is_protected) {
    float am, dm, qq[3], l[3], tEx[3];
    float roots[2], abc;
    int ch, p, c1, c2, nroots, ri;

    am = msdf_medianf(a[0], a[1], a[2]);
    dm = msdf_medianf(d[0], d[1], d[2]);

    for (ch = 0; ch < 3; ch++) {
        abc = a[ch] - b[ch] - c[ch];
        l[ch] = -a[ch] - abc;        /* = b + c - 2a */
        qq[ch] = d[ch] + abc;         /* = d + a - b - c */
        tEx[ch] = (fabsf(qq[ch]) > 1e-12f) ? (-0.5f * l[ch] / qq[ch]) : -1.0f;
    }

    /* For each pair of channels, find t where they cross along the diagonal */
    for (p = 0; p < 3; p++) {
        c1 = p;
        c2 = (p + 1) % 3;
        nroots = msdf_ec_solve_quadratic(
            qq[c1] - qq[c2], l[c1] - l[c2], a[c1] - a[c2], roots);

        for (ri = 0; ri < nroots; ri++) {
            float t = roots[ri];
            float xm, em;
            if (t <= MSDF_EC_T_EPSILON || t >= 1.0f - MSDF_EC_T_EPSILON)
                continue;

            xm = msdf_medianf(
                t * (t * qq[0] + l[0]) + a[0],
                t * (t * qq[1] + l[1]) + a[1],
                t * (t * qq[2] + l[2]) + a[2]
            );

            if (msdf_ec_range_test(0.0f, 1.0f, t, am, dm, xm, span, is_protected))
                return 1;

            /* Also test against local extrema medians — catches overshoots */
            for (ch = 0; ch < 3; ch++) {
                float te = tEx[ch];
                if (te <= MSDF_EC_T_EPSILON || te >= 1.0f - MSDF_EC_T_EPSILON)
                    continue;
                em = msdf_medianf(
                    te * (te * qq[0] + l[0]) + a[0],
                    te * (te * qq[1] + l[1]) + a[1],
                    te * (te * qq[2] + l[2]) + a[2]
                );
                if (msdf_ec_range_test(0.0f, 1.0f, t, am, em, xm, span, is_protected))
                    return 1;
            }
        }
    }
    return 0;
}

/*
 * Determine if a color channel contributes to a real shape edge between two texels.
 * A channel crosses 0.5 between texels a and b, and at that crossing it is the median.
 */
static int msdf_ec_edge_between_texels_ch(const float *a, const float *b, int ch) {
    float denom, t, vals[3], med;
    if ((a[ch] - 0.5f) * (b[ch] - 0.5f) >= 0.0f) return 0;
    denom = b[ch] - a[ch];
    if (fabsf(denom) < 1e-12f) return 0;
    t = (0.5f - a[ch]) / denom;
    if (t <= 0.0f || t >= 1.0f) return 0;

    vals[0] = msdf_mixf(a[0], b[0], t);
    vals[1] = msdf_mixf(a[1], b[1], t);
    vals[2] = msdf_mixf(a[2], b[2], t);
    med = msdf_medianf(vals[0], vals[1], vals[2]);
    return (fabsf(vals[ch] - med) < 1e-6f) ? 1 : 0;
}

/* Get bitmask of channels that contribute to a real edge between two texels */
static int msdf_ec_edge_between_texels(const float *a, const float *b) {
    int mask = 0;
    if (msdf_ec_edge_between_texels_ch(a, b, 0)) mask |= MSDF_RED;
    if (msdf_ec_edge_between_texels_ch(a, b, 1)) mask |= MSDF_GREEN;
    if (msdf_ec_edge_between_texels_ch(a, b, 2)) mask |= MSDF_BLUE;
    return mask;
}

/*
 * Mark texel as PROTECTED if any of its non-median (extreme) channels
 * contribute to a real edge. Flattening such texels would lose edge information.
 */
static void msdf_ec_protect_extreme(unsigned char *stencil, int idx,
                                     const float *texel, int edge_mask) {
    float med;
    int ch_bits[3], median_ch, ch;
    if (!edge_mask) return;

    med = msdf_medianf(texel[0], texel[1], texel[2]);
    ch_bits[0] = MSDF_RED; ch_bits[1] = MSDF_GREEN; ch_bits[2] = MSDF_BLUE;

    /* Find which channel is the median */
    if (fabsf(texel[0] - med) < 1e-6f) median_ch = 0;
    else if (fabsf(texel[1] - med) < 1e-6f) median_ch = 1;
    else median_ch = 2;

    /* If any extreme channel (non-median) is in the edge mask, protect */
    for (ch = 0; ch < 3; ch++) {
        if (ch == median_ch) continue;
        if (edge_mask & ch_bits[ch]) {
            stencil[idx] |= MSDF_EC_PROTECTED;
            return;
        }
    }
}

/*
 * Full MSDF error correction.
 * shape + ix0/iy0 provide coordinate mapping for corner protection.
 */
static void msdf_error_correct_tile(unsigned char *tile, int tw, int th,
                                     const msdf_shape *shape,
                                     int ix0, int iy0) {
    int x, y, size = tw * th;
    float span, prot_radius;
    float *ftile;
    unsigned char *stencil;

    ftile = (float *)malloc((size_t)size * 3 * sizeof(float));
    stencil = (unsigned char *)calloc((size_t)size, 1);

    /* Convert RGBA8 tile to float RGB (alpha left in place) */
    for (y = 0; y < th; y++) {
        for (x = 0; x < tw; x++) {
            int idx = y * tw + x;
            unsigned char *px = tile + idx * 4;
            ftile[idx * 3 + 0] = px[0] / 255.0f;
            ftile[idx * 3 + 1] = px[1] / 255.0f;
            ftile[idx * 3 + 2] = px[2] / 255.0f;
        }
    }

    span = MSDF_EC_MIN_DEVIATION_RATIO / MSDF_PIXEL_RANGE;
    prot_radius = MSDF_EC_PROT_RADIUS_TOLERANCE / MSDF_PIXEL_RANGE;

    /* ── Phase 1a: Protect corners ──
       Mark 2×2 neighborhoods around color-change junctions. These corners
       are where MSDF's multi-channel encoding is most important. */
    {
        int ci, ei;
        for (ci = 0; ci < shape->contour_count; ci++) {
            const msdf_contour *c = &shape->contours[ci];
            if (c->edge_count < 2) continue;

            for (ei = 0; ei < c->edge_count; ei++) {
                const msdf_edge *prev = &c->edges[(ei > 0) ? ei - 1 : c->edge_count - 1];
                const msdf_edge *cur = &c->edges[ei];
                int common = prev->color & cur->color;
                float sx, sy, tile_fx, tile_fy;
                int px0, py0, px, py;

                /* !(x & (x-1)): true when x has at most one bit set (0 or power of 2).
                   This means at most one color is shared → color changes at this corner. */
                if (!(common & (common - 1))) {
                    sx = cur->p0x;
                    sy = cur->p0y;
                    tile_fx = sx - (float)ix0 + MSDF_RECT_MARGIN;
                    tile_fy = -sy - (float)iy0 + MSDF_RECT_MARGIN;

                    px0 = (int)floorf(tile_fx - 0.5f);
                    py0 = (int)floorf(tile_fy - 0.5f);
                    for (py = py0; py <= py0 + 1; py++)
                        for (px = px0; px <= px0 + 1; px++)
                            if (px >= 0 && px < tw && py >= 0 && py < th)
                                stencil[py * tw + px] |= MSDF_EC_PROTECTED;
                }
            }
        }
    }

    /* ── Phase 1b: Protect edges ──
       For each adjacent texel pair near the edge, determine which channels
       contribute to the real shape boundary and protect their extreme values. */
    {
        /* 4 neighbor directions: right, down, down-right, down-left */
        static const int adj_dx[] = {1, 0, 1, 1};
        static const int adj_dy[] = {0, 1, 1, -1};
        int d;

        for (y = 0; y < th; y++) {
            for (x = 0; x < tw; x++) {
                int idx = y * tw + x;
                const float *a = &ftile[idx * 3];
                float am = msdf_medianf(a[0], a[1], a[2]);

                for (d = 0; d < 4; d++) {
                    int nx = x + adj_dx[d], ny = y + adj_dy[d];
                    int nidx, edge_mask;
                    const float *b;
                    float bm;
                    if (nx < 0 || nx >= tw || ny < 0 || ny >= th) continue;

                    nidx = ny * tw + nx;
                    b = &ftile[nidx * 3];
                    bm = msdf_medianf(b[0], b[1], b[2]);

                    /* Only examine pairs close enough to the edge */
                    if (fabsf(am - 0.5f) + fabsf(bm - 0.5f) >= prot_radius)
                        continue;

                    edge_mask = msdf_ec_edge_between_texels(a, b);
                    msdf_ec_protect_extreme(stencil, idx, a, edge_mask);
                    msdf_ec_protect_extreme(stencil, nidx, b, edge_mask);
                }
            }
        }
    }

    /* ── Phase 2: Detect artifacts ──
       For each texel, simulate linear interpolation with 4 cardinal neighbors
       and bilinear interpolation with 4 diagonal neighbors. Find channel crossing
       points where the interpolated median deviates or inverts. Only flag the
       texel further from the edge (higher |median - 0.5|). */
    {
        /* Linear: right, down, left, up */
        static const int lin_dx[] = {1, 0, -1, 0};
        static const int lin_dy[] = {0, 1, 0, -1};
        int n;

        for (y = 0; y < th; y++) {
            for (x = 0; x < tw; x++) {
                int idx = y * tw + x;
                const float *a;
                float am;
                int is_prot;
                if (stencil[idx] & MSDF_EC_ERROR) continue;

                a = &ftile[idx * 3];
                am = msdf_medianf(a[0], a[1], a[2]);
                is_prot = (stencil[idx] & MSDF_EC_PROTECTED) != 0;

                /* Check 4 linear neighbors */
                for (n = 0; n < 4; n++) {
                    int nx = x + lin_dx[n], ny = y + lin_dy[n];
                    const float *b;
                    float bm;
                    if (nx < 0 || nx >= tw || ny < 0 || ny >= th) continue;

                    b = &ftile[(ny * tw + nx) * 3];
                    bm = msdf_medianf(b[0], b[1], b[2]);

                    /* Only flag the texel further from the edge */
                    if (fabsf(am - 0.5f) >= fabsf(bm - 0.5f)) {
                        if (msdf_ec_has_linear_artifact(a, b, span, is_prot)) {
                            stencil[idx] |= MSDF_EC_ERROR;
                            break;
                        }
                    }
                }
                if (stencil[idx] & MSDF_EC_ERROR) continue;

                /* Check 4 diagonal neighbors.
                   For diagonal (dx,dy): b = (x+dx, y), c = (x, y+dy), d = (x+dx, y+dy) */
                {
                    static const int diag_dx[] = {1, -1, -1, 1};
                    static const int diag_dy[] = {1, 1, -1, -1};

                    for (n = 0; n < 4; n++) {
                        int ddx = diag_dx[n], ddy = diag_dy[n];
                        int nx = x + ddx, ny = y + ddy;
                        int bx = x + ddx, by = y;
                        int cx = x, cy = y + ddy;
                        const float *dd, *bb, *cc;
                        float dm;

                        if (nx < 0 || nx >= tw || ny < 0 || ny >= th) continue;
                        if (bx < 0 || bx >= tw) continue;
                        if (cy < 0 || cy >= th) continue;

                        dd = &ftile[(ny * tw + nx) * 3];
                        bb = &ftile[(by * tw + bx) * 3];
                        cc = &ftile[(cy * tw + cx) * 3];
                        dm = msdf_medianf(dd[0], dd[1], dd[2]);

                        if (fabsf(am - 0.5f) >= fabsf(dm - 0.5f)) {
                            if (msdf_ec_has_diagonal_artifact(a, bb, cc, dd, span, is_prot)) {
                                stencil[idx] |= MSDF_EC_ERROR;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    /* ── Phase 3: Apply correction ──
       Set R=G=B=median at all artifact texels. This neutralizes the multi-channel
       encoding, falling back to single-channel SDF at these pixels. */
    for (y = 0; y < th; y++) {
        for (x = 0; x < tw; x++) {
            int idx = y * tw + x;
            if (stencil[idx] & MSDF_EC_ERROR) {
                float m = msdf_medianf(ftile[idx*3], ftile[idx*3+1], ftile[idx*3+2]);
                unsigned char mv = (unsigned char)(msdf_clampf(m, 0, 1) * 255.0f + 0.5f);
                unsigned char *px = tile + idx * 4;
                px[0] = mv;
                px[1] = mv;
                px[2] = mv;
                /* Alpha channel untouched */
            }
        }
    }

    free(ftile);
    free(stencil);
}

/* ── Main API: build atlas for ASCII range ──────────────────────────────── */

static int msdf_build_atlas(stbtt_fontinfo *info,
                             msdf_glyph *glyphs, int first_cp, int last_cp,
                             msdf_atlas *atlas,
                             float *out_ascent, float *out_descent, float *out_line_gap)
{
    float scale = stbtt_ScaleForPixelHeight(info, (float)MSDF_SOURCE_SIZE);
    float inv_source = 1.0f / (float)MSDF_SOURCE_SIZE;
    int cp;

    /* Font metrics in normalized units */
    {
        int asc, desc, lg;
        stbtt_GetFontVMetrics(info, &asc, &desc, &lg);
        float unit_scale = stbtt_ScaleForPixelHeight(info, 1.0f);
        *out_ascent = (float)asc * unit_scale;
        *out_descent = (float)desc * unit_scale;
        *out_line_gap = (float)lg * unit_scale;
    }

    msdf_atlas_init(atlas);

    for (cp = first_cp; cp < last_cp; cp++) {
        int glyph_idx = stbtt_FindGlyphIndex(info, cp);
        msdf_glyph *g = &glyphs[cp];
        memset(g, 0, sizeof(*g));

        /* Advance width (normalized) */
        {
            int adv, lsb;
            stbtt_GetGlyphHMetrics(info, glyph_idx, &adv, &lsb);
            float unit_scale = stbtt_ScaleForPixelHeight(info, 1.0f);
            g->advance = (float)adv * unit_scale;
        }

        /* Bounding box in source_size pixels */
        int ix0, iy0, ix1, iy1;
        stbtt_GetGlyphBitmapBox(info, glyph_idx, scale, scale, &ix0, &iy0, &ix1, &iy1);
        int bw = ix1 - ix0;
        int bh = iy1 - iy0;

        if (bw <= 0 || bh <= 0)
            continue;

        /* Tile size including margin */
        int tw = bw + 2 * MSDF_RECT_MARGIN;
        int th = bh + 2 * MSDF_RECT_MARGIN;

        /* Store offset including margin (in normalized font units).
           ix0/iy0 are the bitmap box top-left relative to glyph origin.
           We subtract margin so the quad starts at the margin edge. */
        g->xoff = (float)(ix0 - MSDF_RECT_MARGIN) * inv_source;
        g->yoff = (float)(iy0 - MSDF_RECT_MARGIN) * inv_source;
        g->width = tw;
        g->height = th;

        /* Extract outline and generate MSDF */
        msdf_shape shape;
        msdf_extract_shape(&shape, info, glyph_idx, scale);
        msdf_color_edges(&shape);

        unsigned char *tile = (unsigned char *)calloc((size_t)(tw * th * 4), 1);

        /* Generate MSDF tile using per-edge signed pseudo-distance.
           Each color channel gets its own sign from its nearest edge's tangent
           cross product — this preserves sharp corners in the multi-channel encoding.
           Map tile pixels to outline coordinates (y-up font space):
             outline_x = ix0 + (px - MARGIN)
             outline_y = -(iy0 + (py - MARGIN)) */
        {
            int px, py;
            for (py = 0; py < th; py++) {
                for (px = 0; px < tw; px++) {
                    float outline_x = (float)(ix0 + px - MSDF_RECT_MARGIN);
                    float outline_y = -(float)(iy0 + py - MSDF_RECT_MARGIN);

                    /* Per-channel signed distance with dot tiebreaker (msdfgen algorithm) */
                    msdf_signed_distance sd_r = {-DBL_MAX, 0};
                    msdf_signed_distance sd_g = {-DBL_MAX, 0};
                    msdf_signed_distance sd_b = {-DBL_MAX, 0};
                    int cii, eii;

                    for (cii = 0; cii < shape.contour_count; cii++) {
                        const msdf_contour *c = &shape.contours[cii];
                        for (eii = 0; eii < c->edge_count; eii++) {
                            const msdf_edge *e = &c->edges[eii];
                            msdf_signed_distance sd = msdf_edge_sd(e, (double)outline_x, (double)outline_y);

                            if ((e->color & MSDF_RED)   && msdf_sd_lt(sd, sd_r)) sd_r = sd;
                            if ((e->color & MSDF_GREEN) && msdf_sd_lt(sd, sd_g)) sd_g = sd;
                            if ((e->color & MSDF_BLUE)  && msdf_sd_lt(sd, sd_b)) sd_b = sd;
                        }
                    }

                    /* Map signed distance to [0..1]: inside (positive) > 0.5 */
                    float r = 0.5f + (float)(sd_r.distance / MSDF_PIXEL_RANGE);
                    float gg = 0.5f + (float)(sd_g.distance / MSDF_PIXEL_RANGE);
                    float b = 0.5f + (float)(sd_b.distance / MSDF_PIXEL_RANGE);
                    /* Alpha = median of channels (reconstructed true SDF) */
                    float a = msdf_medianf(r, gg, b);

                    unsigned char *px_out = tile + (py * tw + px) * 4;
                    px_out[0] = (unsigned char)(msdf_clampf(r, 0, 1) * 255.0f + 0.5f);
                    px_out[1] = (unsigned char)(msdf_clampf(gg, 0, 1) * 255.0f + 0.5f);
                    px_out[2] = (unsigned char)(msdf_clampf(b, 0, 1) * 255.0f + 0.5f);
                    px_out[3] = (unsigned char)(msdf_clampf(a, 0, 1) * 255.0f + 0.5f);
                }
            }
        }

        /* Error correction: neutralize artifacts at channel boundaries */
        msdf_error_correct_tile(tile, tw, th, &shape, ix0, iy0);

        /* Pack into atlas */
        int ax, ay;
        if (msdf_atlas_pack(atlas, tile, tw, th, &ax, &ay) != 0) {
            free(tile);
            msdf_shape_free(&shape);
            return -1;
        }

        g->atlas_x = ax;
        g->atlas_y = ay;
        g->u0 = (float)ax / (float)atlas->width;
        g->v0 = (float)ay / (float)atlas->height;
        g->u1 = (float)(ax + tw) / (float)atlas->width;
        g->v1 = (float)(ay + th) / (float)atlas->height;

        free(tile);
        msdf_shape_free(&shape);
    }

    printf("MSDF atlas built: %dx%d, shelf_y=%d\n",
           atlas->width, atlas->height, atlas->shelf_y + atlas->shelf_h);
    return 0;
}

#endif /* MSDF_GEN_H */
