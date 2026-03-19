#include "debug_render.h"

void debug_draw_line(debug_renderer* dr, vec2 start, vec2 end, debug_color color) {
    if (!dr || !dr->vertex_buffer || dr->current_line_count >= dr->max_lines) return;
    line_expand(&dr->vertex_buffer[dr->current_line_count * LINE_VERTS_PER_LINE],
                start.x, start.y, 0.0f,
                end.x, end.y, 0.0f,
                color.r, color.g, color.b);
    dr->current_line_count++;
}

void debug_draw_rect(debug_renderer* dr, vec2 center, float width, float height, debug_color color) {
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
