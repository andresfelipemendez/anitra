#include "renderer.h"
#include <glad.h>
#include <scene.h>
#include <string>

#if defined ( __clang__ ) || defined ( __GNUC__ )
#define TracyFunction __PRETTY_FUNCTION__
#elif defined ( _MSC_VER )
#define TracyFunction __FUNCSIG__
#endif
#include <tracy/Tracy.hpp>

rect pixel_to_uv(pixel_rect p, sprite_sheet& s) {
    rect uv;
    uv.x = (float)p.x / (float)s.width;
    uv.y = (float)p.y / (float)s.height;  
    uv.w = (float)p.w / (float)s.width;
    uv.h = (float)p.h / (float)s.height;
    return uv;
}

void update_camera_matrix(camera* cam, float* matrix) {
    for (int i = 0; i < 16; i++) matrix[i] = 0.0f;

    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;

    matrix[0] = cam->zoom;
    matrix[5] = cam->zoom;

    matrix[12] = -cam->position.x * cam->zoom;
    matrix[13] = -cam->position.y * cam->zoom;
}

void render_sprite(game* g, GLuint texture, float x, float y) {
    if (!g || g->sprite_shader == 0 || texture == 0) {
        return;
    }
    
    glUseProgram(g->sprite_shader);
    
    if (g->translation_loc != -1) {
        glUniform2f(g->translation_loc, x, y);
    }
    
    if (g->scale_loc != -1) {
        glUniform2f(g->scale_loc, 64.0f, 64.0f); // Default scale for backward compatibility
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
    glUniform2f(g->translation_loc, x, y);
    
    if (g->scale_loc != -1) {
        glUniform2f(g->scale_loc, 64.0f, 64.0f); // Default scale for backward compatibility
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
        e->current_animation.timer += g->dt;

        while (a->timer >= a->animation.frame_time) {
            a->timer -= a->animation.frame_time;
            a->frame_index = (a->frame_index + 1) % a->animation.frame_count;

            int current_frame_id = a->animation.frames[a->frame_index];
        }
    }
}

void render_tile_optimized(game* g, int tile, float x, float y) {
    ZoneScoped;
    if (!g || g->textures[TEXTURE_TILES] == 0 || g->tile_quad_VAO == 0) {
        return;
    }
    
    pixel_rect pixel_region;
    {
        ZoneNamedN(tile_lookup_zone, "Tile Sprite Lookup", true);
        pixel_region = tiles.sprites[tile];
    }
    
    float u1, v1, u2, v2;
    {
        ZoneNamedN(tile_uv_calc_zone, "Tile UV Calculation", true);
        u1 = (float)pixel_region.x / (float)tiles.width;
        v1 = (float)pixel_region.y / (float)tiles.height;
        u2 = (float)(pixel_region.x + pixel_region.w) / (float)tiles.width;
        v2 = (float)(pixel_region.y + pixel_region.h) / (float)tiles.height;
    }
    
    {
        ZoneNamedN(tile_render_zone, "Tile Optimized Render", true);
        // Use shared tile buffer - NO buffer creation/deletion!
        glBindVertexArray(g->tile_quad_VAO);
        
        glUseProgram(g->sprite_shader);
        glUniformMatrix4fv(g->view_loc, 1, GL_FALSE, g->view_matrix);
        glUniformMatrix4fv(g->projection_loc, 1, GL_FALSE, g->ortho_projection);
        glUniform2f(g->translation_loc, x, y);
        glUniform2f(g->scale_loc, 64.0f, 64.0f); // Scale for 64x64 tiles (16x16 * 4.0f)
        
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g->textures[TEXTURE_TILES]);
        glUniform1i(g->texture_loc, 0);
        
        // Set UV coordinates for this specific tile
        glUniform2f(g->sprite_offset_loc, u1, v1);
        glUniform2f(g->sprite_size_loc, u2 - u1, v2 - v1);
        
        if (g->tint_loc != -1) {
            glUniform4f(g->tint_loc, 1.0f, 1.0f, 1.0f, 1.0f);
        }
        
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    }
}

