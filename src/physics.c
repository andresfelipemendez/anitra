#include "game.h"
#include "physics.h"
#include <math.h>

/* Simple AABB collision detection */
int check_aabb_collision(rect a, rect b) {
    return !(a.x + a.w <= b.x ||
             a.x >= b.x + b.w ||
             a.y + a.h <= b.y ||
             a.y >= b.y + b.h);
}

void collision(memory *g) {
    if (!g || !g->gameplay) return;
    
    /* Check entity-to-entity collisions */
    for (int i = 0; i < scene.entity_count; i++) {
        entity *a = &scene.entities[i];
        
        for (int j = i + 1; j < scene.entity_count; j++) {
            entity *b = &scene.entities[j];
            
            if (check_aabb_collision(a->collider.rect, b->collider.rect)) {
                /* Simple collision response: push apart */
                float overlap_x = fminf(a->collider.rect.x + a->collider.rect.w,
                                        b->collider.rect.x + b->collider.rect.w) -
                                  fmaxf(a->collider.rect.x, b->collider.rect.x);
                
                float overlap_y = fminf(a->collider.rect.y + a->collider.rect.h,
                                        b->collider.rect.y + b->collider.rect.h) -
                                  fmaxf(a->collider.rect.y, b->collider.rect.y);
                
                if (fabsf(overlap_x) < fabsf(overlap_y)) {
                    if (a->collider.rect.x < b->collider.rect.x) {
                        a->pos.x -= overlap_x * 0.5f;
                        b->pos.x += overlap_x * 0.5f;
                    } else {
                        a->pos.x += overlap_x * 0.5f;
                        b->pos.x -= overlap_x * 0.5f;
                    }
                } else {
                    if (a->collider.rect.y < b->collider.rect.y) {
                        a->pos.y -= overlap_y * 0.5f;
                        b->pos.y += overlap_y * 0.5f;
                    } else {
                        a->pos.y += overlap_y * 0.5f;
                        b->pos.y -= overlap_y * 0.5f;
                    }
                }
                
                /* Draw collision debug */
                vec2 center_a = {a->pos.x + a->collider.rect.w/2, 
                               a->pos.y + a->collider.rect.h/2};
                vec2 center_b = {b->pos.x + b->collider.rect.w/2,
                               b->pos.y + b->collider.rect.h/2};
                
                debug_draw_line(&g->debug_renderer, center_a, center_b, DEBUG_RED);
            }
        }
    }
}

void apply_movement(memory *g) {
    if (!g || !g->gameplay) return;
    
    const float gravity = -500.0f;
    const float ground_y = 100.0f;
    
    for (int i = 0; i < scene.entity_count; i++) {
        entity *e = &scene.entities[i];
        
        /* Apply velocity */
        e->pos.x += e->velocity.x * g->dt;
        e->pos.y += e->velocity.y * g->dt;
        
        /* Simple gravity */
        if (e->pos.y > ground_y) {
            e->velocity.y += gravity * g->dt;
        } else {
            e->pos.y = ground_y;
            e->velocity.y = 0.0f;
        }
    }
}
