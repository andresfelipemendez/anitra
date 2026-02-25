#ifndef RENDERER_H
#define RENDERER_H
#include <game.h>

rect pixel_to_uv(pixel_rect p, sprite_sheet* s);
void update_camera_matrix(camera* cam, float* matrix);
void update_animation(game* g);
void render_tile(game* g, int tile, float x, float y);
void render_tiles(game* g);
void render_entities(game* g);
void render_health_bar(game* g, float x, float y, float health, float max_health);

#endif
