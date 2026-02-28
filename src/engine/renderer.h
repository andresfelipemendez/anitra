#ifndef RENDERER_H
#define RENDERER_H
#include <game.h>

rect pixel_to_uv(pixel_rect p, sprite_sheet* s);
void update_camera_matrix(camera* cam, float* matrix);
void update_animation(game_state* gs);
void render_tile(game_state* gs, int tile, float x, float y);
void render_tiles(game_state* gs);
void render_entities(game_state* gs);
void render_health_bar(game_state* gs, float x, float y, float health, float max_health);

#endif