void render_tile(game* g, int tile, float x, float y) {
    ZoneScoped;
    if (!g || g->textures[TEXTURE_TILES] == 0) {
        return;
    }
    
    pixel_rect pixel_region;
    {
        ZoneNamedN(tile_lookup_zone, "Tile Sprite Lookup", true);
        pixel_region = tiles.sprites[tile];
    }
    
    {
        ZoneNamedN(tile_pixel_perfect_zone, "Tile Pixel Perfect Render", true);
        render_sprite_pixel_perfect(g, g->textures[TEXTURE_TILES], x, y, 
                                   pixel_region, tiles.width, tiles.height);
    }
}

void render_tiles(game* g) {
    ZoneScoped;
    
    {
        ZoneNamedN(tile_setup_zone, "Tile Setup", true);
        int rows = sizeof(level) / sizeof(level[0]);     // Number of rows (y)
        int cols = sizeof(level[0]) / sizeof(level[0][0]); // Number of columns (x)
        TracyMessageL("Rendering tiles");
        TracyMessage(("Tile grid: " + std::to_string(cols) + "x" + std::to_string(rows)).c_str(), strlen(("Tile grid: " + std::to_string(cols) + "x" + std::to_string(rows)).c_str()));
    }

    int rows = sizeof(level) / sizeof(level[0]);     
    int cols = sizeof(level[0]) / sizeof(level[0][0]); 

    {
        ZoneNamedN(tile_loop_zone, "Tile Rendering Loop", true);
        for (int y = 0; y < rows; y++) {
            ZoneNamedN(tile_row_zone, "Tile Row", true);
            for (int x = 0; x < cols; x++) {
                ZoneNamedN(tile_individual_zone, "Individual Tile", true);
                int tile = level[rows - 1 - y][cols - 1 - x];
                render_tile_optimized(g, tile, x * 64, y * 64);
            }
        }
    }
}

void render_sprite_pixel_perfect_optimized(game* g, GLuint texture, float x, float y, pixel_rect sprite_rect, int texture_width, int texture_height) {
    ZoneScoped;
    if (!g || g->sprite_shader == 0 || texture == 0 || g->sprite_quad_VAO == 0) {
        return;
    }
    
    float quad_width, quad_height;
    float u1, v1, u2, v2;
    
    {
        ZoneNamedN(sprite_calc_zone, "Sprite Calculation", true);
        quad_width = sprite_rect.w * 4.0f;
        quad_height = sprite_rect.h * 4.0f;
        
        u1 = (float)sprite_rect.x / (float)texture_width;
        v1 = (float)sprite_rect.y / (float)texture_height;
        u2 = (float)(sprite_rect.x + sprite_rect.w) / (float)texture_width;
        v2 = (float)(sprite_rect.y + sprite_rect.h) / (float)texture_height;
    }
    
    {
        ZoneNamedN(sprite_render_zone, "Sprite Optimized Render", true);
        TracyMessageL("Using shared sprite buffer - NO buffer creation!");
        
        // Use shared sprite buffer - NO buffer creation/deletion!
        glBindVertexArray(g->sprite_quad_VAO);
        
        glUseProgram(g->sprite_shader);
        glUniformMatrix4fv(g->view_loc, 1, GL_FALSE, g->view_matrix);
        glUniformMatrix4fv(g->projection_loc, 1, GL_FALSE, g->ortho_projection);
        glUniform2f(g->translation_loc, x, y);
        glUniform2f(g->scale_loc, quad_width, quad_height); // Scale the unit quad to sprite size
        
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        glUniform1i(g->texture_loc, 0);
        
        // Set UV coordinates for this specific sprite
        glUniform2f(g->sprite_offset_loc, u1, v1);
        glUniform2f(g->sprite_size_loc, u2 - u1, v2 - v1);
        
        if (g->tint_loc != -1) {
            glUniform4f(g->tint_loc, 1.0f, 1.0f, 1.0f, 1.0f);
        }
        
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    }
}

