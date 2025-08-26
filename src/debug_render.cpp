#include "debug_render.h"

void debug_draw_line(debug_renderer* dr, vec2 start, vec2 end, debug_color color) {
    if (!dr || dr->current_line_count >= dr->max_lines) return;
    
    // Set color uniform
    if (dr->debug_color_loc != -1) {
        glUniform4f(dr->debug_color_loc, color.r, color.g, color.b, color.a);
    }
    
    // Add line vertices to buffer
    int idx = dr->current_line_count * 10;
    dr->line_vertices[idx] = start.x;
    dr->line_vertices[idx + 1] = start.y;
    dr->line_vertices[idx + 2] = color.r;
    dr->line_vertices[idx + 3] = color.g;
    dr->line_vertices[idx + 4] = color.b;
    dr->line_vertices[idx + 5] = end.x;
    dr->line_vertices[idx + 6] = end.y;
    dr->line_vertices[idx + 7] = color.r;
    dr->line_vertices[idx + 8] = color.g;
    dr->line_vertices[idx + 9] = color.b;
    dr->current_line_count++;
}
