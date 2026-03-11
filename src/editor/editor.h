#ifndef EDITOR_H
#define EDITOR_H

#include "math3d.h"
#include "editor_types.h"
#include "error_value.h"
#include "collab/collab_client.h"
#include <stdint.h>
#include <string.h>

/* ── Editor line vertex (matches externals pipeline: float3 pos + float3 color) ── */

typedef struct { float x, y, z, r, g, b; } editor_line_vert;

/* ── Editor persistent state (lives in memory struct, survives hot-reload) ── */

#define EDITOR_MAX_LINES 2048
#define PROF_GRID_MAX_COLS 32
#define PROF_GRID_MAX_ROWS 512
#define PROFILER_TREE_MAX_NODES 65536
#define SCENE_TREE_MAX_ENTITIES 512
#define EDITOR_CPU_TREE_MAX_ZONES 256

/* Project browser icon grid */
#define PB_ICON_SIZE         80
#define PB_ICON_GAP          6
#define PB_FOLDER_TREE_WIDTH 180
#define PB_MAX_THUMBNAILS    32

typedef struct {
    char  key[64];  /* asset key for scene_model_asset lookup */
    int   type;     /* 0=model, 1=animation, 2=dungeon piece, 3=sprite */
    float x, y, w, h;
} pb_thumbnail_request;

#ifndef MENU_BAR_HEIGHT
#define MENU_BAR_HEIGHT 28
#endif

#define EDITOR_CAMERA_PERSPECTIVE 0
#define EDITOR_CAMERA_ORTHOGRAPHIC 1


/* ── Dock System ─────────────────────────────────────────────────────── */
/* Dock node types */
typedef enum {
    DOCK_SPLIT_H,   /* horizontal split — children side by side */
    DOCK_SPLIT_V,   /* vertical split — children top/bottom */
    DOCK_TABS       /* leaf node with tabbed panels */
} DockNodeType;

