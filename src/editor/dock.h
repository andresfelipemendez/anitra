#ifndef DOCK_H
#define DOCK_H

/* ── Docking panel system ──────────────────────────────────────────────────
   Binary tree of splits + tab groups.  Allocated from editor_arena as plain data
   (flat arrays with integer indices) so the state survives DLL hot-reload.
   ────────────────────────────────────────────────────────────────────────── */

#define MAX_DOCK_NODES    32
#define MAX_DOCK_WINDOWS   8
#define MAX_TABS_PER_NODE  8
#define DOCK_HEADER_HEIGHT 24
#define DOCK_DRAG_THRESHOLD 6  /* pixels before drag activates */

/* ── Panel identifiers ─────────────────────────────────────────────────── */

typedef enum {
    PANEL_GAME,
    PANEL_EDITOR,
    PANEL_PROFILER,
    PANEL_COUNT
} PanelId;

/* ── Dock node types ───────────────────────────────────────────────────── */

typedef enum {
    DOCK_SPLIT_H,   /* left  | right  */
    DOCK_SPLIT_V,   /* top   | bottom */
    DOCK_TABS        /* stacked panels with tab bar */
} DockNodeType;

/* ── Dock node ─────────────────────────────────────────────────────────── */

typedef struct {
    DockNodeType type;
    int in_use;

    /* SPLIT fields (type == DOCK_SPLIT_H or DOCK_SPLIT_V) */
    int   children[2];              /* indices into dock_state.nodes[] */
    float ratio;                    /* 0.0–1.0, first child's share   */

    /* TABS fields (type == DOCK_TABS) */
    PanelId panels[MAX_TABS_PER_NODE];
    int     panel_count;
    int     active_tab;

    /* Computed by dock_layout() — pixel rect, window-local */
    float x, y, w, h;
} DockNode;

/* ── Dock window ───────────────────────────────────────────────────────── */

typedef struct {
    void *sdl_window;               /* SDL_Window* */
    int   root_node;                /* index into dock_state.nodes[] */
    int   in_use;
} DockWindow;

/* ── Drag state ────────────────────────────────────────────────────────── */

typedef enum {
    DRAG_IDLE,
    DRAG_PENDING,                   /* mouse down, waiting for threshold */
    DRAG_ACTIVE                     /* dragging within/across windows    */
} DragPhase;

typedef enum {
    DROP_NONE,
    DROP_LEFT,
    DROP_RIGHT,
    DROP_TOP,
    DROP_BOTTOM,
    DROP_CENTER                     /* tab into existing group */
} DropZone;

typedef struct {
    DragPhase phase;
    PanelId   panel;
    int       source_node;          /* node index the panel came from */
    int       source_window;        /* window index */
    float     grab_x, grab_y;      /* mouse pos at mousedown */
    int       hover_node;           /* node being hovered over */
    int       hover_window;         /* window being hovered */
    DropZone  hover_zone;
} DragState;

/* ── Resize state (divider dragging) ───────────────────────────────────── */

#define DOCK_DIVIDER_HIT_HALF  4  /* pixels each side of divider line      */
#define DOCK_MIN_PANEL_SIZE   50  /* minimum child size in pixels           */

typedef struct {
    int   active;                   /* 1 = currently resizing                */
    int   node;                     /* split node being resized              */
    int   window;                   /* window index                          */
    float grab_pos;                 /* mouse pos along split axis at grab    */
    float initial_ratio;            /* ratio before drag started             */
} ResizeState;

/* ── Top-level dock state ──────────────────────────────────────────────── */

typedef struct {
    DockNode   nodes[MAX_DOCK_NODES];
    DockWindow windows[MAX_DOCK_WINDOWS];
    DragState  drag;
    ResizeState resize;
    int        initialized;

    /* Commands: set by editor.dll, processed by externals.dll each frame */
    int        cmd_tear_off;        /* 1 = tear off drag.panel                */
    float      cmd_screen_x;       /* global screen position for new window  */
    float      cmd_screen_y;
    int        cmd_redock;          /* 1 = redock drag.panel into target      */
    int        cmd_redock_target;   /* target DockWindow index                */
    int        cmd_cleanup_windows; /* 1 = destroy any empty dock windows     */
} dock_state;

