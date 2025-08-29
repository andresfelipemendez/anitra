#include "physics.h"
#include "game.h"
#include "debug_render.h"
#include <scene.h>

void collision(game* g) {
    // Draw bounding boxes for all entities using 4x pixel coordinates
    for (int i = 0; i < scene.entity_count; i++) {
        entity* e = &scene.entities[i];
        vec2 pos = e->pos;
        
        // Use 4x scaled sprite size (16x16 becomes 64x64) for colliders
        float half_size = 32.0f; // Half of 64 pixels (16*4)
        
        // Draw bounding box corners
        vec2 top_left = {pos.x - half_size, pos.y + half_size};
        vec2 top_right = {pos.x + half_size, pos.y + half_size};
        vec2 bottom_right = {pos.x + half_size, pos.y - half_size};
        vec2 bottom_left = {pos.x - half_size, pos.y - half_size};
        
        // Draw the bounding box
        debug_draw_line(&g->debug_renderer, top_left, top_right, DEBUG_GREEN);
        debug_draw_line(&g->debug_renderer, top_right, bottom_right, DEBUG_GREEN);
        debug_draw_line(&g->debug_renderer, bottom_right, bottom_left, DEBUG_GREEN);
        debug_draw_line(&g->debug_renderer, bottom_left, top_left, DEBUG_GREEN);
        
        // Draw center cross for reference (4x scaled)
        vec2 center_h1 = {pos.x - 8.0f, pos.y};
        vec2 center_h2 = {pos.x + 8.0f, pos.y};
        vec2 center_v1 = {pos.x, pos.y - 8.0f};
        vec2 center_v2 = {pos.x, pos.y + 8.0f};
        
        debug_draw_line(&g->debug_renderer, center_h1, center_h2, DEBUG_RED);
        debug_draw_line(&g->debug_renderer, center_v1, center_v2, DEBUG_RED);
    }
}