/* Drag phases for docking operations */
typedef enum {
    DRAG_IDLE,
    DRAG_PENDING,
    DRAG_ACTIVE
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

/* Constants */
#define MAX_DOCK_NODES       64
#define MAX_DOCK_PANELS      16
#define MAX_DOCK_WINDOWS     8
#define MAX_TABS_PER_NODE    8
#define DOCK_HEADER_HEIGHT   24
#define DOCK_MIN_PANEL_SIZE  80
#define DOCK_DRAG_THRESHOLD  6
#define DOCK_DIVIDER_HIT     6.0f

enum {
    DOCK_ERR_INVALID_ARGS = 1000,
    DOCK_ERR_INVALID_NODE_INDEX,
    DOCK_ERR_NODE_NOT_IN_USE,
    DOCK_ERR_NODE_NOT_TABS,
    DOCK_ERR_NODE_FULL,
    DOCK_ERR_PANEL_NOT_FOUND,
    DOCK_ERR_ALLOC_NODE_FAILED,
    DOCK_ERR_INVALID_DROP_ZONE,
    DOCK_ERR_INVALID_OUTPUT
};

/* Panel display names (indexed by PanelId) */
static const char *panel_names[PANEL_COUNT] = {
    "Game",
    "Editor",
    "Profiler",
    "Scene Tree",
    "Inspector",
    "Outline",
    "Project",
    "Cache Profiler",
    "CPU Profiler"
};

/* Dock node structure */
typedef struct DockNode {
    int in_use;
    DockNodeType type;

    /* Layout rectangle (pixels) */
    float x, y, w, h;

    /* For DOCK_SPLIT_H / DOCK_SPLIT_V nodes */
    int children[2];
    float ratio;            /* 0..1 split position */

    /* For DOCK_TABS nodes */
    PanelId panels[MAX_DOCK_PANELS];
    int panel_count;
    int active_tab;
} DockNode;

/* Per-window state (main window + tear-offs) */
typedef struct {
    void *sdl_window;       /* SDL_Window* */
    int   root_node;        /* index into dock_state.nodes[] */
    int   in_use;
} DockWindow;

/* Drag state for docking operations */
typedef struct {
    DragPhase phase;
    PanelId   panel;            /* panel being dragged */
    int       source_node;      /* node the panel was dragged from */
    int       source_window;    /* window index of source */
    int       hover_window;
    int       hover_node;
    DropZone  hover_zone;
    float     grab_x, grab_y;   /* mouse pos at grab start */
} DragState;

/* Resize state for split-divider dragging */
typedef struct {
    int   active;
    int   node;             /* split node being resized */
    int   window;           /* window index */
    float initial_ratio;
    float grab_pos;         /* mouse pos along split axis at grab start */
} ResizeState;

/* Dock state structure */
typedef struct dock_state {
    DockNode    nodes[MAX_DOCK_NODES];
    int         node_count;

    DockWindow  windows[MAX_DOCK_WINDOWS];

    int         initialized;

    DragState   drag;
    ResizeState resize;

    /* Deferred commands (executed between frames by externals) */
    int   cmd_tear_off;
    int   cmd_redock;
    int   cmd_redock_target;
    float cmd_screen_x, cmd_screen_y;
    int   cmd_cleanup_windows;
} dock_state;

/* Editor panel rendering callback */
typedef void (*editor_panel_func)(dock_state *d, PanelId panel);

/* ══════════════════════════════════════════════════════════════
 *  Declarations
 * ══════════════════════════════════════════════════════════════ */

/* Core */
static void dock_init(dock_state *d);
static void dock_init_default(dock_state *d);
static int  dock_alloc_node(dock_state *d);

/* Layout */
static void dock_layout(dock_state *d, int window_idx, int win_w, int win_h);

/* Panel management */
static int  dock_add_panel_with_error(dock_state *d, int node_idx, PanelId panel,
                                      error_value *out_error);
static int  dock_add_panel(dock_state *d, int node_idx, PanelId panel);
static int  dock_remove_panel_with_error(dock_state *d, int node_idx, PanelId panel,
                                         error_value *out_error);
static int  dock_remove_panel(dock_state *d, int node_idx, PanelId panel);
static void dock_set_active_tab(dock_state *d, int node_idx, int tab);

/* Splitting */
static int  dock_split_with_error(dock_state *d, int node_idx, int horizontal,
                                  error_value *out_error);
static int  dock_split(dock_state *d, int node_idx, int horizontal);
static int  dock_split_at_with_error(dock_state *d, int window_idx, int node_idx,
                                     PanelId panel, DropZone zone,
                                     int *out_new_leaf, int *out_existing_leaf,
                                     error_value *out_error);
static void dock_split_at(dock_state *d, int window_idx, int node_idx,
                          PanelId panel, DropZone zone);

/* Node queries */
static int  dock_leaf_for_panel(dock_state *d, int root_node, PanelId panel);
static int  dock_find_leaf_for_panel_global(dock_state *d, PanelId panel,
                                            int *out_window_idx);
static int  dock_node_at_point(dock_state *d, int root_node, float x, float y);
static int  dock_divider_at_point(dock_state *d, int root_node, float x, float y);
static int  dock_window_for_sdl(dock_state *d, void *sdl_window);

/* Drop zone detection */
static DropZone dock_drop_zone(DockNode *node, float x, float y);

/* Header hit testing */
static int  header_hit_test_node(dock_state *d, int root_node, float x, float y,
                                 int *out_node, PanelId *out_panel, int *out_tab_idx);

/* Tree manipulation */
static int  dock_collapse_empty_with_error(dock_state *d, int root_node,
                                           int *out_root_node,
                                           error_value *out_error);
static int  dock_collapse_empty(dock_state *d, int root_node);
static void dock_collect_leaves(dock_state *d, int root_node,
                                PanelId *out_panels, int *out_count, int max_count);
static void dock_free_node(dock_state *d, int idx);
static int  dock_find_parent(dock_state *d, int root, int target, int *out_slot);
static void dock_free_subtree(dock_state *d, int root_node);
static void dock_get_panel_rect(dock_state *d, int node_idx, int panel_idx,
                                float *out_x, float *out_y, float *out_w, float *out_h);

/* ══════════════════════════════════════════════════════════════
 *  Implementations
 * ══════════════════════════════════════════════════════════════ */

static int dock_alloc_node(dock_state *d) {
    int i;
    for (i = 0; i < MAX_DOCK_NODES; i++) {
        if (!d->nodes[i].in_use) {
            memset(&d->nodes[i], 0, sizeof(DockNode));
            d->nodes[i].in_use = 1;
            d->nodes[i].type = DOCK_TABS;
            d->nodes[i].children[0] = -1;
            d->nodes[i].children[1] = -1;
            return i;
        }
    }
    return -1;
}

static void dock_init(dock_state *d) {
    int root;
    memset(d, 0, sizeof(dock_state));
    root = dock_alloc_node(d);
    d->nodes[root].panels[0] = PANEL_GAME;
    d->nodes[root].panel_count = 1;
    d->windows[0].in_use = 1;
    d->windows[0].root_node = root;
    d->initialized = 1;
}

static void dock_init_default(dock_state *d) {
    int root, left, center_right_split, center_vert_split, center_split;
    int game_leaf, editor_leaf, assets_leaf, right;
    memset(d, 0, sizeof(dock_state));

    /* Root: horizontal split (Scene Tree | rest) */
    root = dock_alloc_node(d);
    d->nodes[root].type = DOCK_SPLIT_H;
    d->nodes[root].ratio = 0.19f;

    /* Left leaf: Scene Tree only */
    left = dock_alloc_node(d);
    d->nodes[left].panels[0] = PANEL_SCENE_TREE;
    d->nodes[left].panel_count = 1;
    d->nodes[left].active_tab = 0;

    /* Right side: horizontal split (center area | Profiler/Inspector) */
    center_right_split = dock_alloc_node(d);
    d->nodes[center_right_split].type = DOCK_SPLIT_H;
    d->nodes[center_right_split].ratio = 0.78f;

    /* Center area: vertical split (Game+Editor top | Assets bottom) */
    center_vert_split = dock_alloc_node(d);
    d->nodes[center_vert_split].type = DOCK_SPLIT_V;
    d->nodes[center_vert_split].ratio = 0.68f;

    /* Top of center: horizontal split (Game | Editor) */
    center_split = dock_alloc_node(d);
    d->nodes[center_split].type = DOCK_SPLIT_H;
    d->nodes[center_split].ratio = 0.5f;

    game_leaf = dock_alloc_node(d);
    d->nodes[game_leaf].panels[0] = PANEL_GAME;
    d->nodes[game_leaf].panel_count = 1;

    editor_leaf = dock_alloc_node(d);
    d->nodes[editor_leaf].panels[0] = PANEL_EDITOR;
    d->nodes[editor_leaf].panel_count = 1;

    /* Bottom of center: Assets panel */
    assets_leaf = dock_alloc_node(d);
    d->nodes[assets_leaf].panels[0] = PANEL_ASSETS;
    d->nodes[assets_leaf].panel_count = 1;

    /* Right leaf: Profiler + Inspector + Cache Profiler + CPU Profiler tabs */
    right = dock_alloc_node(d);
    d->nodes[right].panels[0] = PANEL_PROFILER;
    d->nodes[right].panels[1] = PANEL_INSPECTOR;
    d->nodes[right].panels[2] = PANEL_CACHE_PROFILER;
    d->nodes[right].panels[3] = PANEL_CPU_PROFILER;
    d->nodes[right].panel_count = 4;
    d->nodes[right].active_tab = 1;

    /* Wire up children */
    d->nodes[root].children[0] = left;
    d->nodes[root].children[1] = center_right_split;
    d->nodes[center_right_split].children[0] = center_vert_split;
    d->nodes[center_right_split].children[1] = right;
    d->nodes[center_vert_split].children[0] = center_split;
    d->nodes[center_vert_split].children[1] = assets_leaf;
    d->nodes[center_split].children[0] = game_leaf;
    d->nodes[center_split].children[1] = editor_leaf;

    d->windows[0].in_use = 1;
    d->windows[0].root_node = root;
    d->initialized = 1;
}

/* ── Layout ────────────────────────────────────────────────── */

static void dock_layout_node(dock_state *d, int idx, float x, float y, float w, float h) {
    DockNode *n;
    if (idx < 0 || idx >= MAX_DOCK_NODES) return;
    n = &d->nodes[idx];
    if (!n->in_use) return;

    n->x = x; n->y = y; n->w = w; n->h = h;

    if (n->type == DOCK_SPLIT_H) {
        float left_w = w * n->ratio;
        dock_layout_node(d, n->children[0], x, y, left_w, h);
        dock_layout_node(d, n->children[1], x + left_w, y, w - left_w, h);
    } else if (n->type == DOCK_SPLIT_V) {
        float top_h = h * n->ratio;
        dock_layout_node(d, n->children[0], x, y, w, top_h);
        dock_layout_node(d, n->children[1], x, y + top_h, w, h - top_h);
    }
    /* DOCK_TABS: just set bounds, nothing to recurse */
}

static void dock_layout(dock_state *d, int window_idx, int win_w, int win_h) {
    if (window_idx < 0 || window_idx >= MAX_DOCK_WINDOWS) return;
    if (!d->windows[window_idx].in_use) return;
    dock_layout_node(d, d->windows[window_idx].root_node,
                     0, (float)MENU_BAR_HEIGHT,
                     (float)win_w, (float)win_h - (float)MENU_BAR_HEIGHT);
}

/* ── Panel management ──────────────────────────────────────── */

static int dock_add_panel_with_error(dock_state *d, int node_idx, PanelId panel,
                                     error_value *out_error) {
    DockNode *n;
    if (!d) {
        ERRV_RETURN_CODE_ERR(-1, out_error, DOCK_ERR_INVALID_ARGS,
                             "dock_add_panel: null dock state");
    }
    if (node_idx < 0 || node_idx >= MAX_DOCK_NODES) {
        ERRV_RETURN_CODE_ERR(-1, out_error, DOCK_ERR_INVALID_NODE_INDEX,
                             "dock_add_panel: invalid node index");
    }
    n = &d->nodes[node_idx];
    if (!n->in_use) {
        ERRV_RETURN_CODE_ERR(-1, out_error, DOCK_ERR_NODE_NOT_IN_USE,
                             "dock_add_panel: node not in use");
    }
    if (n->type != DOCK_TABS) {
        ERRV_RETURN_CODE_ERR(-1, out_error, DOCK_ERR_NODE_NOT_TABS,
                             "dock_add_panel: node is not a tab node");
    }
    if (n->panel_count >= MAX_TABS_PER_NODE) {
        ERRV_RETURN_CODE_ERR(-1, out_error, DOCK_ERR_NODE_FULL,
                             "dock_add_panel: target node has no free tab slots");
    }
    n->panels[n->panel_count++] = panel;
    ERRV_RETURN_CODE_OK(n->panel_count - 1, out_error);
}

static int dock_add_panel(dock_state *d, int node_idx, PanelId panel) {
    return dock_add_panel_with_error(d, node_idx, panel, NULL);
}

static int dock_remove_panel_with_error(dock_state *d, int node_idx, PanelId panel,
                                        error_value *out_error) {
    DockNode *n;
    int i, j;
    if (!d) {
        ERRV_RETURN_CODE_ERR(-1, out_error, DOCK_ERR_INVALID_ARGS,
                             "dock_remove_panel: null dock state");
    }
    if (node_idx < 0 || node_idx >= MAX_DOCK_NODES) {
        ERRV_RETURN_CODE_ERR(-1, out_error, DOCK_ERR_INVALID_NODE_INDEX,
                             "dock_remove_panel: invalid node index");
    }
    n = &d->nodes[node_idx];
    if (!n->in_use) {
        ERRV_RETURN_CODE_ERR(-1, out_error, DOCK_ERR_NODE_NOT_IN_USE,
                             "dock_remove_panel: node not in use");
    }
    if (n->type != DOCK_TABS) {
        ERRV_RETURN_CODE_ERR(-1, out_error, DOCK_ERR_NODE_NOT_TABS,
                             "dock_remove_panel: node is not a tab node");
    }

    for (i = 0; i < n->panel_count; i++) {
        if (n->panels[i] == panel) {
            for (j = i; j < n->panel_count - 1; j++)
                n->panels[j] = n->panels[j + 1];
            n->panel_count--;
            if (n->active_tab >= n->panel_count && n->panel_count > 0)
                n->active_tab = n->panel_count - 1;
            ERRV_RETURN_CODE_OK(0, out_error);
        }
    }
    ERRV_RETURN_CODE_ERR(-1, out_error, DOCK_ERR_PANEL_NOT_FOUND,
                         "dock_remove_panel: panel not found in source node");
}

static int dock_remove_panel(dock_state *d, int node_idx, PanelId panel) {
    return dock_remove_panel_with_error(d, node_idx, panel, NULL);
}

static void dock_set_active_tab(dock_state *d, int node_idx, int tab) {
    DockNode *n;
    if (node_idx < 0 || node_idx >= MAX_DOCK_NODES) return;
    n = &d->nodes[node_idx];
    if (!n->in_use || n->type != DOCK_TABS) return;
    if (tab >= 0 && tab < n->panel_count) n->active_tab = tab;
}

static void dock_get_panel_rect(dock_state *d, int node_idx, int panel_idx,
                                float *out_x, float *out_y, float *out_w, float *out_h) {
    DockNode *n;
    if (node_idx < 0 || node_idx >= MAX_DOCK_NODES) return;
    n = &d->nodes[node_idx];
    if (!n->in_use || n->type != DOCK_TABS) return;
    *out_x = n->x;
    *out_y = n->y + DOCK_HEADER_HEIGHT;
    *out_w = n->w;
    *out_h = n->h - DOCK_HEADER_HEIGHT;
}

/* ── Splitting ─────────────────────────────────────────────── */

static int dock_split_with_error(dock_state *d, int node_idx, int horizontal,
                                 error_value *out_error) {
    DockNode *orig;
    int split_idx, new_leaf;
    if (!d) {
        ERRV_RETURN_CODE_ERR(-1, out_error, DOCK_ERR_INVALID_ARGS,
                             "dock_split: null dock state");
    }
    if (node_idx < 0 || node_idx >= MAX_DOCK_NODES) {
        ERRV_RETURN_CODE_ERR(-1, out_error, DOCK_ERR_INVALID_NODE_INDEX,
                             "dock_split: invalid node index");
    }
    if (!d->nodes[node_idx].in_use) {
        ERRV_RETURN_CODE_ERR(-1, out_error, DOCK_ERR_NODE_NOT_IN_USE,
                             "dock_split: node not in use");
    }

    split_idx = dock_alloc_node(d);
    new_leaf = dock_alloc_node(d);
    if (split_idx < 0 || new_leaf < 0) {
        if (split_idx >= 0) dock_free_node(d, split_idx);
        if (new_leaf >= 0) dock_free_node(d, new_leaf);
        ERRV_RETURN_CODE_ERR(-1, out_error, DOCK_ERR_ALLOC_NODE_FAILED,
                             "dock_split: failed to allocate split nodes");
    }

    orig = &d->nodes[node_idx];

    /* Copy original node to split_idx, then turn node_idx into a split */
    d->nodes[split_idx] = *orig;
    d->nodes[split_idx].in_use = 1;

    orig->type = horizontal ? DOCK_SPLIT_H : DOCK_SPLIT_V;
    orig->ratio = 0.5f;
    orig->children[0] = split_idx;
    orig->children[1] = new_leaf;
    orig->panel_count = 0;

    ERRV_RETURN_CODE_OK(new_leaf, out_error);
}

static int dock_split(dock_state *d, int node_idx, int horizontal) {
    return dock_split_with_error(d, node_idx, horizontal, NULL);
}

static int dock_split_at_with_error(dock_state *d, int window_idx, int node_idx,
                                    PanelId panel, DropZone zone,
                                    int *out_new_leaf, int *out_existing_leaf,
                                    error_value *out_error) {
    DockNode *orig;
    int split_idx, new_leaf;
    int horizontal;
    (void)window_idx;
    if (!d) {
        ERRV_RETURN_CODE_ERR(-1, out_error, DOCK_ERR_INVALID_ARGS,
                             "dock_split_at: null dock state");
    }
    if (node_idx < 0 || node_idx >= MAX_DOCK_NODES) {
        ERRV_RETURN_CODE_ERR(-1, out_error, DOCK_ERR_INVALID_NODE_INDEX,
                             "dock_split_at: invalid node index");
    }
    if (!d->nodes[node_idx].in_use) {
        ERRV_RETURN_CODE_ERR(-1, out_error, DOCK_ERR_NODE_NOT_IN_USE,
                             "dock_split_at: target node not in use");
    }
    if (!(zone == DROP_LEFT || zone == DROP_RIGHT ||
          zone == DROP_TOP || zone == DROP_BOTTOM)) {
        ERRV_RETURN_CODE_ERR(-1, out_error, DOCK_ERR_INVALID_DROP_ZONE,
                             "dock_split_at: invalid drop zone");
    }

    horizontal = (zone == DROP_LEFT || zone == DROP_RIGHT);

    split_idx = dock_alloc_node(d);
    new_leaf = dock_alloc_node(d);
    if (split_idx < 0 || new_leaf < 0) {
        if (split_idx >= 0) dock_free_node(d, split_idx);
        if (new_leaf >= 0) dock_free_node(d, new_leaf);
        ERRV_RETURN_CODE_ERR(-1, out_error, DOCK_ERR_ALLOC_NODE_FAILED,
                             "dock_split_at: failed to allocate split nodes");
    }

    orig = &d->nodes[node_idx];

    /* Copy original content to split_idx */
    d->nodes[split_idx] = *orig;
    d->nodes[split_idx].in_use = 1;

    /* New leaf gets the dropped panel */
    d->nodes[new_leaf].panels[0] = panel;
    d->nodes[new_leaf].panel_count = 1;

    /* Turn node_idx into a split */
    orig->type = horizontal ? DOCK_SPLIT_H : DOCK_SPLIT_V;
    orig->ratio = 0.5f;
    orig->panel_count = 0;

    if (zone == DROP_LEFT || zone == DROP_TOP) {
        orig->children[0] = new_leaf;
        orig->children[1] = split_idx;
    } else {
        orig->children[0] = split_idx;
        orig->children[1] = new_leaf;
    }
    if (out_new_leaf) *out_new_leaf = new_leaf;
    if (out_existing_leaf) *out_existing_leaf = split_idx;
    ERRV_RETURN_CODE_OK(0, out_error);
}

static void dock_split_at(dock_state *d, int window_idx, int node_idx,
                          PanelId panel, DropZone zone) {
    (void)dock_split_at_with_error(d, window_idx, node_idx, panel, zone,
                                   NULL, NULL, NULL);
}

/* ── Node queries ──────────────────────────────────────────── */

static int dock_leaf_for_panel(dock_state *d, int idx, PanelId panel) {
    DockNode *n;
    int i, result;
    if (idx < 0 || idx >= MAX_DOCK_NODES) return -1;
    n = &d->nodes[idx];
    if (!n->in_use) return -1;

    if (n->type == DOCK_TABS) {
        for (i = 0; i < n->panel_count; i++)
            if (n->panels[i] == panel) return idx;
        return -1;
    }

    result = dock_leaf_for_panel(d, n->children[0], panel);
    if (result >= 0) return result;
    return dock_leaf_for_panel(d, n->children[1], panel);
}

static int dock_find_leaf_for_panel_global(dock_state *d, PanelId panel,
                                           int *out_window_idx) {
    int wi, result;
    for (wi = 0; wi < MAX_DOCK_WINDOWS; wi++) {
        if (!d->windows[wi].in_use) continue;
        result = dock_leaf_for_panel(d, d->windows[wi].root_node, panel);
        if (result >= 0) {
            if (out_window_idx) *out_window_idx = wi;
            return result;
        }
    }
    if (out_window_idx) *out_window_idx = -1;
    return -1;
}

static int dock_node_at_point(dock_state *d, int idx, float x, float y) {
    DockNode *n;
    int result;
    if (idx < 0 || idx >= MAX_DOCK_NODES) return -1;
    n = &d->nodes[idx];
    if (!n->in_use) return -1;

    if (x < n->x || x > n->x + n->w || y < n->y || y > n->y + n->h)
        return -1;

    if (n->type == DOCK_TABS) return idx;

    result = dock_node_at_point(d, n->children[0], x, y);
    if (result >= 0) return result;
    return dock_node_at_point(d, n->children[1], x, y);
}

static int dock_divider_at_point(dock_state *d, int idx, float x, float y) {
    DockNode *n;
    float div_pos;
    int result;
    if (idx < 0 || idx >= MAX_DOCK_NODES) return -1;
    n = &d->nodes[idx];
    if (!n->in_use) return -1;

    if (n->type == DOCK_SPLIT_H) {
        div_pos = n->x + n->w * n->ratio;
        if (x >= div_pos - DOCK_DIVIDER_HIT && x <= div_pos + DOCK_DIVIDER_HIT &&
            y >= n->y && y <= n->y + n->h)
            return idx;
    } else if (n->type == DOCK_SPLIT_V) {
        div_pos = n->y + n->h * n->ratio;
        if (y >= div_pos - DOCK_DIVIDER_HIT && y <= div_pos + DOCK_DIVIDER_HIT &&
            x >= n->x && x <= n->x + n->w)
            return idx;
    }

    if (n->type != DOCK_TABS) {
        result = dock_divider_at_point(d, n->children[0], x, y);
        if (result >= 0) return result;
        return dock_divider_at_point(d, n->children[1], x, y);
    }
    return -1;
}

static int dock_window_for_sdl(dock_state *d, void *sdl_window) {
    int i;
    if (!sdl_window) return -1;
    for (i = 0; i < MAX_DOCK_WINDOWS; i++) {
        if (d->windows[i].in_use && d->windows[i].sdl_window == sdl_window)
            return i;
    }
    return -1;
}

/* ── Drop zone detection ───────────────────────────────────── */

static DropZone dock_drop_zone(DockNode *node, float x, float y) {
    float margin_x, margin_y;
    float rel_x, rel_y;
    if (!node) return DROP_NONE;

    margin_x = node->w * 0.25f;
    margin_y = node->h * 0.25f;
    rel_x = x - node->x;
    rel_y = y - node->y;

    if (rel_x < margin_x) return DROP_LEFT;
    if (rel_x > node->w - margin_x) return DROP_RIGHT;
    if (rel_y < margin_y) return DROP_TOP;
    if (rel_y > node->h - margin_y) return DROP_BOTTOM;
    return DROP_CENTER;
}

/* ── Header hit testing ────────────────────────────────────── */

static int header_hit_test_node(dock_state *d, int idx, float x, float y,
                                int *out_node, PanelId *out_panel, int *out_tab_idx) {
    DockNode *n;
    int result;
    if (idx < 0 || idx >= MAX_DOCK_NODES) return 0;
    n = &d->nodes[idx];
    if (!n->in_use) return 0;

    if (n->type == DOCK_TABS) {
        if (x >= n->x && x <= n->x + n->w &&
            y >= n->y && y <= n->y + DOCK_HEADER_HEIGHT) {
            float tab_w;
            int ti;
            *out_node = idx;
            if (n->panel_count > 0) {
                tab_w = n->w / (float)n->panel_count;
                ti = (int)((x - n->x) / tab_w);
                if (ti >= n->panel_count) ti = n->panel_count - 1;
                *out_panel = n->panels[ti];
                *out_tab_idx = ti;
            }
            return 1;
        }
        return 0;
    }

    result = header_hit_test_node(d, n->children[0], x, y, out_node, out_panel, out_tab_idx);
    if (result) return 1;
    return header_hit_test_node(d, n->children[1], x, y, out_node, out_panel, out_tab_idx);
}

/* ── Tree manipulation ─────────────────────────────────────── */

static int dock_collapse_empty_impl(dock_state *d, int idx, error_value *out_error) {
    DockNode *n;
    if (!d) {
        ERRV_RETURN_CODE_ERR(-1, out_error, DOCK_ERR_INVALID_ARGS,
                             "dock_collapse_empty: null dock state");
    }
    if (idx < 0) {
        ERRV_RETURN_CODE_OK(-1, out_error);
    }
    if (idx >= MAX_DOCK_NODES) {
        ERRV_RETURN_CODE_ERR(-1, out_error, DOCK_ERR_INVALID_NODE_INDEX,
                             "dock_collapse_empty: invalid node index");
    }
    n = &d->nodes[idx];
    if (!n->in_use) {
        ERRV_RETURN_CODE_ERR(-1, out_error, DOCK_ERR_NODE_NOT_IN_USE,
                             "dock_collapse_empty: node not in use");
    }

    if (n->type == DOCK_TABS) {
        if (n->panel_count == 0) {
            n->in_use = 0;
            ERRV_RETURN_CODE_OK(-1, out_error);
        }
        ERRV_RETURN_CODE_OK(idx, out_error);
    }

    /* Recursively collapse children */
    n->children[0] = dock_collapse_empty_impl(d, n->children[0], out_error);
    if (out_error && !ERRV_IS_OK(*out_error)) return -1;
    n->children[1] = dock_collapse_empty_impl(d, n->children[1], out_error);
    if (out_error && !ERRV_IS_OK(*out_error)) return -1;

    /* If both children gone, this node is empty */
    if (n->children[0] < 0 && n->children[1] < 0) {
        n->in_use = 0;
        ERRV_RETURN_CODE_OK(-1, out_error);
    }
    /* If one child gone, promote the other */
    if (n->children[0] < 0) {
        int survivor = n->children[1];
        n->in_use = 0;
        ERRV_RETURN_CODE_OK(survivor, out_error);
    }
    if (n->children[1] < 0) {
        int survivor = n->children[0];
        n->in_use = 0;
        ERRV_RETURN_CODE_OK(survivor, out_error);
    }
    ERRV_RETURN_CODE_OK(idx, out_error);
}

static int dock_collapse_empty_with_error(dock_state *d, int root_node,
                                          int *out_root_node,
                                          error_value *out_error) {
    int collapsed_root;
    if (!out_root_node) {
        ERRV_RETURN_CODE_ERR(-1, out_error, DOCK_ERR_INVALID_OUTPUT,
                             "dock_collapse_empty_with_error: out_root_node is null");
    }
    if (out_error) *out_error = ERRV_OK;
    collapsed_root = dock_collapse_empty_impl(d, root_node, out_error);
    if (out_error && !ERRV_IS_OK(*out_error)) return -1;
    *out_root_node = collapsed_root;
    ERRV_RETURN_CODE_OK(0, out_error);
}

static int dock_collapse_empty(dock_state *d, int idx) {
    int collapsed_root = -1;
    (void)dock_collapse_empty_with_error(d, idx, &collapsed_root, NULL);
    return collapsed_root;
}

static void dock_collect_leaves(dock_state *d, int idx,
                                PanelId *out_panels, int *out_count, int max_count) {
    DockNode *n;
    int i;
    if (idx < 0 || idx >= MAX_DOCK_NODES) return;
    n = &d->nodes[idx];
    if (!n->in_use) return;

    if (n->type == DOCK_TABS) {
        for (i = 0; i < n->panel_count && *out_count < max_count; i++) {
            out_panels[*out_count] = n->panels[i];
            (*out_count)++;
        }
        return;
    }

    dock_collect_leaves(d, n->children[0], out_panels, out_count, max_count);
    dock_collect_leaves(d, n->children[1], out_panels, out_count, max_count);
}

static void dock_free_node(dock_state *d, int idx) {
    if (idx < 0 || idx >= MAX_DOCK_NODES) return;
    d->nodes[idx].in_use = 0;
}

static int dock_find_parent(dock_state *d, int root, int target, int *out_slot) {
    DockNode *n;
    int result;
    if (root < 0 || root >= MAX_DOCK_NODES) return -1;
    n = &d->nodes[root];
    if (!n->in_use) return -1;
    if (root == target) return -1;

    if (n->type != DOCK_TABS) {
        if (n->children[0] == target) { if (out_slot) *out_slot = 0; return root; }
        if (n->children[1] == target) { if (out_slot) *out_slot = 1; return root; }
        result = dock_find_parent(d, n->children[0], target, out_slot);
        if (result >= 0) return result;
        return dock_find_parent(d, n->children[1], target, out_slot);
    }
    return -1;
}

static void dock_free_subtree(dock_state *d, int idx) {
    DockNode *n;
    if (idx < 0 || idx >= MAX_DOCK_NODES) return;
    n = &d->nodes[idx];
    if (!n->in_use) return;

    if (n->type != DOCK_TABS) {
        dock_free_subtree(d, n->children[0]);
        dock_free_subtree(d, n->children[1]);
    }
    n->in_use = 0;
}


typedef struct editor_state {
    /* Arena pointers — set by externals init, used by editor for allocation */
    struct arena *root_arena;      /* pointer to memory.arena (for profiler arena display) */
    struct arena *editor_arena;    /* sub-arena for editor allocations */
    struct mig_header *mig_hdr;    /* migration header — persists in editor_arena */

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
    void *project_browser_clay_ctx; /* Clay_Context* in editor_arena */
    int   project_browser_cmd_count;
    void *project_browser_cmd_array;
    void *inspector_clay_ctx;      /* Clay_Context* in editor_arena */
    int   inspector_cmd_count;     /* number of Clay_RenderCommand items */
    void *inspector_cmd_array;     /* Clay_RenderCommand* in Clay arena memory */

    /* Hot-reload-persistent collapsed state */
    int profiler_tree_collapsed[PROFILER_TREE_MAX_NODES];
    int scene_tree_collapsed[SCENE_TREE_MAX_ENTITIES];
    int scene_selected_entity;
    uint8_t scene_selection_mask[SCENE_TREE_MAX_ENTITIES];
    int scene_selection_count;

    /* Profiler input — accumulated per frame by event handler, consumed by profiler_layout */
    float prof_mouse_x, prof_mouse_y; /* mouse position in profiler-local coords */
    float prof_scroll_y;              /* scroll wheel delta this frame */
    int   prof_mouse_down;            /* left button held */
    int   prof_click;                 /* 1 on the frame left button was pressed */
    int   prof_hover_record;          /* hovered flat record index, -1 = none */
    float prof_split_ratio;           /* tree/grid split: 0.1..0.9, default 0.35 */
    int   prof_split_dragging;        /* 1 while dragging the split divider */
    int   prof_split_cursor;          /* 1 when mouse is near split divider — show EW resize cursor */

    /* Scene tree input — accumulated per frame by event handler, consumed by scene_tree_layout */
    float scene_tree_mouse_x, scene_tree_mouse_y; /* mouse position in scene-tree-local coords */
    float scene_tree_scroll_y;                    /* scroll wheel delta this frame */
    int   scene_tree_mouse_down;                  /* left button held */
    int   scene_tree_click;                       /* 1 on the frame left button was pressed */
    int   scene_tree_click_shift;                 /* shift modifier sampled on click frame */
    int   scene_tree_drag_active;                 /* dragging an entity row */
    int   scene_tree_drag_entity;                 /* dragged entity index, -1 = none */
    int   scene_tree_drop_target;                 /* hovered drop target entity, -1 = none */
    int   scene_tree_drop_mode;                   /* scene-tree drop mode enum */
    float project_browser_mouse_x, project_browser_mouse_y;
    float project_browser_scroll_y;
    int   project_browser_mouse_down;
    int   project_browser_click;       /* 1 on the frame left button was pressed */
    char  pb_selected_path[256];       /* selected folder path, empty = show all */
    char  pb_selected_asset_key[64];
    char  pb_selected_asset_path[256];
    int   pb_selected_asset_type;      /* -1=none, 0=model, 1=animation, 2=dungeon, 3=sprite */
    pb_thumbnail_request pb_thumbnails[PB_MAX_THUMBNAILS];
    int   pb_thumbnail_count;

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
    float menu_scroll_y;           /* scroll wheel delta for Clay debug view */
    int   clay_debug_open;         /* 1 when Clay debug tools panel is visible */

    /* Cache profiler panel — Clay render output */
    void *cache_prof_clay_ctx;
    int   cache_prof_cmd_count;
    void *cache_prof_cmd_array;
    float cache_prof_mouse_x, cache_prof_mouse_y;
    float cache_prof_scroll_y;
    int   cache_prof_mouse_down;
    int   cache_prof_click;

    /* CPU profiler panel — Clay render output */
    void *cpu_prof_clay_ctx;
    int   cpu_prof_cmd_count;
    void *cpu_prof_cmd_array;
    float cpu_prof_mouse_x, cpu_prof_mouse_y;
    float cpu_prof_scroll_y;
    int   cpu_prof_mouse_down;
    int   cpu_prof_click;
    int   cpu_prof_timeline_paused;
    int   cpu_prof_timeline_offset;
    uint64_t cpu_prof_timeline_frame_id;
    float cpu_prof_flame_zoom;
    float cpu_prof_flame_center;
    float cpu_prof_flame_zoom_wheel;
    float cpu_prof_flame_pan_wheel;
    float cpu_prof_flame_x, cpu_prof_flame_y;
    float cpu_prof_flame_w, cpu_prof_flame_h;
    int   cpu_prof_minimap_dragging;
    float cpu_prof_minimap_drag_offset;
    struct arena *cpu_prof_text_arena;
    struct arena *prof_text_arena;       /* per-frame scratch for profiler tree text */
    uint8_t cpu_prof_tree_collapsed[EDITOR_CPU_TREE_MAX_ZONES];
    uint64_t cpu_prof_tree_frame_id;
    char  cpu_prof_hover_zone_name[64];
    int   cpu_prof_hover_zone_active;
    char  cpu_prof_selected_zone_name[64];
    int   cpu_prof_selected_zone_active;

    /* Inspector input state */
    float inspector_mouse_x, inspector_mouse_y;
    float inspector_scroll_y;
    int   inspector_mouse_down;
    int   inspector_click;

    /* Inspector field editing state */
    int   insp_edit_field;          /* INSP_FIELD_* or -1 for none */
    int   insp_edit_entity;         /* entity being edited */
    char  insp_edit_buf[64];        /* text input buffer */
    int   insp_edit_cursor;         /* cursor position in buffer */

    /* Inspector per-frame key events (consumed by inspector_layout) */
    int   insp_key_enter;
    int   insp_key_escape;
    int   insp_key_backspace;
    int   insp_key_tab;

    /* Inspector add-component dropdown */
    int   insp_add_comp_open;       /* 1 if dropdown is showing */

    /* Editor layout persistence (separate TOML from scene/project file). */
    char  editor_layout_path[512];
    uint64_t dock_layout_last_hash;
    float dock_layout_save_accum;
    int   dock_layout_hash_valid;

    /* Collaborative editing state */
    collab_state collab;

    /* Dock state — allocated from editor_arena. */
    dock_state *dock;
} editor_state;

#endif /* EDITOR_H */
