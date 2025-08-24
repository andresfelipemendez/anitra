#include "engine.h"
#include <externals.h>
#include <game.h>
#include <scene.h>
#include <stdio.h>

#define IMGUI_USER_CONFIG "myimguiconfig.h"
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <filesystem>
#include <iostream>

#include <glad.h>
namespace fs = std::filesystem;

rect pixel_to_uv(pixel_rect p) {
    rect uv;
    uv.x = (float)p.x / (float)spritesheet_info.width;
    uv.y = (float)p.y / (float)spritesheet_info.height;  
    uv.w = (float)p.w / (float)spritesheet_info.width;
    uv.h = (float)p.h / (float)spritesheet_info.height;
    return uv;
}

void render_sprite(game* g, GLuint texture, float x, float y) {
    if (!g || g->sprite_shader == 0 || texture == 0) {
        
        return;
    }
    
    glUseProgram(g->sprite_shader);
    
    if (g->translation_loc != -1) {
        glUniform2f(g->translation_loc, x, y);
    }
    
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    
    if (g->texture_loc != -1) {
        glUniform1i(g->texture_loc, 0);
    }
    
    if (g->tint_loc != -1) {
        glUniform4f(g->tint_loc, 1.0f, 1.0f, 1.0f, 1.0f);
    }
    
    if (g->quad_VAO != 0) {
        glBindVertexArray(g->quad_VAO);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    }
}

void render_sprite_region(game* g, GLuint texture, float x, float y, rect region) {
    if (!g || g->sprite_shader == 0 || texture == 0) {
        
        return;
    }
    
    glUseProgram(g->sprite_shader);
    
    if (g->translation_loc != -1) {
        glUniform2f(g->translation_loc, x, y);
    }
    
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    
    if (g->texture_loc != -1) {
        glUniform1i(g->texture_loc, 0);
    }
    
    if (g->tint_loc != -1) {
        glUniform4f(g->tint_loc, 1.0f, 1.0f, 1.0f, 1.0f);
    }
    
    if (g->sprite_offset_loc != -1) {
        glUniform2f(g->sprite_offset_loc, region.x, region.y);
    }
    
    if (g->sprite_size_loc != -1) {
        glUniform2f(g->sprite_size_loc, region.w, region.h);
    }
    
    if (g->quad_VAO != 0) {
        glBindVertexArray(g->quad_VAO);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    }
}

void update_animation(game* g) {
    if (!g) {
        return;
    }

    for (int i = 0; i < scene.entity_count; i++) {
        entity* e = &scene.entities[i];
        animator* a = &e->current_animation;

    //     if (!a->playing || a->animation.frame_count <= 1) {
    //         continue;
    //     }

        e->current_animation.timer += g->dt;

        while (a->timer >= a->animation.frame_time) {
            a->timer -= a->animation.frame_time;
            a->frame_index = (a->frame_index + 1) % a->animation.frame_count;

            int current_frame_id = a->animation.frames[a->frame_index];
            // if (current_frame_id >= 0 && current_frame_id < sizeof(char_sprites) / sizeof(char_sprites[0])) {
            //     e->spr.coords = pixel_to_uv(char_sprites[current_frame_id]);
            // }
        }
    }
}

void render_entities(game* g) {
    if (!g) return;
        
    if (g->sprite_shader != 0) {
        glUseProgram(g->sprite_shader);
        
        if (g->projection_loc != -1) {
            glUniformMatrix4fv(g->projection_loc, 1, GL_FALSE, g->ortho_projection);
        }
        
        // Render each entity using sprite regions
        for (int i = 0; i < scene.entity_count; i++) {
            const entity* e = &scene.entities[i];
            float x = e->pos.x;
            float y = e->pos.y;
            int sprite_id = e->current_animation.frame_index;
            
            if (g->textures.char_spritesheet != 0) {
                if (sprite_id >= 0 && sprite_id < 5) {
                    pixel_rect pixel_region = char_sprites[sprite_id];
                    rect uv_region = pixel_to_uv(pixel_region);
                   
                    render_sprite_region(g, g->textures.char_spritesheet, x, y, uv_region);
                } else {
                    printf("Invalid sprite_id: %d for entity %d\n", sprite_id, i);
                    // Fallback to full texture rendering
                    render_sprite(g, g->textures.char_spritesheet, x, y);
                }
            }
        }
        
    } else {
        printf("No sprite shader available\n");
    }
}

EXPORT void hotreloadable_imgui_draw(game *g) {
    if (!g || !g->ctx) return;
    
    update_animation(g);
    render_entities(g);

    
    // ImGui setup
    ImGui::SetCurrentContext(g->ctx);
    ImGui::SetAllocatorFunctions(g->alloc_func, g->free_func, g->user_data);

    // Debug window with texture display
    ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_FirstUseEver);
   
    
    // ImGui::End();
}