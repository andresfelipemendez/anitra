#include "dock.h"
#include <string.h>
#include <math.h>

/* Initialize dock state */
void dock_init(dock_state *d) {
void dock_init(dock_state *d) {
    memset(d, 0, sizeof(*d));
    
    /* Create initial window with game panel */
    d->root_nodes[0] = 0;
    d->node_count = 1;
    
    DockNode *root = &d->nodes[0];
    root->in_use = 1;
    root->type = DOCK_TABS;
    root->x = 0; root->y = 0;
    root->w = 1600; root->h = 900;
    root->panel_count = 1;
    root->panels[0] = PANEL_GAME;
    root->active_tab = 0;
    
    d->window_count = 1;
}

/* Find empty node slot */
static int find_free_node(dock_state *d) {
    for (int i = 0; i < MAX_DOCK_NODES; i++) {
        if (!d->nodes[i].in_use) return i;
    }
    return -1;
}

/* Split a node horizontally or vertically */
int dock_split(dock_state *d, int node_idx, int horizontal) {
    if (node_idx < 0 || node_idx >= MAX_DOCK_NODES) return -1;
    if (!d->nodes[node_idx].in_use) return -1;
    
    int new_node = find_free_node(d);
    if (new_node < 0) return -1;
    
    DockNode *old = &d->nodes[node_idx];
    DockNode *left = &d->nodes[new_node];
    int right_node = find_free_node(d);
    if (right_node < 0) return -1;
    DockNode *right = &d->nodes[right_node];
    
    /* Create new split node */
    left->in_use = 1;
    left->type = DOCK_SPLIT;
    left->children[0] = node_idx;
    left->children[1] = right_node;
    
    if (horizontal) {
        left->w = old->w / 2;
        left->h = old->h;
        
        right->in_use = 1;
        right->type = DOCK_SPLIT;
        right->x = old->x + left->w;
        right->y = old->y;
        right->w = old->w - left->w;
        right->h = old->h;
    } else {
        left->w = old->w;
        left->h = old->h / 2;
        
        right->in_use = 1;
        right->type = DOCK_SPLIT;
        right->x = old->x;
        right->y = old->y + left->h;
        right->w = old->w;
        right->h = old->h - left->h;
    }
    
    /* Update old node position */
    old->x = left->x;
    old->y = left->y;
    old->w = left->w;
    old->h = left->h;
    
    d->node_count = right_node + 1;
    
    return new_node;
}

/* Add panel to a tab node */
int dock_add_panel(dock_state *d, int node_idx, PanelId panel) {
    if (node_idx < 0 || node_idx >= MAX_DOCK_NODES) return -1;
    DockNode *n = &d->nodes[node_idx];
    
    if (!n->in_use || n->type != DOCK_TABS) return -1;
    if (n->panel_count >= MAX_DOCK_PANELS) return -1;
    
    n->panels[n->panel_count++] = panel;
    return n->panel_count - 1;
}

/* Remove panel from a tab node */
int dock_remove_panel(dock_state *d, int node_idx, PanelId panel) {
    if (node_idx < 0 || node_idx >= MAX_DOCK_NODES) return -1;
    DockNode *n = &d->nodes[node_idx];
    
    if (!n->in_use || n->type != DOCK_TABS) return -1;
    
    for (int i = 0; i < n->panel_count; i++) {
        if (n->panels[i] == panel) {
            /* Shift remaining panels */
            for (int j = i; j < n->panel_count - 1; j++) {
                n->panels[j] = n->panels[j + 1];
            }
            n->panel_count--;
            
            if (n->active_tab >= n->panel_count) {
                n->active_tab = n->panel_count > 0 ? n->panel_count - 1 : 0;
            }
            
            return 0;
        }
    }
    
    return -1; /* Panel not found */
}

/* Set active tab in a tab node */
void dock_set_active_tab(dock_state *d, int node_idx, int tab) {
    if (node_idx < 0 || node_idx >= MAX_DOCK_NODES) return;
    DockNode *n = &d->nodes[node_idx];
    
    if (!n->in_use || n->type != DOCK_TABS) return;
    if (tab < 0 || tab >= n->panel_count) return;
    
    n->active_tab = tab;
}

/* Get panel rect within a node */
void dock_get_panel_rect(dock_state *d, int node_idx, int panel_idx,
                         float *out_x, float *out_y, float *out_w, float *out_h) {
    if (node_idx < 0 || node_idx >= MAX_DOCK_NODES) return;
    DockNode *n = &d->nodes[node_idx];
    
    if (!n->in_use || n->type != DOCK_TABS) return;
    if (panel_idx < 0 || panel_idx >= n->panel_count) return;
    
    /* Panel content area excludes header */
    *out_x = n->x;
    *out_y = n->y + 25; /* Header height */
    *out_w = n->w;
    *out_h = n->h - 25;
}

/* Get node at screen position (for drag detection) */
int dock_hit_test(dock_state *d, int window_idx, float x, float y,
                  int *out_node, DropZone *out_zone) {
    if (window_idx < 0 || window_idx >= MAX_WINDOWS) return 0;
    if (d->root_nodes[window_idx] < 0) return 0;
    
    /* Simple recursive hit test */
    return dock_hit_test_recursive(d, d->root_nodes[window_idx], x, y, out_node, out_zone);
}

static int dock_hit_test_recursive(dock_state *d, int node_idx, float x, float y,
                                   int *out_node, DropZone *out_zone) {
    if (node_idx < 0 || node_idx >= MAX_DOCK_NODES) return 0;
    DockNode *n = &d->nodes[node_idx];
    
    if (!n->in_use) return 0;
    
    /* Check if point is in node bounds */
    if (x < n->x || x > n->x + n->w || y < n->y || y > n->y + n->h) {
        return 0;
    }
    
    *out_node = node_idx;
    
    if (n->type == DOCK_TABS) {
        /* In tab node, check for drop zones around tabs */
        float tab_w = n->w / n->panel_count;
        float header_h = 25;
        
        if (y < n->y + header_h) {
            /* On header - possible drop zone */
            *out_zone = DROP_CENTER;
            return 1;
        } else {
            /* In content area */
            *out_zone = DROP_NONE;
            return 1;
        }
    } else {
        /* Split node - check children */
        int left_result = dock_hit_test_recursive(d, n->children[0], x, y, out_node, out_zone);
        if (left_result) return 1;
        
        int right_result = dock_hit_test_recursive(d, n->children[1], x, y, out_node, out_zone);
        if (right_result) return 1;
    }
    
    return 0;
}

/* Begin drag operation */
void dock_begin_drag(dock_state *d, int window_idx, float x, float y) {
    d->drag.phase = DRAG_ACTIVE;
    d->drag.drag_window = window_idx;
    d->drag.drag_start_x = x;
    d->drag.drag_start_y = y;
}

/* Update drag operation */
void dock_update_drag(dock_state *d, float x, float y) {
    if (d->drag.phase != DRAG_ACTIVE) return;
    
    /* Find hovered node and zone */
    DropZone zone = DROP_NONE;
    int node = -1;
    
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (dock_hit_test(d, i, x, y, &node, &zone)) {
            d->drag.hover_window = i;
            d->drag.hover_node = node;
            d->drag.hover_zone = zone;
            return;
        }
    }
    
    /* No hit */
    d->drag.hover_window = -1;
    d->drag.hover_node = -1;
    d->drag.hover_zone = DROP_NONE;
}

/* End drag operation */
void dock_end_drag(dock_state *d) {
    if (d->drag.phase != DRAG_ACTIVE) return;
    
    /* Implement drop logic here */
    /* For now, just reset */
    d->drag.phase = DRAG_NONE;
    d->drag.hover_window = -1;
}