/* ── Functions (implemented inline below) ──────────────────────────────── */

static int dock_alloc_node(dock_state *d);
static void dock_free_node(dock_state *d, int idx);
static void dock_init_default(dock_state *d);
static void dock_layout_node(dock_state *d, int idx, float x, float y, float w, float h);
static void dock_layout(dock_state *d, int win_idx, int win_w, int win_h);
static int dock_leaf_for_panel(dock_state *d, int node_idx, PanelId panel);
static int dock_node_at_point(dock_state *d, int node_idx, float px, float py);
static DropZone dock_drop_zone(DockNode *node, float px, float py);
static int dock_divider_at_point(dock_state *d, int node_idx, float px, float py);

/* ── Inline implementations ────────────────────────────────────────────── */

static int dock_alloc_node(dock_state *d)
{
    int i;
    for (i = 0; i < MAX_DOCK_NODES; i++) {
        if (!d->nodes[i].in_use) {
            DockNode *n = &d->nodes[i];
            n->in_use = 1;
            n->type = DOCK_TABS;
            n->children[0] = -1;
            n->children[1] = -1;
            n->ratio = 0.5f;
            n->panel_count = 0;
            n->active_tab = 0;
            n->x = n->y = n->w = n->h = 0.0f;
            return i;
        }
    }
    return -1;
}

static void dock_free_node(dock_state *d, int idx)
{
    if (idx >= 0 && idx < MAX_DOCK_NODES)
        d->nodes[idx].in_use = 0;
}

/* Build default three-column layout:  Editor(25%) | Game(50%) | Profiler(25%)
   Tree structure:
     SPLIT_H (ratio=0.25)
       left:  TABS [PANEL_EDITOR]
       right: SPLIT_H (ratio=0.667)
         left:  TABS [PANEL_GAME]
         right: TABS [PANEL_PROFILER]
*/
static void dock_init_default(dock_state *d)
{
    int root, editor_leaf, inner, game_leaf, prof_leaf;

    /* Clear everything */
    {
        int i;
        for (i = 0; i < MAX_DOCK_NODES; i++)
            d->nodes[i].in_use = 0;
        for (i = 0; i < MAX_DOCK_WINDOWS; i++)
            d->windows[i].in_use = 0;
    }
    d->drag.phase = DRAG_IDLE;
    d->resize.active = 0;

    /* Editor leaf */
    editor_leaf = dock_alloc_node(d);
    d->nodes[editor_leaf].type = DOCK_TABS;
    d->nodes[editor_leaf].panels[0] = PANEL_EDITOR;
    d->nodes[editor_leaf].panel_count = 1;
    d->nodes[editor_leaf].active_tab = 0;

    /* Game leaf */
    game_leaf = dock_alloc_node(d);
    d->nodes[game_leaf].type = DOCK_TABS;
    d->nodes[game_leaf].panels[0] = PANEL_GAME;
    d->nodes[game_leaf].panel_count = 1;
    d->nodes[game_leaf].active_tab = 0;

    /* Profiler leaf */
    prof_leaf = dock_alloc_node(d);
    d->nodes[prof_leaf].type = DOCK_TABS;
    d->nodes[prof_leaf].panels[0] = PANEL_PROFILER;
    d->nodes[prof_leaf].panel_count = 1;
    d->nodes[prof_leaf].active_tab = 0;

    /* Inner split: Game(66.7%) | Profiler(33.3%) */
    inner = dock_alloc_node(d);
    d->nodes[inner].type = DOCK_SPLIT_H;
    d->nodes[inner].children[0] = game_leaf;
    d->nodes[inner].children[1] = prof_leaf;
    d->nodes[inner].ratio = 0.667f;

    /* Root split: Editor(25%) | (Game | Profiler)(75%) */
    root = dock_alloc_node(d);
    d->nodes[root].type = DOCK_SPLIT_H;
    d->nodes[root].children[0] = editor_leaf;
    d->nodes[root].children[1] = inner;
    d->nodes[root].ratio = 0.25f;

    /* Main window — sdl_window will be set by init_externals() */
    d->windows[0].in_use = 1;
    d->windows[0].root_node = root;
    d->windows[0].sdl_window = 0;

    d->initialized = 1;
}

