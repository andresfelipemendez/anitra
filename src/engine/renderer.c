#include "renderer.h"

#define RENDER_QUERY_MAX_RESULTS 256

typedef struct sprite_render_item {
    int texture_id;
    float x;
    float y;
    pixel_rect pixel_region;
    int texture_width;
    int texture_height;
    int has_health;
    float health;
    float max_health;
} sprite_render_item;

static transform_component *find_transform_component(game_state *gs, int entity_index) {
    int i;
    if (!gs || !gs->transform_components) return NULL;
    for (i = 0; i < gs->transform_component_count; i++) {
        transform_component *tc = &gs->transform_components[i];
        if (tc->entity_index == entity_index) return tc;
    }
    return NULL;
}

static parent_transform_component *find_parent_transform_component(game_state *gs, int entity_index) {
    int i;
    if (!gs || !gs->parent_transform_components) return NULL;
    for (i = 0; i < gs->parent_transform_component_count; i++) {
        parent_transform_component *pt = &gs->parent_transform_components[i];
        if (pt->entity_index == entity_index) return pt;
    }
    return NULL;
}

static Vec3 resolve_world_position(game_state *gs, int entity_index) {
    Vec3 world = VEC3(0.0f, 0.0f, 0.0f);
    int current = entity_index;
    int guard = 0;
    if (!gs || !gs->scene_entities || !gs->transform_components) return world;

    while (guard <= gs->scene_entity_count && current >= 0 && current < gs->scene_entity_count) {
        transform_component *tc = find_transform_component(gs, current);
        parent_transform_component *pt;
        if (tc) {
            world = vec3_add(world, tc->position);
        }

        pt = find_parent_transform_component(gs, current);
        if (!pt) break;
        if (pt->parent_entity_index == current) break;
        current = pt->parent_entity_index;
        guard++;
    }

    return world;
}

static health_component *find_health_component(game_state *gs, int entity_index) {
    int i;
    if (!gs || !gs->health_components) return NULL;
    for (i = 0; i < gs->health_component_count; i++) {
        health_component *hc = &gs->health_components[i];
        if (hc->entity_index == entity_index) return hc;
    }
    return NULL;
}

static int query_sprite_render_items(game_state *gs, sprite_render_item *out_items, int max_items) {
    int i;
    int count = 0;
    if (!gs || !out_items || max_items <= 0 || !gs->scene_entities || !gs->mesh_components) return 0;

    for (i = 0; i < gs->mesh_component_count && count < max_items; i++) {
        mesh_component *mc = &gs->mesh_components[i];
        int entity_index = mc->entity_index;
        transform_component *tc;
        health_component *hc;
        entity *e;
        int sprite_id;
        sprite_render_item *item;
        Vec3 world_pos;

        if (entity_index < 0 || entity_index >= gs->scene_entity_count) continue;
        if (!mc->visible) continue;

        tc = find_transform_component(gs, entity_index);
        if (!tc) continue;
        world_pos = resolve_world_position(gs, entity_index);

        e = &gs->scene_entities[entity_index];
        if (e->sprite_sheet.width <= 0 || e->sprite_sheet.height <= 0) continue;
        if (e->current_animation.animation.frame_count <= 0) continue;
        if (e->current_animation.frame_index < 0) continue;

        sprite_id = e->current_animation.animation.frames[e->current_animation.frame_index];
        if (sprite_id < 0 || sprite_id >= 64) continue;

        hc = find_health_component(gs, entity_index);
        item = &out_items[count++];
        item->texture_id = e->sprite_sheet.texture_id;
        item->x = world_pos.x;
        item->y = world_pos.y;
        item->pixel_region = e->sprite_sheet.sprites[sprite_id];
        item->texture_width = e->sprite_sheet.width;
        item->texture_height = e->sprite_sheet.height;
        if (hc) {
            item->has_health = 1;
            item->health = hc->health;
            item->max_health = hc->max_health;
        } else {
            item->has_health = 0;
            item->health = 0.0f;
            item->max_health = 0.0f;
        }
    }

    return count;
}

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

    for (i = 0; i < gs->scene_entity_count; i++) {
        entity* e = &gs->scene_entities[i];
        animator* a = &e->current_animation;
        if (a->animation.frame_time <= 0.0f || a->animation.frame_count <= 0)
            continue;
        e->current_animation.timer += gs->dt;

        while (a->timer >= a->animation.frame_time) {
            a->timer -= a->animation.frame_time;
            a->frame_index = (a->frame_index + 1) % a->animation.frame_count;
        }
    }
}

void render_tile(game_state* gs, int tile, float x, float y) {
    (void)gs;
    (void)tile;
    (void)x;
    (void)y;
}

void render_tiles(game_state* gs) {
    (void)gs;
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
    sprite_render_item items[RENDER_QUERY_MAX_RESULTS];
    int item_count;
    int i;
    if (!gs || !gs->scene_entities) return;

    item_count = query_sprite_render_items(gs, items, RENDER_QUERY_MAX_RESULTS);
    for (i = 0; i < item_count; i++) {
        const sprite_render_item *item = &items[i];
        render_sprite_pixel_perfect(gs, item->texture_id, item->x, item->y,
                                   item->pixel_region, item->texture_width, item->texture_height);

        if (item->has_health) {
            render_health_bar(gs, item->x, item->y, item->health, item->max_health);
        }
    }
}
