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
