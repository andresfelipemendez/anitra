#ifndef DOCK_H
#define DOCK_H

#include "editor_types.h"
#include <stdint.h>

/* Dock node types */
typedef enum {
    DOCK_SPLIT,
    DOCK_TABS
} DockNodeType;

/* Drag phases for docking operations */
typedef enum {
    DRAG_NONE,
    DRAG_ACTIVE,
    DRAG_COMPLETE
} DragPhase;

/* Drop zones around a dock node */
typedef enum {
    DROP_NONE,
    DROP_LEFT,
    DROP_RIGHT,
    DROP_TOP,
    DROP_BOTTOM,
    DROP_CENTER
} DropZone;

/* Dock node structure */
#define MAX_DOCK_NODES 64
#define MAX_DOCK_PANELS 16

typedef struct DockNode {
    int in_use;
    DockNodeType type;
    
    /* Layout data */
    float x, y, w, h;
    
    /* For DOCK_SPLIT nodes */
    int children[2];
    
    /* For DOCK_TABS nodes */
    PanelId panels[MAX_DOCK_PANELS];
    int panel_count;
    int active_tab;
} DockNode;

/* Drag state for docking operations */
typedef struct {
    DragPhase phase;
    int drag_window;
    int hover_window;
    int hover_node;
    DropZone hover_zone;
    float drag_start_x, drag_start_y;
} DragState;

/* Dock state structure */
#define MAX_WINDOWS 8

typedef struct dock_state {
    DockNode nodes[MAX_DOCK_NODES];
    int node_count;
    
    /* Root node index for each window */
    int root_nodes[MAX_WINDOWS];
    int window_count;
    
    DragState drag;
} dock_state;

/* Editor panel rendering functions (implemented in editor.dll) */
typedef void (*editor_panel_func)(dock_state *d, PanelId panel);

/* Editor state - populated by editor.dll, read by externals for rendering */
typedef struct editor_state {
    dock_state dock;
    int show_profiler;
    int show_inspector;
    int show_outline;
    int show_assets;
    
    /* Profiler data (populated by engine, rendered by editor) */
    double frame_time;
    double update_time;
    double render_time;
    int draw_call_count;
} editor_state;

/* Dock operations */
void dock_init(dock_state *d);
int dock_split(dock_state *d, int node_idx, int horizontal);
int dock_add_panel(dock_state *d, int node_idx, PanelId panel);
int dock_remove_panel(dock_state *d, int node_idx, PanelId panel);
void dock_set_active_tab(dock_state *d, int node_idx, int tab);
void dock_get_panel_rect(dock_state *d, int node_idx, int panel_idx,
                         float *out_x, float *out_y, float *out_w, float *out_h);

/* Drag operations */
void dock_begin_drag(dock_state *d, int window_idx, float x, float y);
void dock_update_drag(dock_state *d, float x, float y);
void dock_end_drag(dock_state *d);

#endif /* DOCK_H */
