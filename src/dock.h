#ifndef DOCK_H
#define DOCK_H

#include "editor_types.h"
#include <stdint.h>
#include <string.h>

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

/* Panel display names (indexed by PanelId) */
static const char *panel_names[PANEL_COUNT] = {
    "Game",
    "Editor",
    "Profiler",
    "Scene Tree",
    "Inspector",
    "Outline",
    "Assets"
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
static int  dock_add_panel(dock_state *d, int node_idx, PanelId panel);
static int  dock_remove_panel(dock_state *d, int node_idx, PanelId panel);
static void dock_set_active_tab(dock_state *d, int node_idx, int tab);

/* Splitting */
static int  dock_split(dock_state *d, int node_idx, int horizontal);
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
    int root, left, center_right_split, center_split, game_leaf, editor_leaf, right;
    memset(d, 0, sizeof(dock_state));

    /* Root: horizontal split (Scene Tree | rest) */
    root = dock_alloc_node(d);
    d->nodes[root].type = DOCK_SPLIT_H;
    d->nodes[root].ratio = 0.19f;

    /* Left leaf: Scene Tree panel */
    left = dock_alloc_node(d);
    d->nodes[left].panels[0] = PANEL_SCENE_TREE;
    d->nodes[left].panel_count = 1;

    /* Right side: horizontal split ((Game | Editor) | Profiler/Inspector) */
    center_right_split = dock_alloc_node(d);
    d->nodes[center_right_split].type = DOCK_SPLIT_H;
    d->nodes[center_right_split].ratio = 0.78f;

    /* Center area: horizontal split (Game | Editor) */
    center_split = dock_alloc_node(d);
    d->nodes[center_split].type = DOCK_SPLIT_H;
    d->nodes[center_split].ratio = 0.5f;

    game_leaf = dock_alloc_node(d);
    d->nodes[game_leaf].panels[0] = PANEL_GAME;
    d->nodes[game_leaf].panel_count = 1;

    editor_leaf = dock_alloc_node(d);
    d->nodes[editor_leaf].panels[0] = PANEL_EDITOR;
    d->nodes[editor_leaf].panel_count = 1;

    /* Right leaf: Profiler + Inspector tabs (Inspector active by default) */
    right = dock_alloc_node(d);
    d->nodes[right].panels[0] = PANEL_PROFILER;
    d->nodes[right].panels[1] = PANEL_INSPECTOR;
    d->nodes[right].panel_count = 2;
    d->nodes[right].active_tab = 1;

    /* Wire up children */
    d->nodes[root].children[0] = left;
    d->nodes[root].children[1] = center_right_split;
    d->nodes[center_right_split].children[0] = center_split;
    d->nodes[center_right_split].children[1] = right;
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

static int dock_add_panel(dock_state *d, int node_idx, PanelId panel) {
    DockNode *n;
    if (node_idx < 0 || node_idx >= MAX_DOCK_NODES) return -1;
    n = &d->nodes[node_idx];
    if (!n->in_use || n->type != DOCK_TABS) return -1;
    if (n->panel_count >= MAX_TABS_PER_NODE) return -1;
    n->panels[n->panel_count++] = panel;
    return n->panel_count - 1;
}

static int dock_remove_panel(dock_state *d, int node_idx, PanelId panel) {
    DockNode *n;
    int i, j;
    if (node_idx < 0 || node_idx >= MAX_DOCK_NODES) return -1;
    n = &d->nodes[node_idx];
    if (!n->in_use || n->type != DOCK_TABS) return -1;

    for (i = 0; i < n->panel_count; i++) {
        if (n->panels[i] == panel) {
            for (j = i; j < n->panel_count - 1; j++)
                n->panels[j] = n->panels[j + 1];
            n->panel_count--;
            if (n->active_tab >= n->panel_count && n->panel_count > 0)
                n->active_tab = n->panel_count - 1;
            return 0;
        }
    }
    return -1;
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

static int dock_split(dock_state *d, int node_idx, int horizontal) {
    DockNode *orig;
    int split_idx, new_leaf;
    if (node_idx < 0 || node_idx >= MAX_DOCK_NODES) return -1;
    if (!d->nodes[node_idx].in_use) return -1;

    split_idx = dock_alloc_node(d);
    new_leaf = dock_alloc_node(d);
    if (split_idx < 0 || new_leaf < 0) return -1;

    orig = &d->nodes[node_idx];

    /* Copy original node to split_idx, then turn node_idx into a split */
    d->nodes[split_idx] = *orig;
    d->nodes[split_idx].in_use = 1;

    orig->type = horizontal ? DOCK_SPLIT_H : DOCK_SPLIT_V;
    orig->ratio = 0.5f;
    orig->children[0] = split_idx;
    orig->children[1] = new_leaf;
    orig->panel_count = 0;

    return new_leaf;
}

static void dock_split_at(dock_state *d, int window_idx, int node_idx,
                          PanelId panel, DropZone zone) {
    DockNode *orig;
    int split_idx, new_leaf;
    int horizontal;
    (void)window_idx;
    if (node_idx < 0 || node_idx >= MAX_DOCK_NODES) return;

    horizontal = (zone == DROP_LEFT || zone == DROP_RIGHT);

    split_idx = dock_alloc_node(d);
    new_leaf = dock_alloc_node(d);
    if (split_idx < 0 || new_leaf < 0) return;

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

static int dock_collapse_empty(dock_state *d, int idx) {
    DockNode *n;
    if (idx < 0 || idx >= MAX_DOCK_NODES) return -1;
    n = &d->nodes[idx];
    if (!n->in_use) return -1;

    if (n->type == DOCK_TABS) {
        if (n->panel_count == 0) {
            n->in_use = 0;
            return -1;
        }
        return idx;
    }

    /* Recursively collapse children */
    n->children[0] = dock_collapse_empty(d, n->children[0]);
    n->children[1] = dock_collapse_empty(d, n->children[1]);

    /* If both children gone, this node is empty */
    if (n->children[0] < 0 && n->children[1] < 0) {
        n->in_use = 0;
        return -1;
    }
    /* If one child gone, promote the other */
    if (n->children[0] < 0) {
        int survivor = n->children[1];
        n->in_use = 0;
        return survivor;
    }
    if (n->children[1] < 0) {
        int survivor = n->children[0];
        n->in_use = 0;
        return survivor;
    }
    return idx;
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

#endif /* DOCK_H */
