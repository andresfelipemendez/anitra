#include "renderer.h"
#include <glad.h>
#include <scene.h>

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

void render_tile(game* g, int tile, float x, float y) {
    if (!g || g->textures[TEXTURE_TILES] == 0) {
        return;
    }
    
    pixel_rect pixel_region = tiles.sprites[tile];
    render_sprite_pixel_perfect(g, g->textures[TEXTURE_TILES], x, y, 
                               pixel_region, tiles.width, tiles.height);
}

void render_tiles(game* g) {
    int rows = sizeof(level) / sizeof(level[0]);     // Number of rows (y)
    int cols = sizeof(level[0]) / sizeof(level[0][0]); // Number of columns (x)

    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            int tile = level[rows - 1 - y][cols - 1 - x];
            render_tile(g, tile, x * 64, y * 64);  // Use 4x pixel coordinates (16*4=64)
        }
    }
}

void render_sprite_pixel_perfect(game* g, GLuint texture, float x, float y, pixel_rect sprite_rect, int texture_width, int texture_height) {
    if (!g || g->sprite_shader == 0 || texture == 0) {
        return;
    }
    
    float quad_width = sprite_rect.w * 4.0f;   // 4x pixel scaling
    float quad_height = sprite_rect.h * 4.0f;  // 4x pixel scaling
    
    float hw = quad_width * 0.5f;   
    float hh = quad_height * 0.5f;
    
    float u1 = (float)sprite_rect.x / (float)texture_width;
    float v1 = (float)sprite_rect.y / (float)texture_height;
    float u2 = (float)(sprite_rect.x + sprite_rect.w) / (float)texture_width;
    float v2 = (float)(sprite_rect.y + sprite_rect.h) / (float)texture_height;
    
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
    
    // Create temporary VAO and buffers
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
    
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);
    glDeleteBuffers(1, &EBO);
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
        render_scaled_sprite(g, g->textures[TEXTURE_HEALTH_FILL], 
                           bar_x + fill_offset, bar_y, fill_width, 32.0f);
    }
    
    glUniform4f(g->tint_loc, 1.0f, 1.0f, 1.0f, 1.0f);
    render_scaled_sprite(g, g->textures[TEXTURE_HEALTH_BAR], 
                       bar_x, bar_y, 128.0f, 32.0f);
    
    glUniform4f(g->tint_loc, 1.0f, 1.0f, 1.0f, 1.0f);
}

void render_entities(game* g) {
    if (!g) return;
        
    if (g->sprite_shader != 0) {
        glUseProgram(g->sprite_shader);
        
        glUniformMatrix4fv(g->view_loc, 1, GL_FALSE, g->view_matrix);
        glUniformMatrix4fv(g->projection_loc, 1, GL_FALSE, g->ortho_projection);
        
        for (int i = 0; i < scene.entity_count; i++) {
            entity* e = &scene.entities[i];
            float x = e->pos.x;
            float y = e->pos.y;
            int sprite_id = e->current_animation.animation.frames[e->current_animation.frame_index];
            
            // Render entity sprite at 4x pixel scale
            if (g->textures[e->sprite_sheet.texture_id] != 0) {
                    pixel_rect pixel_region = e->sprite_sheet.sprites[sprite_id];
                    render_sprite_pixel_perfect(g, g->textures[e->sprite_sheet.texture_id], x, y, 
                                               pixel_region, e->sprite_sheet.width, e->sprite_sheet.height);
            }
            
            render_health_bar(g, x, y, e->health, 100.0f); // assuming max health is 100
        }
        
    } else {
    }
}