/* Recursively compute pixel rects for all nodes in a tree */
static void dock_layout_node(dock_state *d, int idx, float x, float y, float w, float h)
{
    DockNode *n;
    if (idx < 0 || idx >= MAX_DOCK_NODES) return;
    n = &d->nodes[idx];
    if (!n->in_use) return;

    n->x = x;
    n->y = y;
    n->w = w;
    n->h = h;

    if (n->type == DOCK_SPLIT_H) {
        float left_w = w * n->ratio;
        dock_layout_node(d, n->children[0], x, y, left_w, h);
        dock_layout_node(d, n->children[1], x + left_w, y, w - left_w, h);
    }
    else if (n->type == DOCK_SPLIT_V) {
        float top_h = h * n->ratio;
        dock_layout_node(d, n->children[0], x, y, w, top_h);
        dock_layout_node(d, n->children[1], x, y + top_h, w, h - top_h);
    }
    /* DOCK_TABS: leaf node, rect is the full area */
}

/* Compute layout for all nodes in a window */
static void dock_layout(dock_state *d, int win_idx, int win_w, int win_h)
{
    DockWindow *dw;
    if (win_idx < 0 || win_idx >= MAX_DOCK_WINDOWS) return;
    dw = &d->windows[win_idx];
    if (!dw->in_use) return;
    dock_layout_node(d, dw->root_node, 0.0f, 0.0f, (float)win_w, (float)win_h);
}

/* Find the leaf node containing a specific panel */
static int dock_leaf_for_panel(dock_state *d, int node_idx, PanelId panel)
{
    DockNode *n;
    int result;
    if (node_idx < 0 || node_idx >= MAX_DOCK_NODES) return -1;
    n = &d->nodes[node_idx];
    if (!n->in_use) return -1;

    if (n->type == DOCK_TABS) {
        int i;
        for (i = 0; i < n->panel_count; i++) {
            if (n->panels[i] == panel) return node_idx;
        }
        return -1;
    }

    result = dock_leaf_for_panel(d, n->children[0], panel);
    if (result >= 0) return result;
    return dock_leaf_for_panel(d, n->children[1], panel);
}

/* Find the deepest leaf node at a pixel position (within a tree) */
static int dock_node_at_point(dock_state *d, int node_idx, float px, float py)
{
    DockNode *n;
    int result;
    if (node_idx < 0 || node_idx >= MAX_DOCK_NODES) return -1;
    n = &d->nodes[node_idx];
    if (!n->in_use) return -1;

    /* Point outside this node? */
    if (px < n->x || px >= n->x + n->w || py < n->y || py >= n->y + n->h)
        return -1;

    if (n->type == DOCK_TABS) return node_idx;

    result = dock_node_at_point(d, n->children[0], px, py);
    if (result >= 0) return result;
    return dock_node_at_point(d, n->children[1], px, py);
}

/* Determine which drop zone the cursor is in relative to a node rect.
   Edges (30% inset) = split zones, center = tab zone. */
static DropZone dock_drop_zone(DockNode *node, float px, float py)
{
    float rx, ry, margin_x, margin_y;
    if (!node) return DROP_NONE;

    rx = px - node->x;
    ry = py - node->y;
    margin_x = node->w * 0.3f;
    margin_y = node->h * 0.3f;

    if (rx < margin_x) return DROP_LEFT;
    if (rx > node->w - margin_x) return DROP_RIGHT;
    if (ry < margin_y) return DROP_TOP;
    if (ry > node->h - margin_y) return DROP_BOTTOM;
    return DROP_CENTER;
}

