#ifndef RENDERER_H
#define RENDERER_H
#include <game.h>

void update_camera_matrix(struct camera* cam, float* matrix);
void update_animation(struct game* g);
void render_tile(struct game* g, int tile, float x, float y);
void render_tiles(struct game* g);
void render_entities(struct game* g);
void render_health_bar(struct game* g, float x, float y, float health, float max_health);
void render_sprite_pixel_perfect(struct game* g, SDL_Texture* texture, float x, float y, pixel_rect sprite_rect, int texture_width, int texture_height);
void render_scaled_sprite(struct game* g, SDL_Texture* texture, float x, float y, float width, float height);

#endif
