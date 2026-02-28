#ifndef EDITOR_H
#define EDITOR_H

#include <arena.h>

/* Panel IDs for docking system */
typedef enum {
    PANEL_GAME,
    PANEL_PROFILER,
    PANEL_INSPECTOR,
    PANEL_OUTLINE,
    PANEL_ASSETS,
    PANEL_COUNT
} PanelId;

/* Editor state - populated by editor.dll, read by externals for rendering */
typedef struct editor_state {
    /* Dock state (dock.h defines the full structure) */
    void *dock;
    
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

#endif /* EDITOR_H */