/* Panel name strings (for header rendering) */
static const char *panel_names[PANEL_COUNT] = {
    "Game",
    "Scene Editor",
    "Memory Profiler"
};

/* ── Phase 2: Tree manipulation helpers ───────────────────────────────── */

/* Forward declarations */
static int  dock_find_parent(dock_state *d, int root, int child_idx, int *out_slot);
static void dock_remove_panel(dock_state *d, int node_idx, PanelId panel);
static int  dock_collapse_empty(dock_state *d, int root);
static int  dock_window_for_sdl(dock_state *d, void *sdl_win);
static int  dock_find_leaf_for_panel_global(dock_state *d, PanelId panel, int *out_win_idx);
static void dock_collect_leaves(dock_state *d, int node_idx, PanelId *out, int *count, int max_count);
static int  header_hit_test_node(dock_state *d, int node_idx, float mx, float my,
                                  int *out_node, PanelId *out_panel, int *out_tab_idx);
static void dock_split_at(dock_state *d, int win_idx, int target_node,
                           PanelId new_panel, DropZone zone);

/* Find the parent split node of a given child.
   Returns parent index, writes 0 or 1 to *out_slot.
   Returns -1 if child is root or not found. */
static int dock_find_parent(dock_state *d, int root, int child_idx, int *out_slot)
{
    DockNode *n;
    int result;
    if (root < 0 || root >= MAX_DOCK_NODES) return -1;
    n = &d->nodes[root];
    if (!n->in_use) return -1;
    if (n->type == DOCK_TABS) return -1; /* leaf can't be parent */

    if (n->children[0] == child_idx) { if (out_slot) *out_slot = 0; return root; }
    if (n->children[1] == child_idx) { if (out_slot) *out_slot = 1; return root; }

    result = dock_find_parent(d, n->children[0], child_idx, out_slot);
    if (result >= 0) return result;
    return dock_find_parent(d, n->children[1], child_idx, out_slot);
}

/* Remove a panel from a DOCK_TABS leaf. Shifts remaining panels left. */
static void dock_remove_panel(dock_state *d, int node_idx, PanelId panel)
{
    DockNode *n;
    int i, found;
    if (node_idx < 0 || node_idx >= MAX_DOCK_NODES) return;
    n = &d->nodes[node_idx];
    if (!n->in_use || n->type != DOCK_TABS) return;

    found = -1;
    for (i = 0; i < n->panel_count; i++) {
        if (n->panels[i] == panel) { found = i; break; }
    }
    if (found < 0) return;

    /* Shift panels left */
    for (i = found; i < n->panel_count - 1; i++)
        n->panels[i] = n->panels[i + 1];
    n->panel_count--;

    /* Clamp active_tab */
    if (n->active_tab >= n->panel_count && n->panel_count > 0)
        n->active_tab = n->panel_count - 1;
}

/* Collapse empty leaves after panel removal.
   If a TABS leaf has panel_count == 0, replace its parent split with the sibling.
   Returns the (possibly new) root index. */
static int dock_collapse_empty(dock_state *d, int root)
{
    DockNode *n;
    int changed;
    if (root < 0 || root >= MAX_DOCK_NODES) return root;
    n = &d->nodes[root];
    if (!n->in_use) return root;

    /* If root itself is an empty leaf, just free it and return -1 */
    if (n->type == DOCK_TABS && n->panel_count == 0) {
        dock_free_node(d, root);
        return -1;
    }

    /* Recursively collapse children first */
    if (n->type != DOCK_TABS) {
        n->children[0] = dock_collapse_empty(d, n->children[0]);
        n->children[1] = dock_collapse_empty(d, n->children[1]);
    }

    /* Now check if this split has an empty child — promote the sibling */
    changed = 1;
    while (changed) {
        changed = 0;
        n = &d->nodes[root];
        if (!n->in_use || n->type == DOCK_TABS) break;

        if (n->children[0] < 0) {
            /* Left child gone, promote right */
            int sibling = n->children[1];
            dock_free_node(d, root);
            root = sibling;
            changed = 1;
        } else if (n->children[1] < 0) {
            /* Right child gone, promote left */
            int sibling = n->children[0];
            dock_free_node(d, root);
            root = sibling;
            changed = 1;
        }
    }

    return root;
}

