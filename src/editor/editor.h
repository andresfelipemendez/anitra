#ifndef EDITOR_H
#define EDITOR_H

#include "math3d.h"

/* ── Editor line vertex (matches externals pipeline: float3 pos + float3 color) ── */

typedef struct { float x, y, z, r, g, b; } editor_line_vert;

/* ── Editor persistent state (lives in memory struct, survives hot-reload) ── */

#define EDITOR_MAX_LINES 2048

typedef struct editor_state {
    /* Camera */
    Vec3  cam_pos;
    float cam_yaw, cam_pitch;
    float cam_speed, cam_sens;
    int   cam_mouse_look;

    /* Window state */
    void *window;        /* SDL_Window* — set by externals, read by editor */
    int   open;
    int   initialized;

    /* Panel rect within the window (set by externals from dock layout each frame) */
    float panel_x, panel_y;   /* top-left in window pixels */
    float panel_w, panel_h;   /* content area size in pixels (below header) */

    /* Gizmo */
    int   gizmo_hovered;   /* 0=none, 1=X, 2=Y, 3=Z */
    int   gizmo_active;
    Vec3  gizmo_drag_start_eye;
    Vec3  gizmo_drag_start_target;
    float gizmo_drag_accum;
    Vec3  gizmo_screen_axis;
    float gizmo_world_per_pixel;

    /* Line output — written by editor each frame, read by externals for GPU upload */
    editor_line_vert lines[EDITOR_MAX_LINES * 2];
    int line_count;      /* number of line segments (each = 2 vertices) */

    /* Font metrics — pre-computed by externals at init, used by editor for Clay text measurement.
       Advances are in design units scaled to 1px font height; multiply by fontSize for pixel width. */
    float font_advances[128];
    float font_line_height;    /* (ascent - descent + lineGap) normalized to 1px */

    /* Profiler Clay render output — written by editor each frame, read by externals for GPU upload.
       profiler_clay_ctx survives hot-reload (lives in editor_arena). */
    void *profiler_clay_ctx;       /* Clay_Context* in editor_arena */
    int   profiler_cmd_count;      /* number of Clay_RenderCommand items */
    void *profiler_cmd_array;      /* Clay_RenderCommand* in Clay arena memory */

    /* Dock state — opaque, allocated from editor_arena.
       Actual type is dock_state* (defined in editor/dock.h). */
    void *dock;
} editor_state;

#endif /* EDITOR_H */
