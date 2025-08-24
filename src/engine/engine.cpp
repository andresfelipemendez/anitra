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
    
    // Set position
    if (g->translation_loc != -1) {
        glUniform2f(g->translation_loc, x, y);
    }
    
    // Bind texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    
    if (g->texture_loc != -1) {
        glUniform1i(g->texture_loc, 0);
    }
    
    // Set tint
    if (g->tint_loc != -1) {
        glUniform4f(g->tint_loc, 1.0f, 1.0f, 1.0f, 1.0f);
    }
    
    // Pass sprite region to shader
    if (g->sprite_offset_loc != -1) {
        glUniform2f(g->sprite_offset_loc, region.x, region.y);
    }
    
    if (g->sprite_size_loc != -1) {
        glUniform2f(g->sprite_size_loc, region.w, region.h);
    }
    
    // Render the quad
    if (g->quad_VAO != 0) {
        glBindVertexArray(g->quad_VAO);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
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
        for (int i = 0; i < scene_config.entity_count; i++) {
            float x = scene_config.entity_data[i].x;
            float y = scene_config.entity_data[i].y;
            int sprite_id = scene_config.entity_data[i].sprite_id;
            
            if (g->textures.char_spritesheet != 0) {
                // Get the sprite region from scene data
                if (sprite_id >= 0 && sprite_id < 4) {  // Bounds check
                    pixel_rect s = char_sprites[sprite_id];
                    rect sprite_region = {
                        .x = (float)s.x,
                        .y = (float)s.y,
                        .w = (float)s.w,
                        .h = (float)s.h,
                    };
                    // Use the new sprite region rendering function
                    render_sprite_region(g, g->textures.char_spritesheet, x, y, sprite_region);
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
    
    render_entities(g);
    
    // ImGui setup
    ImGui::SetCurrentContext(g->ctx);
    ImGui::SetAllocatorFunctions(g->alloc_func, g->free_func, g->user_data);

    // Debug window with texture display
    ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_FirstUseEver);
   
    
    // ImGui::End();
}