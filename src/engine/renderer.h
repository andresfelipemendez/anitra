#ifndef RENDERER_H
#define RENDERER_H
#include <game.h>

rect pixel_to_uv(pixel_rect p, sprite_sheet* s);
void update_camera_matrix(camera* cam, float* matrix);
void update_animation(memory* g);
void render_tile(memory* g, int tile, float x, float y);
void render_tiles(memory* g);
void render_entities(memory* g);
void render_health_bar(memory* g, float x, float y, float health, float max_health);

#endif
