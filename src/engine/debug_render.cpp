#include "debug_render.h"

void debug_draw_line(debug_renderer* dr, vec2 start, vec2 end, debug_color color) {
    if (!dr || dr->current_line_count >= dr->max_lines) return;
    
    // Add line vertices to buffer
    int idx = dr->current_line_count * 10;
    dr->vertex_buffer[idx] = start.x;
    dr->vertex_buffer[idx + 1] = start.y;
    dr->vertex_buffer[idx + 2] = color.r;
    dr->vertex_buffer[idx + 3] = color.g;
    dr->vertex_buffer[idx + 4] = color.b;
    dr->vertex_buffer[idx + 5] = end.x;
    dr->vertex_buffer[idx + 6] = end.y;
    dr->vertex_buffer[idx + 7] = color.r;
    dr->vertex_buffer[idx + 8] = color.g;
    dr->vertex_buffer[idx + 9] = color.b;
    dr->current_line_count++;
}

void debug_draw_rect(debug_renderer* dr, vec2 center, float width, float height, debug_color color) {
    if (!dr) return;
    
    float half_width = width * 0.5f;
    float half_height = height * 0.5f;
    
    vec2 top_left = {center.x - half_width, center.y + half_height};
    vec2 top_right = {center.x + half_width, center.y + half_height};
    vec2 bottom_right = {center.x + half_width, center.y - half_height};
    vec2 bottom_left = {center.x - half_width, center.y - half_height};
    
    debug_draw_line(dr, top_left, top_right, color);
    debug_draw_line(dr, top_right, bottom_right, color);
    debug_draw_line(dr, bottom_right, bottom_left, color);
    debug_draw_line(dr, bottom_left, top_left, color);
}