/* Find which DockWindow index owns a given SDL_Window pointer */
static int dock_window_for_sdl(dock_state *d, void *sdl_win)
{
    int i;
    for (i = 0; i < MAX_DOCK_WINDOWS; i++) {
        if (d->windows[i].in_use && d->windows[i].sdl_window == sdl_win)
            return i;
    }
    return -1;
}

/* Search ALL windows for a panel. Returns the leaf node index,
   writes the window index to *out_win_idx. Returns -1 if not found. */
static int dock_find_leaf_for_panel_global(dock_state *d, PanelId panel, int *out_win_idx)
{
    int i, leaf;
    for (i = 0; i < MAX_DOCK_WINDOWS; i++) {
        if (!d->windows[i].in_use) continue;
        leaf = dock_leaf_for_panel(d, d->windows[i].root_node, panel);
        if (leaf >= 0) {
            if (out_win_idx) *out_win_idx = i;
            return leaf;
        }
    }
    if (out_win_idx) *out_win_idx = -1;
    return -1;
}

/* Collect all panels from leaves reachable from a subtree root.
   Used when closing a tear-off window to recover its panels. */
static void dock_collect_leaves(dock_state *d, int node_idx, PanelId *out, int *count, int max_count)
{
    DockNode *n;
    int i;
    if (node_idx < 0 || node_idx >= MAX_DOCK_NODES) return;
    n = &d->nodes[node_idx];
    if (!n->in_use) return;

    if (n->type == DOCK_TABS) {
        for (i = 0; i < n->panel_count && *count < max_count; i++) {
            out[*count] = n->panels[i];
            (*count)++;
        }
        return;
    }

    dock_collect_leaves(d, n->children[0], out, count, max_count);
    dock_collect_leaves(d, n->children[1], out, count, max_count);
}

/* Free all nodes in a subtree (used when destroying a tear-off window) */
static void dock_free_subtree(dock_state *d, int node_idx)
{
    DockNode *n;
    if (node_idx < 0 || node_idx >= MAX_DOCK_NODES) return;
    n = &d->nodes[node_idx];
    if (!n->in_use) return;

    if (n->type != DOCK_TABS) {
        dock_free_subtree(d, n->children[0]);
        dock_free_subtree(d, n->children[1]);
    }
    dock_free_node(d, node_idx);
}

/* Hit-test: check if (mx,my) in window-local coords is on a tab header.
   Walks DOCK_TABS leaves reachable from node_idx. Returns 1 on hit.
   When multiple tabs exist, determines which specific tab was clicked
   based on horizontal position (equal-width tabs). */
static int header_hit_test_node(dock_state *d, int node_idx, float mx, float my,
                                 int *out_node, PanelId *out_panel, int *out_tab_idx)
{
    DockNode *n;
    if (node_idx < 0 || node_idx >= MAX_DOCK_NODES) return 0;
    n = &d->nodes[node_idx];
    if (!n->in_use) return 0;

    if (n->type == DOCK_TABS) {
        if (n->panel_count > 0 &&
            mx >= n->x && mx < n->x + n->w &&
            my >= n->y && my < n->y + DOCK_HEADER_HEIGHT) {
            /* Determine which tab was hit */
            float tab_w = n->w / (float)n->panel_count;
            int tab_idx = (int)((mx - n->x) / tab_w);
            if (tab_idx >= n->panel_count) tab_idx = n->panel_count - 1;
            if (tab_idx < 0) tab_idx = 0;

            if (out_node) *out_node = node_idx;
            if (out_panel) *out_panel = n->panels[tab_idx];
            if (out_tab_idx) *out_tab_idx = tab_idx;
            return 1;
        }
        return 0;
    }

    if (header_hit_test_node(d, n->children[0], mx, my, out_node, out_panel, out_tab_idx)) return 1;
    return header_hit_test_node(d, n->children[1], mx, my, out_node, out_panel, out_tab_idx);
}

