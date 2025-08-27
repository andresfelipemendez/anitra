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

void debug_draw_line(debug_renderer* dr, vec2 start, vec2 end, debug_color color) {
    int idx = dr->current_line_count * 10;
    dr->vertex_buffer[idx + 0] = start.x;
    dr->vertex_buffer[idx + 1] = start.y;
    dr->vertex_buffer[idx + 2] = color.r;
    dr->vertex_buffer[idx + 3] = color.g;
    dr->vertex_buffer[idx + 4] = color.b;
    dr->vertex_buffer[idx + 5] = end.x;
    dr->vertex_buffer[idx + 6] = end.y;
    dr->vertex_buffer[idx + 7] = color.r;  // Same color for both vertices
    dr->vertex_buffer[idx + 8] = color.g;
    dr->vertex_buffer[idx + 9] = color.b;
    dr->current_line_count++;
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
     glUseProgram(g->sprite_shader);
    
    if (g->translation_loc != -1) {
        glUniform2f(g->translation_loc, x, y);
    }
    
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g->textures[TEXTURE_TILES]);
    
    if (g->texture_loc != -1) {
        glUniform1i(g->texture_loc, 0);
    }
    
    if (g->quad_VAO != 0) {
        glBindVertexArray(g->quad_VAO);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    }

    if (g->translation_loc != -1) {
        glUniform2f(g->translation_loc, x, y);
    }
    glUniformMatrix4fv(g->view_loc, 1, GL_FALSE, g->view_matrix);
    pixel_rect pixel_region = tiles.sprites[tile];
    rect region = pixel_to_uv(pixel_region, tiles);
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

void render_tiles(game* g) {
    int rows = sizeof(level) / sizeof(level[0]);     // Number of rows (y)
    int cols = sizeof(level[0]) / sizeof(level[0][0]); // Number of columns (x)

    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            // Read from the opposite end of the array
            int tile = level[rows - 1 - y][cols - 1 - x];
            render_tile(g, tile, x * 64, y * 64);
        }
    }
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
            int sprite_id = e->current_animation.frame_index;
            
            if (g->textures[e->sprite_sheet.texture_id] != 0) {
                if (sprite_id >= 0 && sprite_id < 5 ) {
                    pixel_rect pixel_region = e->sprite_sheet.sprites[sprite_id];
                    rect uv_region = pixel_to_uv(pixel_region, e->sprite_sheet);
                   
                    render_sprite_region(g, g->textures[e->sprite_sheet.texture_id], x, y, uv_region);
                } else {
                    // Fallback to full texture rendering
                    render_sprite(g, g->textures[e->sprite_sheet.texture_id], x, y);
                }
            }
        }
        
    } else {
    }
}