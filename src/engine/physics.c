#include "physics.h"
#include "game.h"
#include "debug_render.h"
#include <assert.h>

static void sync_collider_to_pos(entity *e, vec2 pos) {
    e->collider.rect.x = pos.x;
    e->collider.rect.y = pos.y;
}

static int animator_hitbox_active(const animator *a) {
    return a->animation.keyframes[0].frame != -1 &&
           a->frame_index >= a->animation.keyframes[0].frame &&
           a->frame_index <= a->animation.keyframes[1].frame;
}

static rect make_animator_hitbox(const entity *e, const animator *a) {
    rect hit_box = {
        e->pos.x + a->animation.collider.x,
        e->pos.y + a->animation.collider.y,
        a->animation.collider.w,
        a->animation.collider.h,
    };
    return hit_box;
}

static void draw_center_cross(debug_renderer *dbg, vec2 center, float cross_size, debug_color color) {
    vec2 center_h1 = {center.x - cross_size, center.y};
    vec2 center_h2 = {center.x + cross_size, center.y};
    vec2 center_v1 = {center.x, center.y - cross_size};
    vec2 center_v2 = {center.x, center.y + cross_size};

    debug_draw_line(dbg, center_h1, center_h2, color);
    debug_draw_line(dbg, center_v1, center_v2, color);
}

bool bbox_collide(const rect* a, const rect* b) {
    float a_left = a->x - a->w * 0.5f;
    float a_right = a->x + a->w * 0.5f;
    float a_top = a->y + a->h * 0.5f;
    float a_bottom = a->y - a->h * 0.5f;

    float b_left = b->x - b->w * 0.5f;
    float b_right = b->x + b->w * 0.5f;
    float b_top = b->y + b->h * 0.5f;
    float b_bottom = b->y - b->h * 0.5f;

    return (a_left < b_right && a_right > b_left &&
            a_bottom < b_top && a_top > b_bottom);
}

void collision(game_state* gs) {
    entity* player;
    const animator* pa;
    int player_hitbox_active;
    rect player_hit_box = {0};
    vec2 player_pos;
    vec2 predicted_player_pos;
    float cross_size = 8.0f;
    if (!gs || !gs->scene_entities || gs->scene_entity_count <= 0) return;
    player = &gs->scene_entities[0];
    pa = &player->current_animation;
    assert(player->type == PLAYER || player->type == ENEMY);

    predicted_player_pos.x = player->pos.x + (player->velocity.x * gs->dt);
    predicted_player_pos.y = player->pos.y + (player->velocity.y * gs->dt);
    sync_collider_to_pos(player, predicted_player_pos);

    debug_draw_rect(&gs->dbg, predicted_player_pos,
                    player->collider.rect.w, player->collider.rect.h, DEBUG_BLUE);

    player_pos.x = player->pos.x;
    player_pos.y = player->pos.y;
    draw_center_cross(&gs->dbg, player_pos, cross_size, DEBUG_RED);

    player_hitbox_active = animator_hitbox_active(pa);
    if (player_hitbox_active) {
        vec2 player_hit_center;
        player_hit_box = make_animator_hitbox(player, pa);
        player_hit_center.x = player_hit_box.x;
        player_hit_center.y = player_hit_box.y;
        debug_draw_rect(&gs->dbg, player_hit_center,
                        player_hit_box.w, player_hit_box.h, DEBUG_YELLOW);
    }

    for (int i = 1; i < gs->scene_entity_count; i++) {
        entity* e = &gs->scene_entities[i];
        animator* a = &e->current_animation;
        vec2 enemy_pos;
        debug_color color = DEBUG_GREEN;

        sync_collider_to_pos(e, e->pos);

        if (player_hitbox_active && bbox_collide(&player_hit_box, &e->collider.rect)) {
            color = DEBUG_RED;
            e->health -= 5 * gs->dt;
        }

        if (bbox_collide(&player->collider.rect, &e->collider.rect)) {
            color = DEBUG_RED;
            player->health -= 5.0f * gs->dt;
            if (player->health < 0.0f) {
                player->health = 0.0f;
            }
        }

        if (animator_hitbox_active(a)) {
            rect enemy_hit_box = make_animator_hitbox(e, a);
            vec2 enemy_hit_center;
            enemy_hit_center.x = enemy_hit_box.x;
            enemy_hit_center.y = enemy_hit_box.y;
            debug_draw_rect(&gs->dbg, enemy_hit_center,
                            enemy_hit_box.w, enemy_hit_box.h, DEBUG_YELLOW);
        }

        enemy_pos.x = e->collider.rect.x;
        enemy_pos.y = e->collider.rect.y;
        debug_draw_rect(&gs->dbg, enemy_pos, e->collider.rect.w, e->collider.rect.h, color);

        enemy_pos.x = e->pos.x;
        enemy_pos.y = e->pos.y;
        draw_center_cross(&gs->dbg, enemy_pos, cross_size, DEBUG_RED);
    }

    sync_collider_to_pos(player, player->pos);
}

void apply_movement(game_state* gs) {
    entity* player;
    if (!gs || !gs->scene_entities || gs->scene_entity_count <= 0) return;
    player = &gs->scene_entities[0];
    assert(player->type == PLAYER || player->type == ENEMY);

    vec2 new_pos = {
        player->pos.x + player->velocity.x * gs->dt,
        player->pos.y + player->velocity.y * gs->dt
    };

    sync_collider_to_pos(player, new_pos);

    bool collision_detected = false;

    for (int j = 1; j < gs->scene_entity_count; j++) {
        entity* other = &gs->scene_entities[j];
        sync_collider_to_pos(other, other->pos);

        if (bbox_collide(&player->collider.rect, &other->collider.rect)) {
            collision_detected = true;
            break;
        }
    }

    if (!collision_detected) {
        player->pos = new_pos;
    }

    player->velocity.x = 0.0f;
    player->velocity.y = 0.0f;

}
