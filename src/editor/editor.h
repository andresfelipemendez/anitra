#ifndef EDITOR_H
#define EDITOR_H

#include "math3d.h"
#include <stdint.h>

/* ── Editor line vertex (matches externals pipeline: float3 pos + float3 color) ── */

typedef struct { float x, y, z, r, g, b; } editor_line_vert;

/* ── Editor persistent state (lives in memory struct, survives hot-reload) ── */

#define EDITOR_MAX_LINES 2048
#define PROF_GRID_MAX_COLS 32
#define PROF_GRID_MAX_ROWS 512
#define PROFILER_TREE_MAX_NODES 65536
#define SCENE_TREE_MAX_ENTITIES 512

#ifndef MENU_BAR_HEIGHT
#define MENU_BAR_HEIGHT 28
#endif

#define EDITOR_CAMERA_PERSPECTIVE 0
#define EDITOR_CAMERA_ORTHOGRAPHIC 1

typedef struct editor_state {
    /* Arena pointers — set by externals init, used by editor for allocation */
    struct arena *root_arena;      /* pointer to memory.arena (for profiler arena display) */
    struct arena *editor_arena;    /* sub-arena for editor allocations */

    /* Clay editor context — opaque Clay_Context*, allocated from editor_arena */
    void *clay_editor;

    /* Camera */
    Vec3  cam_pos;
    float cam_yaw, cam_pitch;
    float cam_speed, cam_sens;
    int   cam_projection_mode; /* EDITOR_CAMERA_* */
    float cam_ortho_size;      /* vertical orthographic span in world units */
    int   cam_mouse_look;
    uint8_t cam_mouse_button; /* SDL_BUTTON_LEFT / SDL_BUTTON_RIGHT while looking */

    /* Window state */
    void *window;        /* SDL_Window* — set by externals, read by editor */
    int   open;
    int   initialized;

    /* Panel rect within the window (set by externals from dock layout each frame) */
    float panel_x, panel_y;   /* top-left in window pixels */
    float panel_w, panel_h;   /* content area size in pixels (below header) */

    /* Gizmo */
    int   gizmo_hovered;   /* 0=none, 1=X, 2=Y, 3=Z, 4..9=capsule edit handles */
    int   gizmo_active;
    int   gizmo_entity_index;
    Vec3  gizmo_drag_start_eye;
    Vec3  gizmo_drag_start_target;
    Vec3  gizmo_drag_start_entity_pos;
    Vec3  gizmo_drag_axis_world;
    int   gizmo_drag_mode;              /* 0=translate, 1=capsule radius, 2=capsule half-height */
    float gizmo_drag_start_capsule_radius;
    float gizmo_drag_start_capsule_half_height;
    float gizmo_drag_axis_local_scale;  /* world-to-local conversion factor for collider drags */
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

    /* Scene tree Clay render output — written by editor each frame, read by externals. */
    void *scene_tree_clay_ctx;     /* Clay_Context* in editor_arena */
    int   scene_tree_cmd_count;    /* number of Clay_RenderCommand items */
    void *scene_tree_cmd_array;    /* Clay_RenderCommand* in Clay arena memory */
    void *inspector_clay_ctx;      /* Clay_Context* in editor_arena */
    int   inspector_cmd_count;     /* number of Clay_RenderCommand items */
    void *inspector_cmd_array;     /* Clay_RenderCommand* in Clay arena memory */

    /* Hot-reload-persistent collapsed state */
    int profiler_tree_collapsed[PROFILER_TREE_MAX_NODES];
    int scene_tree_collapsed[SCENE_TREE_MAX_ENTITIES];
    int scene_selected_entity;

    /* Profiler input — accumulated per frame by event handler, consumed by profiler_layout */
    float prof_mouse_x, prof_mouse_y; /* mouse position in profiler-local coords */
    float prof_scroll_y;              /* scroll wheel delta this frame */
    int   prof_mouse_down;            /* left button held */
    int   prof_click;                 /* 1 on the frame left button was pressed */
    int   prof_hover_record;          /* hovered flat record index, -1 = none */

    /* Scene tree input — accumulated per frame by event handler, consumed by scene_tree_layout */
    float scene_tree_mouse_x, scene_tree_mouse_y; /* mouse position in scene-tree-local coords */
    float scene_tree_scroll_y;                    /* scroll wheel delta this frame */
    int   scene_tree_mouse_down;                  /* left button held */
    int   scene_tree_click;                       /* 1 on the frame left button was pressed */
    int   scene_tree_drag_active;                 /* dragging an entity row */
    int   scene_tree_drag_entity;                 /* dragged entity index, -1 = none */
    int   scene_tree_drop_target;                 /* hovered drop target entity, -1 = none */
    int   scene_tree_drop_mode;                   /* scene-tree drop mode enum */

    /* Scrollbar data — set by profiler_layout after EndLayout, read by externals renderer */
    float prof_scroll_pos;            /* current scroll Y offset (<=0) */
    float prof_content_h;             /* total content height */
    float prof_container_h;           /* visible container height */
    float prof_track_x, prof_track_y; /* PTreeScroll bounding box origin (Clay coords) */
    float prof_track_w, prof_track_h; /* PTreeScroll bounding box size */

    /* Grid texture — CPU pixel buffer built by editor, uploaded by externals.
       Each pixel = one 64KB arena cell. Nearest-neighbor sampled to panel size. */
    uint8_t  prof_grid_pixels[PROF_GRID_MAX_COLS * PROF_GRID_MAX_ROWS * 4]; /* RGBA */
    int      prof_grid_w, prof_grid_h;    /* actual pixel dimensions this frame */
    float    prof_grid_x, prof_grid_y;    /* Clay bounding box origin */
    float    prof_grid_bw, prof_grid_bh;  /* Clay bounding box size */

    /* Menu bar Clay render output — written by editor each frame, read by externals. */
    void *menu_bar_clay_ctx;       /* Clay_Context* in editor_arena */
    int   menu_bar_cmd_count;      /* number of Clay_RenderCommand items */
    void *menu_bar_cmd_array;      /* Clay_RenderCommand* in Clay arena memory */
    void *editor_toolbar_clay_ctx; /* Clay_Context* for editor viewport toolbar */
    int   editor_toolbar_cmd_count;
    void *editor_toolbar_cmd_array;

    /* Menu bar interaction state */
    int   menu_open;               /* which top-level menu is open (-1 = none) */
    int   menu_hover;              /* which submenu item is hovered (-1 = none) */
    float menu_mouse_x, menu_mouse_y; /* mouse position in window coords */
    int   menu_click;              /* 1 on the frame left button was pressed in menu bar area */

    /* Dock state — opaque, allocated from editor_arena.
       Actual type is dock_state* (defined in editor/dock.h). */
    void *dock;
} editor_state;

#endif /* EDITOR_H */
