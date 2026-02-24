#ifndef DEBUG_RENDER_H
#define DEBUG_RENDER_H

typedef struct { float x, y; } vec2;

typedef struct {
    float r, g, b;
} debug_color;

#define DEBUG_RED   (debug_color){1.0f, 0.0f, 0.0f}
#define DEBUG_GREEN (debug_color){0.0f, 1.0f, 0.0f}
#define DEBUG_BLUE  (debug_color){0.0f, 0.0f, 1.0f}
#define DEBUG_YELLOW (debug_color){1.0f, 1.0f, 0.0f}

typedef struct {
    float* vertex_buffer;
    int current_line_count;
    int max_lines;
} debug_renderer;

void debug_draw_line(debug_renderer* dr, vec2 start, vec2 end, debug_color color);
void debug_draw_rect(debug_renderer* dr, vec2 center, float width, float height, debug_color color);

#endif
