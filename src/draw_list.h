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
    draw_command *sprites;
    int sprite_count;
    int sprite_capacity;
    debug_line_command *lines;
    int line_count;
    int line_capacity;
    float view_matrix[16];
    float ortho_projection[16];
} draw_list;

#endif /* DRAW_LIST_H */
