#ifndef DEBUG_RENDER_H
#define DEBUG_RENDER_H

#include <glad.h>
#include <math.h>

typedef struct {
    float x, y;
} vec2;

typedef struct {
    float x, y;        
    float r, g, b;    
} debug_vertex;

typedef struct {
    float r, g, b;
} debug_color;

typedef struct {
    GLuint debug_shader;
    GLuint line_VAO, line_VBO;
    GLuint circle_VAO, circle_VBO;
    GLuint debug_projection_loc;
    GLuint debug_view_loc;
    GLuint debug_translation_loc;
    
    float* vertex_buffer;
    int max_lines;
    int current_line_count;
} debug_renderer;

void debug_draw_line(debug_renderer* dr, vec2 start, vec2 end, debug_color color);
void debug_draw_rect(debug_renderer* dr, vec2 center, float width, float height, debug_color color);

static const debug_color DEBUG_RED = {1.0f, 0.0f, 0.0f};
static const debug_color DEBUG_GREEN = {0.0f, 1.0f, 0.0f};
static const debug_color DEBUG_BLUE = {0.0f, 0.0f, 1.0f};
static const debug_color DEBUG_YELLOW = {1.0f, 1.0f, 0.0f};
static const debug_color DEBUG_WHITE = {1.0f, 1.0f, 1.0f};

#endif // DEBUG_RENDER_H
