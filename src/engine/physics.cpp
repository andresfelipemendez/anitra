#include "physics.h"
#include "game.h"
#include "debug_render.h"
#include <cassert>
#include <scene.h>

bool bbox_collide(struct collider a, struct collider b) {
    float a_left = a.rect.x - a.rect.w * 0.5f;
    float a_right = a.rect.x + a.rect.w * 0.5f;
    float a_top = a.rect.y + a.rect.h * 0.5f;
    float a_bottom = a.rect.y - a.rect.h * 0.5f;
    
    float b_left = b.rect.x - b.rect.w * 0.5f;
    float b_right = b.rect.x + b.rect.w * 0.5f;
    float b_top = b.rect.y + b.rect.h * 0.5f;
    float b_bottom = b.rect.y - b.rect.h * 0.5f;
    
    return (a_left < b_right && a_right > b_left && 
            a_bottom < b_top && a_top > b_bottom);
}

void collision(game* g) {
    entity* player = &scene.entities[0];
    assert(player->type == PLAYER);
    
    player->collider.rect.x = player->pos.x;
    player->collider.rect.y = player->pos.y;
    
    vec2 player_center = {player->collider.rect.x, player->collider.rect.y};
    debug_draw_rect(&g->debug_renderer, player_center, player->collider.rect.w, player->collider.rect.h, DEBUG_BLUE);
    
    float cross_size = 8.0f;
    vec2 center_h1 = {player->pos.x - cross_size, player->pos.y};
    vec2 center_h2 = {player->pos.x + cross_size, player->pos.y};
    vec2 center_v1 = {player->pos.x, player->pos.y - cross_size};
    vec2 center_v2 = {player->pos.x, player->pos.y + cross_size};
    
    debug_draw_line(&g->debug_renderer, center_h1, center_h2, DEBUG_RED);
    debug_draw_line(&g->debug_renderer, center_v1, center_v2, DEBUG_RED);
    
    for (int i = 1; i < scene.entity_count; i++) {
        entity* e = &scene.entities[i];
        
        e->collider.rect.x = e->pos.x;
        e->collider.rect.y = e->pos.y;
        
        debug_color color = DEBUG_GREEN;
        if (bbox_collide(player->collider, e->collider)) {
            color = DEBUG_RED;
            player->health -= 5.0f * g->dt;
            if (player->health < 0.0f) {
                player->health = 0.0f;
            }
        }
        
        vec2 center = {e->collider.rect.x, e->collider.rect.y};
        debug_draw_rect(&g->debug_renderer, center, e->collider.rect.w, e->collider.rect.h, color);
        
        vec2 enemy_center_h1 = {e->pos.x - cross_size, e->pos.y};
        vec2 enemy_center_h2 = {e->pos.x + cross_size, e->pos.y};
        vec2 enemy_center_v1 = {e->pos.x, e->pos.y - cross_size};
        vec2 enemy_center_v2 = {e->pos.x, e->pos.y + cross_size};
        
        debug_draw_line(&g->debug_renderer, enemy_center_h1, enemy_center_h2, DEBUG_RED);
        debug_draw_line(&g->debug_renderer, enemy_center_v1, enemy_center_v2, DEBUG_RED);
    }
}