void render_sprite_pixel_perfect(game* g, GLuint texture, float x, float y, pixel_rect sprite_rect, int texture_width, int texture_height) {
    ZoneScoped;
    if (!g || g->sprite_shader == 0 || texture == 0) {
        return;
    }
    
    float quad_width, quad_height, hw, hh;
    float u1, v1, u2, v2;
    
    {
        ZoneNamedN(vertex_calc_zone, "Vertex Calculation", true);
        quad_width = sprite_rect.w * 4.0f;
        quad_height = sprite_rect.h * 4.0f;
        
        hw = quad_width * 0.5f;   
        hh = quad_height * 0.5f;
        
        u1 = (float)sprite_rect.x / (float)texture_width;
        v1 = (float)sprite_rect.y / (float)texture_height;
        u2 = (float)(sprite_rect.x + sprite_rect.w) / (float)texture_width;
        v2 = (float)(sprite_rect.y + sprite_rect.h) / (float)texture_height;
    }
    
    float vertices[] = {
        -hw,  hh,  u1, v1,  // top left
         hw,  hh,  u2, v1,  // top right
         hw, -hh,  u2, v2,  // bottom right
        -hw, -hh,  u1, v2   // bottom left
    };
    
    unsigned int indices[] = {
        0, 1, 2,  
        2, 3, 0   
    };
    
    GLuint VAO, VBO, EBO;
    {
        ZoneNamedN(buffer_creation_zone, "Buffer Creation", true);
        TracyMessageL("Creating temporary VAO/VBO/EBO - PERFORMANCE BOTTLENECK");
        glGenVertexArrays(1, &VAO);
        glGenBuffers(1, &VBO);
        glGenBuffers(1, &EBO);
    }
    
    {
        ZoneNamedN(buffer_setup_zone, "Buffer Setup", true);
        glBindVertexArray(VAO);
        glBindBuffer(GL_ARRAY_BUFFER, VBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
        
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
        glEnableVertexAttribArray(1);
    }
    
    {
        ZoneNamedN(shader_uniforms_zone, "Shader Uniforms", true);
        glUseProgram(g->sprite_shader);
        glUniformMatrix4fv(g->view_loc, 1, GL_FALSE, g->view_matrix);
        glUniformMatrix4fv(g->projection_loc, 1, GL_FALSE, g->ortho_projection);
        glUniform2f(g->translation_loc, x, y);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        glUniform1i(g->texture_loc, 0);
        
        glUniform2f(g->sprite_offset_loc, 0.0f, 0.0f);
        glUniform2f(g->sprite_size_loc, 1.0f, 1.0f);
        
        if (g->tint_loc != -1) {
            glUniform4f(g->tint_loc, 1.0f, 1.0f, 1.0f, 1.0f);
        }
    }
    
    {
        ZoneNamedN(draw_call_zone, "Draw Call", true);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    }
    
    {
        ZoneNamedN(buffer_cleanup_zone, "Buffer Cleanup", true);
        TracyMessageL("Deleting temporary VAO/VBO/EBO - PERFORMANCE BOTTLENECK");
        glDeleteVertexArrays(1, &VAO);
        glDeleteBuffers(1, &VBO);
        glDeleteBuffers(1, &EBO);
    }
}

void render_scaled_sprite_optimized(game* g, GLuint texture, float x, float y, float width, float height) {
    ZoneScoped;
    if (!g || g->sprite_shader == 0 || texture == 0 || g->sprite_quad_VAO == 0) {
        return;
    }
    
    {
        ZoneNamedN(scaled_sprite_render_zone, "Scaled Sprite Optimized Render", true);
        TracyMessageL("Using shared sprite buffer for scaled sprite - NO buffer creation!");
        
        // Use shared sprite buffer - NO buffer creation/deletion!
        glBindVertexArray(g->sprite_quad_VAO);
        
        glUseProgram(g->sprite_shader);
        glUniform2f(g->translation_loc, x, y);
        glUniform2f(g->scale_loc, width, height); // Scale the unit quad to desired size
        
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        glUniform1i(g->texture_loc, 0);
        
        // Use full texture (0,0) to (1,1)
        glUniform2f(g->sprite_offset_loc, 0.0f, 0.0f);
        glUniform2f(g->sprite_size_loc, 1.0f, 1.0f);
        
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    }
}

void render_scaled_sprite(game* g, GLuint texture, float x, float y, float width, float height) {
    if (!g || g->sprite_shader == 0 || texture == 0) {
        return;
    }
    
    // Create vertices for custom size
    float hw = width * 0.5f;   // half width
    float hh = height * 0.5f;  // half height
    
    float vertices[] = {
        -hw,  hh,  0.0f, 0.0f,  // top left
         hw,  hh,  1.0f, 0.0f,  // top right
         hw, -hh,  1.0f, 1.0f,  // bottom right
        -hw, -hh,  0.0f, 1.0f   // bottom left
    };
    
    unsigned int indices[] = {
        0, 1, 2,  
        2, 3, 0   
    };
    
    // Create temporary VAO and buffers for custom size
    GLuint VAO, VBO, EBO;
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
    glGenBuffers(1, &EBO);
    
    glBindVertexArray(VAO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    
    glUseProgram(g->sprite_shader);
    glUniform2f(g->translation_loc, x, y);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform1i(g->texture_loc, 0);
    glUniform2f(g->sprite_offset_loc, 0.0f, 0.0f);
    glUniform2f(g->sprite_size_loc, 1.0f, 1.0f);
    
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);
    glDeleteBuffers(1, &EBO);
}

void render_health_bar(game* g, float x, float y, float health, float max_health) {
    if (!g || max_health <= 0.0f || g->textures[TEXTURE_HEALTH_BAR] == 0 || g->textures[TEXTURE_HEALTH_FILL] == 0) {
        return;
    }
    
    float health_ratio = health / max_health;
    if (health_ratio > 1.0f) health_ratio = 1.0f;
    if (health_ratio < 0.0f) health_ratio = 0.0f;
    
    pixel_rect bar_frame_rect = {0, 0, 64, 16};  
    pixel_rect bar_fill_rect = {0, 0, (int)(64 * health_ratio), 16};  
    
    float bar_x = x;
    float bar_y = y + 64.0f; 
    
    glUseProgram(g->sprite_shader);
    glUniformMatrix4fv(g->view_loc, 1, GL_FALSE, g->view_matrix);
    glUniformMatrix4fv(g->projection_loc, 1, GL_FALSE, g->ortho_projection);
    
    if (health_ratio > 0.0f) {
        float fill_min = 34.0f;  // 17px * 2
        float fill_max = 112.0f; // 56px * 2
        float fill_range = fill_max - fill_min; // 78px
        float fill_width = fill_min + (fill_range * health_ratio);
        float fill_offset = -(128.0f - fill_width) * 0.5f;
        render_scaled_sprite_optimized(g, g->textures[TEXTURE_HEALTH_FILL], 
                           bar_x + fill_offset, bar_y, fill_width, 32.0f);
    }
    
    glUniform4f(g->tint_loc, 1.0f, 1.0f, 1.0f, 1.0f);
    render_scaled_sprite_optimized(g, g->textures[TEXTURE_HEALTH_BAR], 
                       bar_x, bar_y, 128.0f, 32.0f);
    
    glUniform4f(g->tint_loc, 1.0f, 1.0f, 1.0f, 1.0f);
}

void render_entities(game* g) {
    ZoneScoped;
    if (!g) return;
        
    if (g->sprite_shader != 0) {
        glUseProgram(g->sprite_shader);
        
        glUniformMatrix4fv(g->view_loc, 1, GL_FALSE, g->view_matrix);
        glUniformMatrix4fv(g->projection_loc, 1, GL_FALSE, g->ortho_projection);
        
        for (int i = 0; i < scene.entity_count; i++) {
            ZoneNamedN(entity_render_zone, "Entity Rendering", true);
            entity* e = &scene.entities[i];
            float x = e->pos.x;
            float y = e->pos.y;
            int sprite_id = e->current_animation.frame_index;
            
            {
                ZoneNamedN(sprite_render_zone, "Sprite Rendering", true);
                if (g->textures[e->sprite_sheet.texture_id] != 0) {
                    if (sprite_id >= 0 && sprite_id < 6) { // Updated to 6 for slime sprites
                        pixel_rect pixel_region = e->sprite_sheet.sprites[sprite_id];
                        render_sprite_pixel_perfect_optimized(g, g->textures[e->sprite_sheet.texture_id], x, y, 
                                                   pixel_region, e->sprite_sheet.width, e->sprite_sheet.height);
                    } else {
                        pixel_rect pixel_region = e->sprite_sheet.sprites[0];
                        render_sprite_pixel_perfect_optimized(g, g->textures[e->sprite_sheet.texture_id], x, y, 
                                                   pixel_region, e->sprite_sheet.width, e->sprite_sheet.height);
                    }
                }
            }
            
            {
                ZoneNamedN(health_render_zone, "Health Bar Rendering", true);
                render_health_bar(g, x, y, e->health, 100.0f); // assuming max health is 100
            }
        }
        
    } else {
    }
}