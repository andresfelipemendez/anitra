#ifndef DRAW_LIST_H
#define DRAW_LIST_H

#define MAX_DRAW_COMMANDS 256
#define MAX_DEBUG_LINES 128

typedef struct draw_command {
    int texture_id;
    float x, y;
    float width, height;
    float uv_x, uv_y;
    float uv_w, uv_h;
    float tint_r, tint_g, tint_b, tint_a;
} draw_command;

typedef struct debug_line_command {
    float x1, y1, x2, y2;
    float r, g, b;
} debug_line_command;

typedef struct draw_list {
    draw_command sprites[MAX_DRAW_COMMANDS];
    int sprite_count;
    debug_line_command lines[MAX_DEBUG_LINES];
    int line_count;
    float view_matrix[16];
    float ortho_projection[16];
} draw_list;

#endif /* DRAW_LIST_H */
