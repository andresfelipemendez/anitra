#include "renderer.h"
#include <scene.h>
#include <SDL3/SDL.h>

void update_camera_matrix(struct camera* cam, float* matrix) {
    for (int i = 0; i < 16; i++) matrix[i] = 0.0f;
    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
    matrix[0] = cam->zoom;
    matrix[5] = cam->zoom;
    matrix[12] = -cam->position.x * cam->zoom;
    matrix[13] = -cam->position.y * cam->zoom;
}

static void world_to_screen(struct game* g, float wx, float wy, float* sx, float* sy) {
    float cx = g->camera.position.x;
    float cy = g->camera.position.y;
    float zoom = g->camera.zoom;
    *sx = (wx - cx) * zoom + g->width * 0.5f;
    *sy = g->height * 0.5f - (wy - cy) * zoom;
}

void render_sprite_pixel_perfect(struct game* g, SDL_Texture* texture, float x, float y,
                                  pixel_rect sprite_rect, int texture_width, int texture_height) {
    if (!g || !g->renderer || !texture) return;
    (void)texture_width;
    (void)texture_height;

    float quad_width = sprite_rect.w * 4.0f;
    float quad_height = sprite_rect.h * 4.0f;

    float screen_x, screen_y;
    world_to_screen(g, x, y, &screen_x, &screen_y);

    float zoom = g->camera.zoom;

    SDL_FRect src = {
        (float)sprite_rect.x, (float)sprite_rect.y,
        (float)sprite_rect.w, (float)sprite_rect.h
    };
    SDL_FRect dst = {
        screen_x - (quad_width * zoom * 0.5f),
        screen_y - (quad_height * zoom * 0.5f),
        quad_width * zoom,
        quad_height * zoom
    };

    SDL_RenderTexture(g->renderer, texture, &src, &dst);
}

void render_scaled_sprite(struct game* g, SDL_Texture* texture, float x, float y,
                           float width, float height) {
    if (!g || !g->renderer || !texture) return;

    float screen_x, screen_y;
    world_to_screen(g, x, y, &screen_x, &screen_y);

    float zoom = g->camera.zoom;

    SDL_FRect dst = {
        screen_x - (width * zoom * 0.5f),
        screen_y - (height * zoom * 0.5f),
        width * zoom,
        height * zoom
    };

    SDL_RenderTexture(g->renderer, texture, NULL, &dst);
}

void update_animation(struct game* g) {
    if (!g) return;

    for (int i = 0; i < scene.entity_count; i++) {
        entity* e = &scene.entities[i];
        struct animator* a = &e->current_animation;
        e->current_animation.timer += g->dt;

        while (a->timer >= a->animation.frame_time) {
            a->timer -= a->animation.frame_time;
            a->frame_index = (a->frame_index + 1) % a->animation.frame_count;
        }
    }
}

void render_tile(struct game* g, int tile, float x, float y) {
    if (!g || !g->textures[TEXTURE_TILES]) return;

    pixel_rect pixel_region = tiles.sprites[tile];
    render_sprite_pixel_perfect(g, g->textures[TEXTURE_TILES], x, y,
                                pixel_region, tiles.width, tiles.height);
}

void render_tiles(struct game* g) {
    int rows = sizeof(level) / sizeof(level[0]);
    int cols = sizeof(level[0]) / sizeof(level[0][0]);

    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            int tile = level[rows - 1 - y][cols - 1 - x];
            render_tile(g, tile, x * 64, y * 64);
        }
    }
}

void render_health_bar(struct game* g, float x, float y, float health, float max_health) {
    if (!g || max_health <= 0.0f || !g->textures[TEXTURE_HEALTH_BAR] || !g->textures[TEXTURE_HEALTH_FILL])
        return;

    float health_ratio = health / max_health;
    if (health_ratio > 1.0f) health_ratio = 1.0f;
    if (health_ratio < 0.0f) health_ratio = 0.0f;

    float bar_x = x;
    float bar_y = y + 64.0f;

    if (health_ratio > 0.0f) {
        float fill_min = 34.0f;
        float fill_max = 112.0f;
        float fill_range = fill_max - fill_min;
        float fill_width = fill_min + (fill_range * health_ratio);
        float fill_offset = -(128.0f - fill_width) * 0.5f;
        render_scaled_sprite(g, g->textures[TEXTURE_HEALTH_FILL],
                             bar_x + fill_offset, bar_y, fill_width, 32.0f);
    }

    render_scaled_sprite(g, g->textures[TEXTURE_HEALTH_BAR],
                         bar_x, bar_y, 128.0f, 32.0f);
}

void render_entities(struct game* g) {
    if (!g || !g->renderer) return;

    for (int i = 0; i < scene.entity_count; i++) {
        entity* e = &scene.entities[i];
        float x = e->pos.x;
        float y = e->pos.y;
        int sprite_id = e->current_animation.animation.frames[e->current_animation.frame_index];

        if (g->textures[e->sprite_sheet.texture_id]) {
            pixel_rect pixel_region = e->sprite_sheet.sprites[sprite_id];
            render_sprite_pixel_perfect(g, g->textures[e->sprite_sheet.texture_id], x, y,
                                        pixel_region, e->sprite_sheet.width, e->sprite_sheet.height);
        }

        render_health_bar(g, x, y, e->health, 100.0f);
    }
}