/* Find the split node whose divider line is under the cursor.
   Returns node index of the SPLIT_H or SPLIT_V node, or -1 if none.
   Checks deepest (most nested) splits first so inner dividers win over outer. */
static int dock_divider_at_point(dock_state *d, int node_idx, float px, float py)
{
    DockNode *n;
    int result;
    if (node_idx < 0 || node_idx >= MAX_DOCK_NODES) return -1;
    n = &d->nodes[node_idx];
    if (!n->in_use) return -1;

    /* Only split nodes have dividers */
    if (n->type == DOCK_TABS) return -1;

    /* Check children first (deeper dividers take priority) */
    result = dock_divider_at_point(d, n->children[0], px, py);
    if (result >= 0) return result;
    result = dock_divider_at_point(d, n->children[1], px, py);
    if (result >= 0) return result;

    /* Check this node's divider */
    if (px < n->x || px >= n->x + n->w || py < n->y || py >= n->y + n->h)
        return -1; /* mouse outside this node entirely */

    if (n->type == DOCK_SPLIT_H) {
        float div_x = n->x + n->w * n->ratio;
        if (px >= div_x - DOCK_DIVIDER_HIT_HALF && px <= div_x + DOCK_DIVIDER_HIT_HALF)
            return node_idx;
    } else { /* DOCK_SPLIT_V */
        float div_y = n->y + n->h * n->ratio;
        if (py >= div_y - DOCK_DIVIDER_HIT_HALF && py <= div_y + DOCK_DIVIDER_HIT_HALF)
            return node_idx;
    }

    return -1;
}

/* Split a leaf node by inserting a new panel beside or above/below it.
   Creates a new SPLIT node that replaces target_node in the tree.
   zone must be DROP_LEFT, DROP_RIGHT, DROP_TOP, or DROP_BOTTOM.
   For DROP_CENTER, use direct tab insertion instead. */
static void dock_split_at(dock_state *d, int win_idx, int target_node,
                           PanelId new_panel, DropZone zone)
{
    int new_leaf, split_node, parent, slot;
    DockNode *sn;

    if (zone == DROP_NONE || zone == DROP_CENTER) return;
    if (target_node < 0 || target_node >= MAX_DOCK_NODES) return;
    if (!d->nodes[target_node].in_use) return;

    /* 1. Allocate a new leaf for the dropped panel */
    new_leaf = dock_alloc_node(d);
    if (new_leaf < 0) return;
    d->nodes[new_leaf].type = DOCK_TABS;
    d->nodes[new_leaf].panels[0] = new_panel;
    d->nodes[new_leaf].panel_count = 1;
    d->nodes[new_leaf].active_tab = 0;

    /* 2. Allocate a new split node */
    split_node = dock_alloc_node(d);
    if (split_node < 0) { dock_free_node(d, new_leaf); return; }
    sn = &d->nodes[split_node];
    sn->type = (zone == DROP_LEFT || zone == DROP_RIGHT) ? DOCK_SPLIT_H : DOCK_SPLIT_V;
    sn->ratio = 0.5f;

    /* 3. Set children: new panel goes on the side the user dropped */
    if (zone == DROP_LEFT || zone == DROP_TOP) {
        sn->children[0] = new_leaf;     /* new panel first  */
        sn->children[1] = target_node;  /* existing second  */
    } else {
        sn->children[0] = target_node;  /* existing first   */
        sn->children[1] = new_leaf;     /* new panel second */
    }

    /* 4. Replace target_node with split_node in the parent (or root) */
    parent = dock_find_parent(d, d->windows[win_idx].root_node, target_node, &slot);
    if (parent >= 0) {
        d->nodes[parent].children[slot] = split_node;
    } else {
        /* target_node was the root — update window's root */
        d->windows[win_idx].root_node = split_node;
    }
}

#endif /* DOCK_H */
