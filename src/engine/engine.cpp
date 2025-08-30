#include "engine.h"
#include <externals.h>
#include <game.h>
#include <scene.h>
#include <stdio.h>
#include "renderer.h"
#define IMGUI_USER_CONFIG "myimguiconfig.h"
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <physics.h>
#include <glad.h>

void update_input(game* g) {
    if (!g || scene.entity_count == 0) return;
    
    entity* player = &scene.entities[0];
    const float move_speed = 200.0f;
    
    player->velocity.x = g->input.horizontal * move_speed;
    player->velocity.y = g->input.vertical * move_speed;

    bool attack_pressed = (g->input.input_mask & INPUT_A) ;
    if(attack_pressed) {
        debug_draw_line(&g->debug_renderer, {0,0},{10,20}, DEBUG_GREEN);
        debug_draw_line(&g->debug_renderer, {10,10},{100,200}, DEBUG_BLUE);
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

EXPORT void init_engine(game *g) {
    printf("hi from init engine\n");
}

EXPORT void update_engine(game *g) {
    if (!g || !g->ctx) return;
    g->debug_renderer.current_line_count = 0;

    update_input(g);
    update_animation(g);
    render_tiles(g);
    collision(g);
    apply_movement(g);
    update_camera_matrix(&g->camera, g->view_matrix);
    render_entities(g);

    glUseProgram(g->debug_renderer.debug_shader);
    if (g->debug_renderer.debug_projection_loc != -1) {
        glUniformMatrix4fv(g->debug_renderer.debug_projection_loc, 1, GL_FALSE, g->ortho_projection);
    }
    if (g->debug_renderer.debug_view_loc != -1) {
        glUniformMatrix4fv(g->debug_renderer.debug_view_loc, 1, GL_FALSE, g->view_matrix);
    }
    

    if (g->debug_renderer.current_line_count > 0) {
        glBindVertexArray(g->debug_renderer.line_VAO);
        glBindBuffer(GL_ARRAY_BUFFER, g->debug_renderer.line_VBO);
        glBufferSubData(GL_ARRAY_BUFFER, 0, g->debug_renderer.current_line_count * 10 * sizeof(float), g->debug_renderer.vertex_buffer);
        glDrawArrays(GL_LINES, 0, g->debug_renderer.current_line_count * 2);
    }
    
    glBindVertexArray(0);
    glUseProgram(0);

    ImGui::SetCurrentContext(g->ctx);
    ImGui::SetAllocatorFunctions(g->alloc_func, g->free_func, g->user_data);

    ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_FirstUseEver);
}

EXPORT void destroy_engine(game *g) {
    printf("hi from destroy_engine \n");
}
