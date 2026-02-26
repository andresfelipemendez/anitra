#include <game.h>
#include <export.h>
#include <scene.h>
#include <stdio.h>
#include "renderer.h"
#include <physics.h>
#include <math.h>

void update_input(game* g) {
    entity* player;
    const float move_speed = 200.0f;
    int attack_pressed;
    const float camera_speed = 300.0f;
    const float zoom_speed = 2.0f;
    if (!g || scene.entity_count == 0) return;

    player = &scene.entities[0];

    player->velocity.x = g->input.horizontal * move_speed;
    player->velocity.y = g->input.vertical * move_speed;

    attack_pressed = (g->input.input_mask & INPUT_A);
    if (attack_pressed) {
        vec2 a1 = {0,0}, b1 = {10,20};
        vec2 a2 = {10,10}, b2 = {100,200};
        debug_draw_line(&g->debug_renderer, a1, b1, DEBUG_GREEN);
        debug_draw_line(&g->debug_renderer, a2, b2, DEBUG_BLUE);
    }

    if (g->input.input_mask & INPUT_X) {
        g->camera.position.x += g->input.horizontal * camera_speed * g->dt;
        g->camera.position.y += g->input.vertical * camera_speed * g->dt;
    }
    if (g->input.input_mask & INPUT_Y) {
        float zoom_delta = g->input.vertical * zoom_speed * g->dt;
        g->camera.zoom = fmaxf(0.1f, fminf(5.0f, g->camera.zoom + zoom_delta));
    }
}

EXPORT void init_engine(game *g) {
    /* Allocate entity storage from the gameplay sub-arena (only on first init, survives hot reload) */
    if (!scene.entities) {
        scene.entity_capacity = 64;
        scene.entities = (entity *)arena_alloc(g->gameplay,
            (uint32_t)(scene.entity_capacity * sizeof(entity)), 16, "entities");
    }
    scene_init();
    printf("hi from init engine\n");
}

EXPORT void update_engine(game *g) {
    int i;
    if (!g) return;

    g->draw_list.sprite_count = 0;
    g->draw_list.line_count = 0;
    g->debug_renderer.current_line_count = 0;

    update_input(g);
    update_animation(g);
    render_tiles(g);
    collision(g);
    apply_movement(g);
    update_camera_matrix(&g->camera, g->draw_list.view_matrix);
    render_entities(g);

    /* Copy debug lines from debug_renderer to draw_list */
    for (i = 0; i < g->debug_renderer.current_line_count; i++) {
        int idx;
        debug_line_command* line;
        if (g->draw_list.line_count >= g->draw_list.line_capacity) break;
        idx = i * 10;
        line = &g->draw_list.lines[g->draw_list.line_count++];
        line->x1 = g->debug_renderer.vertex_buffer[idx];
        line->y1 = g->debug_renderer.vertex_buffer[idx+1];
        line->r = g->debug_renderer.vertex_buffer[idx+2];
        line->g = g->debug_renderer.vertex_buffer[idx+3];
        line->b = g->debug_renderer.vertex_buffer[idx+4];
        line->x2 = g->debug_renderer.vertex_buffer[idx+5];
        line->y2 = g->debug_renderer.vertex_buffer[idx+6];
    }
}

EXPORT void destroy_engine(game *g) {
    printf("hi from destroy_engine \n");
}
