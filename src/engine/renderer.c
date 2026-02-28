#include "renderer.h"
#include <scene.h>

rect pixel_to_uv(pixel_rect p, sprite_sheet* s) {
    rect uv;
    uv.x = (float)p.x / (float)s->width;
    uv.y = (float)p.y / (float)s->height;
    uv.w = (float)p.w / (float)s->width;
    uv.h = (float)p.h / (float)s->height;
    return uv;
}

void update_camera_matrix(camera* cam, float* matrix) {
    int i;
    for (i = 0; i < 16; i++) matrix[i] = 0.0f;

    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;

    matrix[0] = cam->zoom;
    matrix[5] = cam->zoom;

    matrix[12] = -cam->position.x * cam->zoom;
    matrix[13] = -cam->position.y * cam->zoom;
}

static void push_sprite(draw_list* dl, int texture_id, float x, float y,
                        float w, float h, float uv_x, float uv_y,
                        float uv_w, float uv_h,
                        float r, float g, float b, float a) {
    draw_command* cmd;
    if (dl->sprite_count >= dl->sprite_capacity) return;
    cmd = &dl->sprites[dl->sprite_count++];
    cmd->texture_id = texture_id;
    cmd->x = x; cmd->y = y;
    cmd->width = w; cmd->height = h;
    cmd->uv_x = uv_x; cmd->uv_y = uv_y;
    cmd->uv_w = uv_w; cmd->uv_h = uv_h;
    cmd->tint_r = r; cmd->tint_g = g;
    cmd->tint_b = b; cmd->tint_a = a;
}

void render_sprite_pixel_perfect(game_state* gs, int texture_id, float x, float y,
                                 pixel_rect sprite_rect, int texture_width, int texture_height) {
    float quad_width = sprite_rect.w * 4.0f;
    float quad_height = sprite_rect.h * 4.0f;

    float uv_x = (float)sprite_rect.x / (float)texture_width;
    float uv_y = (float)sprite_rect.y / (float)texture_height;
    float uv_w = (float)sprite_rect.w / (float)texture_width;
    float uv_h = (float)sprite_rect.h / (float)texture_height;

    push_sprite(&gs->dl, texture_id, x, y,
                quad_width, quad_height,
                uv_x, uv_y, uv_w, uv_h,
                1.0f, 1.0f, 1.0f, 1.0f);
}

void render_scaled_sprite(game_state* gs, int texture_id, float x, float y, float width, float height) {
    push_sprite(&gs->dl, texture_id, x, y,
                width, height,
                0.0f, 0.0f, 1.0f, 1.0f,
                1.0f, 1.0f, 1.0f, 1.0f);
}

void update_animation(game_state* gs) {
    int i;
    if (!gs) return;

    for (i = 0; i < scene.entity_count; i++) {
        entity* e = &scene.entities[i];
        animator* a = &e->current_animation;
        e->current_animation.timer += gs->dt;

        while (a->timer >= a->animation.frame_time) {
            a->timer -= a->animation.frame_time;
            a->frame_index = (a->frame_index + 1) % a->animation.frame_count;
        }
    }
}

void render_tile(game_state* gs, int tile, float x, float y) {
    pixel_rect pixel_region = tiles.sprites[tile];
    render_sprite_pixel_perfect(gs, TEXTURE_TILES, x, y,
                               pixel_region, tiles.width, tiles.height);
}

void render_tiles(game_state* gs) {
    int rows = sizeof(level) / sizeof(level[0]);
    int cols = sizeof(level[0]) / sizeof(level[0][0]);
    int y, x;
    for (y = 0; y < rows; y++) {
        for (x = 0; x < cols; x++) {
            int tile = level[rows - 1 - y][cols - 1 - x];
            render_tile(gs, tile, x * 64, y * 64);
        }
    }
}

void render_health_bar(game_state* gs, float x, float y, float health, float max_health) {
    float health_ratio;
    float bar_x, bar_y;

    if (!gs || max_health <= 0.0f) return;

    health_ratio = health / max_health;
    if (health_ratio > 1.0f) health_ratio = 1.0f;
    if (health_ratio < 0.0f) health_ratio = 0.0f;

    bar_x = x;
    bar_y = y + 64.0f;

    if (health_ratio > 0.0f) {
        float fill_min = 34.0f;
        float fill_max = 112.0f;
        float fill_range = fill_max - fill_min;
        float fill_width = fill_min + (fill_range * health_ratio);
        float fill_offset = -(128.0f - fill_width) * 0.5f;
        render_scaled_sprite(gs, TEXTURE_HEALTH_FILL,
                           bar_x + fill_offset, bar_y, fill_width, 32.0f);
    }

    render_scaled_sprite(gs, TEXTURE_HEALTH_BAR,
                       bar_x, bar_y, 128.0f, 32.0f);
}

void render_entities(game_state* gs) {
    int i;
    if (!gs) return;

    for (i = 0; i < scene.entity_count; i++) {
        entity* e = &scene.entities[i];
        float x = e->pos.x;
        float y = e->pos.y;
        int sprite_id = e->current_animation.animation.frames[e->current_animation.frame_index];
        pixel_rect pixel_region = e->sprite_sheet.sprites[sprite_id];

        render_sprite_pixel_perfect(gs, e->sprite_sheet.texture_id, x, y,
                                   pixel_region, e->sprite_sheet.width, e->sprite_sheet.height);

        render_health_bar(gs, x, y, e->health, 100.0f);
    }
}
