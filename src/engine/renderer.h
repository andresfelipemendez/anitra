#ifndef RENDERER_H
#define RENDERER_H
#include <game.h>
rect pixel_to_uv(pixel_rect p, sprite_sheet& s) ;
void debug_draw_line(debug_renderer* dr, vec2 start, vec2 end, debug_color color) ;
void update_camera_matrix(camera* cam, float* matrix) ;
void render_sprite(game* g, GLuint texture, float x, float y);
void render_sprite_region(game* g, GLuint texture, float x, float y, rect region) ;
void render_sprite_pixel_perfect(game* g, GLuint texture, float x, float y, pixel_rect sprite_rect, int texture_width, int texture_height);
void update_animation(game* g);
void render_tile(game* g, int tile, float x, float y) ;
void render_tiles(game* g);
void render_entities(game* g);
void render_health_bar(game* g, float x, float y, float health, float max_health);
void render_scaled_sprite(game* g, GLuint texture, float x, float y, float width, float height);
#endif