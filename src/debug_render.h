#ifndef DEBUG_RENDER_H
#define DEBUG_RENDER_H

#include "math3d.h"
#include <stdint.h>

/* Debug colors */
typedef enum {
    DEBUG_RED,
    DEBUG_GREEN,
    DEBUG_BLUE,
    DEBUG_YELLOW,
    DEBUG_CYAN,
    DEBUG_MAGENTA,
    DEBUG_WHITE,
    DEBUG_BLACK
} DebugColor;

/* Debug renderer state */
#define MAX_DEBUG_LINES 256

typedef struct debug_renderer {
    float vertex_buffer[MAX_DEBUG_LINES * 10]; /* x,y,r,g,b for each line endpoint */
    int current_line_count;
    int max_lines;
} debug_renderer;

/* Initialize debug renderer */
static inline void debug_renderer_init(debug_renderer *r, int max_lines) {
    r->current_line_count = 0;
    r->max_lines = max_lines;
}

/* Add a debug line (world space) */
static inline void debug_draw_line(debug_renderer *r, vec2 start, vec2 end, DebugColor color) {
    if (r->current_line_count >= r->max_lines) return;
    
    int idx = r->current_line_count * 10;
    float cr, cg, cb;
    
    switch (color) {
        case DEBUG_RED:       cr = 1.0f; cg = 0.0f; cb = 0.0f; break;
        case DEBUG_GREEN:     cr = 0.0f; cg = 1.0f; cb = 0.0f; break;
        case DEBUG_BLUE:      cr = 0.0f; cg = 0.0f; cb = 1.0f; break;
        case DEBUG_YELLOW:    cr = 1.0f; cg = 1.0f; cb = 0.0f; break;
        case DEBUG_CYAN:      cr = 0.0f; cg = 1.0f; cb = 1.0f; break;
        case DEBUG_MAGENTA:   cr = 1.0f; cg = 0.0f; cb = 1.0f; break;
        case DEBUG_WHITE:     cr = 1.0f; cg = 1.0f; cb = 1.0f; break;
        case DEBUG_BLACK:     cr = 0.0f; cg = 0.0f; cb = 0.0f; break;
        default:              cr = 1.0f; cg = 1.0f; cb = 1.0f; break;
    }
    
    r->vertex_buffer[idx++] = start.x;
    r->vertex_buffer[idx++] = start.y;
    r->vertex_buffer[idx++] = cr;
    r->vertex_buffer[idx++] = cg;
    r->vertex_buffer[idx++] = cb;
    
    r->vertex_buffer[idx++] = end.x;
    r->vertex_buffer[idx++] = end.y;
    r->vertex_buffer[idx++] = cr;
    r->vertex_buffer[idx++] = cg;
    r->vertex_buffer[idx++] = cb;
    
    r->current_line_count++;
}

/* Reset debug lines for new frame */
static inline void debug_renderer_reset(debug_renderer *r) {
    r->current_line_count = 0;
}

#endif /* DEBUG_RENDER_H */
