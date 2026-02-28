#ifndef DEBUG_RENDER_H
#define DEBUG_RENDER_H

#include "math3d.h"

/* Debug color (RGB float) */
typedef struct {
    float r, g, b;
} debug_color;

/* Debug renderer state */
#define DEBUG_RENDER_MAX_LINES 256

typedef struct debug_renderer {
    float* vertex_buffer;  /* x,y,r,g,b for each line endpoint — arena-allocated */
    int max_lines;
    int current_line_count;
} debug_renderer;

/* Predefined colors */
static const debug_color DEBUG_RED    = {1.0f, 0.0f, 0.0f};
static const debug_color DEBUG_GREEN  = {0.0f, 1.0f, 0.0f};
static const debug_color DEBUG_BLUE   = {0.0f, 0.0f, 1.0f};
static const debug_color DEBUG_YELLOW = {1.0f, 1.0f, 0.0f};
static const debug_color DEBUG_CYAN   = {0.0f, 1.0f, 1.0f};
static const debug_color DEBUG_WHITE  = {1.0f, 1.0f, 1.0f};

/* Debug draw functions (implemented in engine/debug_render.c) */
void debug_draw_line(debug_renderer *dr, vec2 start, vec2 end, debug_color color);
void debug_draw_rect(debug_renderer *dr, vec2 center, float width, float height, debug_color color);

/* Reset debug lines for new frame */
static inline void debug_renderer_reset(debug_renderer *r) {
    r->current_line_count = 0;
}

#endif /* DEBUG_RENDER_H */
