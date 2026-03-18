#ifndef LINE_UTIL_H
#define LINE_UTIL_H

/* Shared AA line vertex — used by both engine debug lines and editor 3D lines.
   Each line segment is expanded into a quad (6 vertices / 2 triangles).
   The vertex shader offsets by `side` in screen-space perpendicular direction. */

typedef struct line_vert {
    float x, y, z;       /* this endpoint position */
    float r, g, b;       /* color */
    float ox, oy, oz;    /* other endpoint position */
    float side;           /* -1 or +1 for quad expansion */
} line_vert;

/* Expand one line segment into 6 quad vertices (2 triangles).
   out must point to an array of at least 6 line_vert. */
static inline void line_expand(line_vert *out,
                               float x1, float y1, float z1,
                               float x2, float y2, float z2,
                               float r, float g, float b) {
    /* tri 1: A-, A+, B+ */
    out[0] = (line_vert){x1,y1,z1, r,g,b, x2,y2,z2, -1.0f};
    out[1] = (line_vert){x1,y1,z1, r,g,b, x2,y2,z2,  1.0f};
    out[2] = (line_vert){x2,y2,z2, r,g,b, x1,y1,z1,  1.0f};
    /* tri 2: A-, B+, B- */
    out[3] = (line_vert){x1,y1,z1, r,g,b, x2,y2,z2, -1.0f};
    out[4] = (line_vert){x2,y2,z2, r,g,b, x1,y1,z1,  1.0f};
    out[5] = (line_vert){x2,y2,z2, r,g,b, x1,y1,z1, -1.0f};
}

#define LINE_VERTS_PER_LINE 6

#endif /* LINE_UTIL_H */
