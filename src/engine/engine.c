#include "engine.h"
#include <externals.h>
#include <game.h>
#include <scene.h>
#include <stdio.h>
#include "renderer.h"
#include <physics.h>
#include <SDL3/SDL.h>
#include <math.h>

static void world_to_screen_engine(struct game* g, float wx, float wy, float* sx, float* sy) {
    float cx = g->camera.position.x;
    float cy = g->camera.position.y;
    float zoom = g->camera.zoom;
    *sx = (wx - cx) * zoom + g->width * 0.5f;
    *sy = g->height * 0.5f - (wy - cy) * zoom;
}

static void flush_debug_lines(struct game* g) {
    debug_renderer* dr = &g->debug_renderer;
    if (!dr || dr->current_line_count == 0) return;

    for (int i = 0; i < dr->current_line_count; i++) {
        int idx = i * 10;
        float x1 = dr->vertex_buffer[idx + 0];
        float y1 = dr->vertex_buffer[idx + 1];
        float r  = dr->vertex_buffer[idx + 2];
        float gr = dr->vertex_buffer[idx + 3];
        float b  = dr->vertex_buffer[idx + 4];
        float x2 = dr->vertex_buffer[idx + 5];
        float y2 = dr->vertex_buffer[idx + 6];

        float sx1, sy1, sx2, sy2;
        world_to_screen_engine(g, x1, y1, &sx1, &sy1);
        world_to_screen_engine(g, x2, y2, &sx2, &sy2);

        SDL_SetRenderDrawColor(g->renderer,
            (Uint8)(r * 255), (Uint8)(gr * 255), (Uint8)(b * 255), 255);
        SDL_RenderLine(g->renderer, sx1, sy1, sx2, sy2);
    }
}

void update_input(struct game* g) {
    if (!g || scene.entity_count == 0) return;

    entity* player = &scene.entities[0];
    const float move_speed = 200.0f;

    player->velocity.x = g->input.horizontal * move_speed;
    player->velocity.y = g->input.vertical * move_speed;

    int attack_pressed = (g->input.input_mask & INPUT_A);
    if (attack_pressed) {
        vec2 a0 = {0, 0}, a1 = {10, 20};
        vec2 b0 = {10, 10}, b1 = {100, 200};
        debug_draw_line(&g->debug_renderer, a0, a1, DEBUG_GREEN);
        debug_draw_line(&g->debug_renderer, b0, b1, DEBUG_BLUE);
    }

    const float camera_speed = 300.0f;
    const float zoom_speed = 2.0f;

    if (g->input.input_mask & INPUT_X) {
        g->camera.position.x += g->input.horizontal * camera_speed * g->dt;
        g->camera.position.y += g->input.vertical * camera_speed * g->dt;
    }
    if (g->input.input_mask & INPUT_Y) {
        float zoom_delta = g->input.vertical * zoom_speed * g->dt;
        g->camera.zoom = fmaxf(0.1f, fminf(5.0f, g->camera.zoom + zoom_delta));
    }
}

EXPORT void init_engine(struct game* g) {
    printf("hi from init engine\n");
}

EXPORT void update_engine(struct game* g) {
    if (!g) return;
    g->debug_renderer.current_line_count = 0;

    update_input(g);
    update_animation(g);
    render_tiles(g);
    collision(g);
    apply_movement(g);
    update_camera_matrix(&g->camera, g->view_matrix);
    render_entities(g);
    flush_debug_lines(g);
}

EXPORT void destroy_engine(struct game* g) {
    printf("hi from destroy_engine\n");
}
