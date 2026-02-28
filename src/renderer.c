#include "game.h"
#include "renderer.h"
#include <math.h>

/* Update camera matrix (orthographic projection) */
void update_camera_matrix(camera *cam, float *out_matrix) {
    if (!cam || !out_matrix) return;
    
    /* Clear matrix */
    memset(out_matrix, 0, 16 * sizeof(float));
    
    /* Set diagonal elements for scale */
    out_matrix[0] = cam->zoom;
    out_matrix[5] = -cam->zoom; /* Flip Y for OpenGL-style coordinates */
    out_matrix[10] = 1.0f;
    out_matrix[15] = 1.0f;
    
    /* Set translation (camera position) */
    out_matrix[12] = -cam->position.x * cam->zoom;
    out_matrix[13] = cam->position.y * cam->zoom; /* Note: no flip for Y translation */
}

/* Render tile map */
void render_tiles(memory *g) {
    if (!g || !g->gameplay) return;
    
    /* Placeholder - implement actual tile rendering here */
    /* This would iterate over the level array and render tiles at their positions */
}

/* Render game entities */
void render_entities(memory *g) {
    if (!g || !g->gameplay) return;
    
    /* Placeholder - implement entity rendering using draw_list */
    /* For now, just ensure we have space in the draw list */
    if (g->draw_list.sprite_capacity == 0) {
        g->draw_list.sprite_capacity = 256;
        g->draw_list.sprite_count = 0;
    }
}

/* Update animation for all entities */
void update_animation(memory *g) {
    if (!g || !g->gameplay) return;
    
    /* Placeholder - implement animation updates per entity */
    /* This would advance timers and update frame_index for each entity's animator */
}
