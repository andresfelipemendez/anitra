#ifndef DRAW_LIST_H
#define DRAW_LIST_H

#include "line_util.h"

#define MAX_DRAW_COMMANDS 256
#define DRAW_LIST_MAX_DEBUG_LINES 128
#define DRAW_LIST_MAX_MESH_COMMANDS 256

typedef struct draw_command {
    int texture_id;
    float x, y;
    float width, height;
    float uv_x, uv_y;
    float uv_w, uv_h;
    float tint_r, tint_g, tint_b, tint_a;
} draw_command;

typedef struct mesh_draw_command {
    int model_asset_index;
    int use_skinned_bones;
    int bone_buffer_offset;
    float model[16];
} mesh_draw_command;

typedef struct draw_list {
    draw_command *sprites;
    int sprite_count;
    int sprite_capacity;
    line_vert *line_verts;     /* pre-expanded AA line quads (6 verts per line) */
    int line_count;            /* number of line segments */
    int line_capacity;
    float line_width;          /* line width in pixels */
    mesh_draw_command *meshes;
    int mesh_count;
    int mesh_capacity;
    float view_matrix[16];
    float ortho_projection[16];
} draw_list;

#endif /* DRAW_LIST_H */
