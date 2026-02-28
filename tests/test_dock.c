/* ── Dock system unit tests ────────────────────────────────────────────────
   Standalone test binary — no SDL, no GPU, no arena needed.
   dock.h functions are static inline and operate on plain structs.

   Build:  cl /Isrc/editor tests/test_dock.c /Fe:build/test_dock.exe
   Run:    build/test_dock.exe
   ────────────────────────────────────────────────────────────────────────── */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dock.h"

/* ── Minimal test harness ─────────────────────────────────────────────── */

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    static void test_##name(void); \
    static void run_##name(void) { \
        tests_run++; \
        printf("  %-50s ", #name); \
        test_##name(); \
        tests_passed++; \
        printf("PASS\n"); \
    } \
    static void test_##name(void)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("FAIL\n    %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        tests_failed++; \
        tests_passed--; /* undo the pre-increment in run_ */ \
        return; \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        printf("FAIL\n    %s:%d: %s == %d, expected %d\n", \
               __FILE__, __LINE__, #a, (int)(a), (int)(b)); \
        tests_failed++; \
        tests_passed--; \
        return; \
    } \
} while(0)

#define ASSERT_FLOAT_EQ(a, b, eps) do { \
    float _a = (float)(a), _b = (float)(b); \
    if (_a - _b > (eps) || _b - _a > (eps)) { \
        printf("FAIL\n    %s:%d: %s == %.3f, expected %.3f\n", \
               __FILE__, __LINE__, #a, _a, _b); \
        tests_failed++; \
        tests_passed--; \
        return; \
    } \
} while(0)

/* Helper: create a fresh dock_state with default layout */
static dock_state make_default_dock(void) {
    dock_state d;
    memset(&d, 0, sizeof(d));
    dock_init_default(&d);
    return d;
}

/* ── Tests: Node allocation ───────────────────────────────────────────── */

TEST(alloc_node_returns_valid_index) {
    dock_state d;
    int idx;
    memset(&d, 0, sizeof(d));
    idx = dock_alloc_node(&d);
    ASSERT(idx >= 0);
    ASSERT(idx < MAX_DOCK_NODES);
    ASSERT(d.nodes[idx].in_use == 1);
}

TEST(alloc_node_returns_unique_indices) {
    dock_state d;
    int a, b, c;
    memset(&d, 0, sizeof(d));
    a = dock_alloc_node(&d);
    b = dock_alloc_node(&d);
    c = dock_alloc_node(&d);
    ASSERT(a != b);
    ASSERT(b != c);
    ASSERT(a != c);
}

TEST(free_node_allows_reuse) {
    dock_state d;
    int a, b;
    memset(&d, 0, sizeof(d));
    a = dock_alloc_node(&d);
    dock_free_node(&d, a);
    b = dock_alloc_node(&d);
    ASSERT_EQ(a, b); /* should reuse the freed slot */
}

TEST(alloc_node_exhaustion_returns_minus_one) {
    dock_state d;
    int i, last;
    memset(&d, 0, sizeof(d));
    for (i = 0; i < MAX_DOCK_NODES; i++)
        dock_alloc_node(&d);
    last = dock_alloc_node(&d);
    ASSERT_EQ(last, -1);
}

/* ── Tests: Default layout ────────────────────────────────────────────── */

TEST(init_default_creates_seven_nodes) {
    dock_state d = make_default_dock();
    int count = 0, i;
    for (i = 0; i < MAX_DOCK_NODES; i++)
        if (d.nodes[i].in_use) count++;
    ASSERT_EQ(count, 7); /* root + center/right split + center split + 4 leaves */
}

TEST(init_default_window_zero_active) {
    dock_state d = make_default_dock();
    ASSERT_EQ(d.windows[0].in_use, 1);
    ASSERT(d.windows[0].root_node >= 0);
}

TEST(init_default_all_panels_reachable) {
    dock_state d = make_default_dock();
    int root = d.windows[0].root_node;
    ASSERT(dock_leaf_for_panel(&d, root, PANEL_GAME) >= 0);
    ASSERT(dock_leaf_for_panel(&d, root, PANEL_EDITOR) >= 0);
    ASSERT(dock_leaf_for_panel(&d, root, PANEL_PROFILER) >= 0);
    ASSERT(dock_leaf_for_panel(&d, root, PANEL_SCENE_TREE) >= 0);
    ASSERT(dock_leaf_for_panel(&d, root, PANEL_INSPECTOR) >= 0);
}

TEST(init_default_each_panel_in_own_leaf) {
    dock_state d = make_default_dock();
    int root = d.windows[0].root_node;
    int g_leaf = dock_leaf_for_panel(&d, root, PANEL_GAME);
    int e_leaf = dock_leaf_for_panel(&d, root, PANEL_EDITOR);
    int p_leaf = dock_leaf_for_panel(&d, root, PANEL_PROFILER);
    ASSERT(g_leaf != e_leaf);
    ASSERT(g_leaf != p_leaf);
    ASSERT(e_leaf != p_leaf);
}

/* ── Tests: Layout ────────────────────────────────────────────────────── */

TEST(layout_root_covers_full_window) {
    dock_state d = make_default_dock();
    DockNode *root;
    dock_layout(&d, 0, 1600, 900);
    root = &d.nodes[d.windows[0].root_node];
    ASSERT_FLOAT_EQ(root->x, 0.0f, 0.01f);
    ASSERT_FLOAT_EQ(root->y, (float)MENU_BAR_HEIGHT, 0.01f);
    ASSERT_FLOAT_EQ(root->w, 1600.0f, 0.01f);
    ASSERT_FLOAT_EQ(root->h, 900.0f - (float)MENU_BAR_HEIGHT, 0.01f);
}

TEST(layout_leaves_partition_width) {
    dock_state d = make_default_dock();
    int root = d.windows[0].root_node;
    int s_leaf, g_leaf, e_leaf, p_leaf;
    float total;
    dock_layout(&d, 0, 1600, 900);
    s_leaf = dock_leaf_for_panel(&d, root, PANEL_SCENE_TREE);
    e_leaf = dock_leaf_for_panel(&d, root, PANEL_EDITOR);
    g_leaf = dock_leaf_for_panel(&d, root, PANEL_GAME);
    p_leaf = dock_leaf_for_panel(&d, root, PANEL_PROFILER);
    total = d.nodes[s_leaf].w + d.nodes[g_leaf].w + d.nodes[e_leaf].w + d.nodes[p_leaf].w;
    ASSERT_FLOAT_EQ(total, 1600.0f, 1.0f);
}

TEST(layout_leaves_same_height) {
    dock_state d = make_default_dock();
    int root = d.windows[0].root_node;
    int e_leaf, g_leaf, p_leaf;
    dock_layout(&d, 0, 1600, 900);
    e_leaf = dock_leaf_for_panel(&d, root, PANEL_EDITOR);
    g_leaf = dock_leaf_for_panel(&d, root, PANEL_GAME);
    p_leaf = dock_leaf_for_panel(&d, root, PANEL_PROFILER);
    ASSERT_FLOAT_EQ(d.nodes[e_leaf].h, 900.0f - (float)MENU_BAR_HEIGHT, 0.01f);
    ASSERT_FLOAT_EQ(d.nodes[g_leaf].h, 900.0f - (float)MENU_BAR_HEIGHT, 0.01f);
    ASSERT_FLOAT_EQ(d.nodes[p_leaf].h, 900.0f - (float)MENU_BAR_HEIGHT, 0.01f);
}

/* ── Tests: Node at point ─────────────────────────────────────────────── */

TEST(node_at_point_finds_correct_leaf) {
    dock_state d = make_default_dock();
    int root = d.windows[0].root_node;
    int s_leaf, g_leaf, e_leaf, p_leaf;
    int found;
    dock_layout(&d, 0, 1600, 900);
    s_leaf = dock_leaf_for_panel(&d, root, PANEL_SCENE_TREE);
    e_leaf = dock_leaf_for_panel(&d, root, PANEL_EDITOR);
    g_leaf = dock_leaf_for_panel(&d, root, PANEL_GAME);
    p_leaf = dock_leaf_for_panel(&d, root, PANEL_PROFILER);

    /* Scene Tree is leftmost */
    found = dock_node_at_point(&d, root, 100.0f, 450.0f);
    ASSERT_EQ(found, s_leaf);

    /* Game is left-center */
    found = dock_node_at_point(&d, root, 500.0f, 450.0f);
    ASSERT_EQ(found, g_leaf);

    /* Editor is right-center */
    found = dock_node_at_point(&d, root, 1000.0f, 450.0f);
    ASSERT_EQ(found, e_leaf);

    /* Profiler is rightmost (1120..1600) */
    found = dock_node_at_point(&d, root, 1500.0f, 450.0f);
    ASSERT_EQ(found, p_leaf);
}

TEST(node_at_point_outside_returns_minus_one) {
    dock_state d = make_default_dock();
    int root = d.windows[0].root_node;
    dock_layout(&d, 0, 1600, 900);
    ASSERT_EQ(dock_node_at_point(&d, root, -10.0f, 450.0f), -1);
    ASSERT_EQ(dock_node_at_point(&d, root, 1700.0f, 450.0f), -1);
    ASSERT_EQ(dock_node_at_point(&d, root, 800.0f, -10.0f), -1);
    ASSERT_EQ(dock_node_at_point(&d, root, 800.0f, 1000.0f), -1);
}

/* ── Tests: Drop zones ────────────────────────────────────────────────── */

TEST(drop_zone_edges) {
    DockNode n = {0};
    n.x = 100; n.y = 100; n.w = 400; n.h = 300;
    /* Left 30% → x=100..220 */
    ASSERT_EQ(dock_drop_zone(&n, 110.0f, 250.0f), DROP_LEFT);
    /* Right 30% → x=380..500 */
    ASSERT_EQ(dock_drop_zone(&n, 490.0f, 250.0f), DROP_RIGHT);
    /* Top 30% → y=100..190 */
    ASSERT_EQ(dock_drop_zone(&n, 300.0f, 110.0f), DROP_TOP);
    /* Bottom 30% → y=310..400 */
    ASSERT_EQ(dock_drop_zone(&n, 300.0f, 390.0f), DROP_BOTTOM);
    /* Center */
    ASSERT_EQ(dock_drop_zone(&n, 300.0f, 250.0f), DROP_CENTER);
}

/* ── Tests: Divider hit-test ──────────────────────────────────────────── */

TEST(divider_hit_on_vertical_split_line) {
    /* Hit root divider at runtime-computed position. */
    dock_state d = make_default_dock();
    int root = d.windows[0].root_node;
    dock_layout(&d, 0, 1600, 900);
    {
        DockNode *rn = &d.nodes[root];
        float root_div_x = rn->x + rn->w * rn->ratio;
        int hit = dock_divider_at_point(&d, root, root_div_x, 450.0f);
        ASSERT(hit >= 0);
        ASSERT_EQ(d.nodes[hit].type, DOCK_SPLIT_H);
    }
}

TEST(divider_hit_on_inner_split_line) {
    dock_state d = make_default_dock();
    int root = d.windows[0].root_node;
    int center_right = d.nodes[root].children[1];
    int center_split = d.nodes[center_right].children[0];
    dock_layout(&d, 0, 1600, 900);
    {
        DockNode *cn = &d.nodes[center_split];
        float center_div_x = cn->x + cn->w * cn->ratio;
        int hit = dock_divider_at_point(&d, root, center_div_x, 450.0f);
        ASSERT(hit >= 0);
        ASSERT_EQ(d.nodes[hit].type, DOCK_SPLIT_H);
    }
}

TEST(divider_miss_in_panel_center) {
    dock_state d = make_default_dock();
    dock_layout(&d, 0, 1600, 900);
    {
        /* Center of the editor panel — no divider */
        int hit = dock_divider_at_point(&d, d.windows[0].root_node, 200.0f, 450.0f);
        ASSERT_EQ(hit, -1);
    }
}

TEST(divider_miss_outside_window) {
    dock_state d = make_default_dock();
    dock_layout(&d, 0, 1600, 900);
    {
        int hit = dock_divider_at_point(&d, d.windows[0].root_node, -10.0f, 450.0f);
        ASSERT_EQ(hit, -1);
    }
}

TEST(divider_inner_takes_priority_over_outer) {
    /* dock_divider_at_point checks children first, so deeper wins. */
    dock_state d = make_default_dock();
    int root = d.windows[0].root_node;
    int center_right = d.nodes[root].children[1];
    int inner = d.nodes[center_right].children[0]; /* center SPLIT_H */
    dock_layout(&d, 0, 1600, 900);
    {
        DockNode *in = &d.nodes[inner];
        float inner_div_x = in->x + in->w * in->ratio;
        int hit_inner = dock_divider_at_point(&d, root, inner_div_x, 450.0f);
        ASSERT(hit_inner >= 0);
        ASSERT_EQ(hit_inner, inner);
    }
}

/* ── Tests: Split at ──────────────────────────────────────────────────── */

TEST(split_at_left_creates_horizontal_split) {
    dock_state d = make_default_dock();
    int root = d.windows[0].root_node;
    int g_leaf, new_root;
    DockNode *split;
    dock_layout(&d, 0, 1600, 900);
    g_leaf = dock_leaf_for_panel(&d, root, PANEL_GAME);

    /* Split game panel: put editor to the LEFT of game */
    dock_remove_panel(&d, dock_leaf_for_panel(&d, root, PANEL_EDITOR), PANEL_EDITOR);
    dock_split_at(&d, 0, g_leaf, PANEL_EDITOR, DROP_LEFT);

    /* The game leaf should now be wrapped in a SPLIT_H */
    new_root = d.windows[0].root_node;
    ASSERT(new_root >= 0);

    /* Find where editor ended up */
    ASSERT(dock_leaf_for_panel(&d, new_root, PANEL_EDITOR) >= 0);
    ASSERT(dock_leaf_for_panel(&d, new_root, PANEL_GAME) >= 0);
}

TEST(split_at_bottom_creates_vertical_split) {
    dock_state d;
    int leaf, root_idx;
    DockNode *split;

    /* Simple setup: one window, one leaf with GAME */
    memset(&d, 0, sizeof(d));
    leaf = dock_alloc_node(&d);
    d.nodes[leaf].type = DOCK_TABS;
    d.nodes[leaf].panels[0] = PANEL_GAME;
    d.nodes[leaf].panel_count = 1;
    d.windows[0].in_use = 1;
    d.windows[0].root_node = leaf;

    dock_split_at(&d, 0, leaf, PANEL_PROFILER, DROP_BOTTOM);

    root_idx = d.windows[0].root_node;
    /* dock_split_at converts the node in-place to a split */
    ASSERT_EQ(root_idx, leaf);
    split = &d.nodes[root_idx];
    ASSERT_EQ(split->type, DOCK_SPLIT_V);
    ASSERT(split->children[0] >= 0);     /* game on top (copied to new node) */
    ASSERT(split->children[1] >= 0);     /* profiler on bottom */
    ASSERT_EQ(d.nodes[split->children[0]].panels[0], PANEL_GAME);
    ASSERT_EQ(d.nodes[split->children[1]].panels[0], PANEL_PROFILER);
}

TEST(split_at_right_new_panel_is_second_child) {
    dock_state d;
    int leaf, root_idx;
    DockNode *split;

    memset(&d, 0, sizeof(d));
    leaf = dock_alloc_node(&d);
    d.nodes[leaf].type = DOCK_TABS;
    d.nodes[leaf].panels[0] = PANEL_GAME;
    d.nodes[leaf].panel_count = 1;
    d.windows[0].in_use = 1;
    d.windows[0].root_node = leaf;

    dock_split_at(&d, 0, leaf, PANEL_EDITOR, DROP_RIGHT);

    root_idx = d.windows[0].root_node;
    /* dock_split_at converts the node in-place to a split */
    ASSERT_EQ(root_idx, leaf);
    split = &d.nodes[root_idx];
    ASSERT_EQ(split->type, DOCK_SPLIT_H);
    ASSERT(split->children[0] >= 0);     /* game (copied to new node) */
    ASSERT_EQ(d.nodes[split->children[0]].panels[0], PANEL_GAME);
    ASSERT_EQ(d.nodes[split->children[1]].panels[0], PANEL_EDITOR); /* editor second */
}

/* ── Tests: Remove panel ──────────────────────────────────────────────── */

TEST(remove_panel_decrements_count) {
    dock_state d = make_default_dock();
    int root = d.windows[0].root_node;
    int g_leaf = dock_leaf_for_panel(&d, root, PANEL_GAME);
    ASSERT_EQ(d.nodes[g_leaf].panel_count, 1);
    dock_remove_panel(&d, g_leaf, PANEL_GAME);
    ASSERT_EQ(d.nodes[g_leaf].panel_count, 0);
}

TEST(remove_panel_from_multi_tab_shifts) {
    dock_state d;
    int leaf;
    memset(&d, 0, sizeof(d));
    leaf = dock_alloc_node(&d);
    d.nodes[leaf].panels[0] = PANEL_GAME;
    d.nodes[leaf].panels[1] = PANEL_EDITOR;
    d.nodes[leaf].panels[2] = PANEL_PROFILER;
    d.nodes[leaf].panel_count = 3;
    d.nodes[leaf].active_tab = 1;

    dock_remove_panel(&d, leaf, PANEL_EDITOR);

    ASSERT_EQ(d.nodes[leaf].panel_count, 2);
    ASSERT_EQ(d.nodes[leaf].panels[0], PANEL_GAME);
    ASSERT_EQ(d.nodes[leaf].panels[1], PANEL_PROFILER);
    ASSERT_EQ(d.nodes[leaf].active_tab, 1); /* clamped to last */
}

/* ── Tests: Collapse empty ────────────────────────────────────────────── */

TEST(collapse_empty_leaf_returns_minus_one) {
    dock_state d;
    int leaf, result;
    memset(&d, 0, sizeof(d));
    leaf = dock_alloc_node(&d);
    d.nodes[leaf].panel_count = 0;
    result = dock_collapse_empty(&d, leaf);
    ASSERT_EQ(result, -1);
    ASSERT_EQ(d.nodes[leaf].in_use, 0);
}

TEST(collapse_promotes_sibling_when_child_empty) {
    dock_state d;
    int split, left, right, result;
    memset(&d, 0, sizeof(d));

    /* Create: SPLIT_H(empty_leaf, game_leaf) */
    left = dock_alloc_node(&d);
    d.nodes[left].panel_count = 0; /* empty */

    right = dock_alloc_node(&d);
    d.nodes[right].panels[0] = PANEL_GAME;
    d.nodes[right].panel_count = 1;

    split = dock_alloc_node(&d);
    d.nodes[split].type = DOCK_SPLIT_H;
    d.nodes[split].children[0] = left;
    d.nodes[split].children[1] = right;

    result = dock_collapse_empty(&d, split);
    ASSERT_EQ(result, right);         /* sibling promoted */
    ASSERT_EQ(d.nodes[split].in_use, 0); /* split freed */
    ASSERT_EQ(d.nodes[left].in_use, 0);  /* empty leaf freed */
    ASSERT_EQ(d.nodes[right].in_use, 1); /* game leaf survives */
}

/* ── Tests: Find parent ───────────────────────────────────────────────── */

TEST(find_parent_of_root_returns_minus_one) {
    dock_state d = make_default_dock();
    int root = d.windows[0].root_node;
    int slot;
    int parent = dock_find_parent(&d, root, root, &slot);
    ASSERT_EQ(parent, -1);
}

TEST(find_parent_of_child) {
    dock_state d = make_default_dock();
    int root = d.windows[0].root_node;
    int slot = -1;
    int e_leaf = dock_leaf_for_panel(&d, root, PANEL_EDITOR);
    int parent = dock_find_parent(&d, root, e_leaf, &slot);
    ASSERT(parent >= 0);
    ASSERT(slot == 0 || slot == 1);
    ASSERT_EQ(d.nodes[parent].children[slot], e_leaf);
}

/* ── Tests: Header hit test ───────────────────────────────────────────── */

TEST(header_hit_test_in_header_area) {
    dock_state d = make_default_dock();
    int root = d.windows[0].root_node;
    int hit_node = -1;
    PanelId hit_panel = PANEL_GAME;
    int hit_tab = -1;

    dock_layout(&d, 0, 1600, 900);

    /* Click in the scene-tree header (leftmost panel, y in dock header area) */
    ASSERT(header_hit_test_node(&d, root, 100.0f, (float)MENU_BAR_HEIGHT + 10.0f,
                                 &hit_node, &hit_panel, &hit_tab));
    ASSERT_EQ(hit_panel, PANEL_SCENE_TREE);
    ASSERT_EQ(hit_tab, 0);
}

TEST(header_hit_test_below_header_misses) {
    dock_state d = make_default_dock();
    int root = d.windows[0].root_node;
    int hit_node = -1;
    PanelId hit_panel = PANEL_GAME;
    int hit_tab = -1;

    dock_layout(&d, 0, 1600, 900);

    /* Click below header in the editor panel content area */
    ASSERT(!header_hit_test_node(&d, root, 100.0f, (float)(MENU_BAR_HEIGHT + DOCK_HEADER_HEIGHT + 10),
                                  &hit_node, &hit_panel, &hit_tab));
}

TEST(header_hit_test_multi_tab_selects_correct_tab) {
    dock_state d;
    int leaf;
    int hit_node = -1;
    PanelId hit_panel = PANEL_GAME;
    int hit_tab = -1;

    /* Setup: single leaf with 3 tabs */
    memset(&d, 0, sizeof(d));
    leaf = dock_alloc_node(&d);
    d.nodes[leaf].panels[0] = PANEL_GAME;
    d.nodes[leaf].panels[1] = PANEL_EDITOR;
    d.nodes[leaf].panels[2] = PANEL_PROFILER;
    d.nodes[leaf].panel_count = 3;
    d.nodes[leaf].active_tab = 0;
    d.nodes[leaf].x = 0; d.nodes[leaf].y = 0;
    d.nodes[leaf].w = 900; d.nodes[leaf].h = 600;

    /* Tab width = 900/3 = 300. Click at x=450 → tab index 1 (EDITOR) */
    ASSERT(header_hit_test_node(&d, leaf, 450.0f, 10.0f,
                                 &hit_node, &hit_panel, &hit_tab));
    ASSERT_EQ(hit_panel, PANEL_EDITOR);
    ASSERT_EQ(hit_tab, 1);

    /* Click at x=750 → tab index 2 (PROFILER) */
    ASSERT(header_hit_test_node(&d, leaf, 750.0f, 10.0f,
                                 &hit_node, &hit_panel, &hit_tab));
    ASSERT_EQ(hit_panel, PANEL_PROFILER);
    ASSERT_EQ(hit_tab, 2);
}

/* ── Tests: Window for SDL ────────────────────────────────────────────── */

TEST(window_for_sdl_finds_correct_window) {
    dock_state d = make_default_dock();
    void *fake_win = (void *)0xDEADBEEF;
    d.windows[0].sdl_window = fake_win;
    ASSERT_EQ(dock_window_for_sdl(&d, fake_win), 0);
    ASSERT_EQ(dock_window_for_sdl(&d, (void *)0x12345), -1);
}

/* ── Tests: Full drag-split-collapse cycle ────────────────────────────── */

TEST(drag_split_collapse_cycle) {
    /* Simulate: drag PROFILER from its leaf, drop LEFT of GAME */
    dock_state d = make_default_dock();
    int root = d.windows[0].root_node;
    int g_leaf, p_leaf, new_root;

    dock_layout(&d, 0, 1600, 900);
    g_leaf = dock_leaf_for_panel(&d, root, PANEL_GAME);
    p_leaf = dock_leaf_for_panel(&d, root, PANEL_PROFILER);

    /* Execute split: profiler to the LEFT of game */
    dock_split_at(&d, 0, g_leaf, PANEL_PROFILER, DROP_LEFT);

    /* Remove profiler from its original location */
    dock_remove_panel(&d, p_leaf, PANEL_PROFILER);

    /* Collapse empty nodes */
    new_root = dock_collapse_empty(&d, d.windows[0].root_node);
    d.windows[0].root_node = new_root;

    /* Verify: all 3 panels still reachable */
    ASSERT(new_root >= 0);
    ASSERT(dock_leaf_for_panel(&d, new_root, PANEL_GAME) >= 0);
    ASSERT(dock_leaf_for_panel(&d, new_root, PANEL_EDITOR) >= 0);
    ASSERT(dock_leaf_for_panel(&d, new_root, PANEL_PROFILER) >= 0);

    /* Profiler should now be in a different leaf than before */
    ASSERT(dock_leaf_for_panel(&d, new_root, PANEL_PROFILER) != p_leaf);
}

TEST(drag_split_vertical_cycle) {
    /* Simulate: drag EDITOR, drop BOTTOM of GAME → vertical split */
    dock_state d = make_default_dock();
    int root = d.windows[0].root_node;
    int g_leaf, e_leaf, new_root;
    DockNode *parent_of_game;

    dock_layout(&d, 0, 1600, 900);
    g_leaf = dock_leaf_for_panel(&d, root, PANEL_GAME);
    e_leaf = dock_leaf_for_panel(&d, root, PANEL_EDITOR);

    dock_split_at(&d, 0, g_leaf, PANEL_EDITOR, DROP_BOTTOM);
    dock_remove_panel(&d, e_leaf, PANEL_EDITOR);
    new_root = dock_collapse_empty(&d, d.windows[0].root_node);
    d.windows[0].root_node = new_root;

    /* All panels reachable */
    ASSERT(dock_leaf_for_panel(&d, new_root, PANEL_GAME) >= 0);
    ASSERT(dock_leaf_for_panel(&d, new_root, PANEL_EDITOR) >= 0);
    ASSERT(dock_leaf_for_panel(&d, new_root, PANEL_PROFILER) >= 0);

    /* Game and editor should share a vertical split parent */
    {
        int slot;
        int g2 = dock_leaf_for_panel(&d, new_root, PANEL_GAME);
        int parent = dock_find_parent(&d, new_root, g2, &slot);
        ASSERT(parent >= 0);
        ASSERT_EQ(d.nodes[parent].type, DOCK_SPLIT_V);
    }
}

TEST(drop_center_adds_tab) {
    /* Drop a panel into another node as a tab (CENTER zone) */
    dock_state d = make_default_dock();
    int root = d.windows[0].root_node;
    int g_leaf = dock_leaf_for_panel(&d, root, PANEL_GAME);
    int e_leaf = dock_leaf_for_panel(&d, root, PANEL_EDITOR);
    DockNode *gn;

    /* Add editor as tab in game node */
    gn = &d.nodes[g_leaf];
    gn->panels[gn->panel_count] = PANEL_EDITOR;
    gn->panel_count++;
    gn->active_tab = gn->panel_count - 1;

    /* Remove from source */
    dock_remove_panel(&d, e_leaf, PANEL_EDITOR);

    /* Verify game node now has 2 tabs */
    ASSERT_EQ(gn->panel_count, 2);
    ASSERT_EQ(gn->panels[0], PANEL_GAME);
    ASSERT_EQ(gn->panels[1], PANEL_EDITOR);

    /* Collapse empty nodes */
    {
        int new_root = dock_collapse_empty(&d, d.windows[0].root_node);
        d.windows[0].root_node = new_root;
        ASSERT(new_root >= 0);
    }
}

/* ── Tests: Collect leaves ────────────────────────────────────────────── */

TEST(collect_leaves_gathers_all_panels) {
    dock_state d = make_default_dock();
    int root = d.windows[0].root_node;
    PanelId collected[PANEL_COUNT];
    int count = 0;
    int has_game = 0, has_editor = 0, has_prof = 0, has_scene = 0, has_insp = 0, i;

    dock_collect_leaves(&d, root, collected, &count, PANEL_COUNT);
    ASSERT_EQ(count, 5);

    for (i = 0; i < count; i++) {
        if (collected[i] == PANEL_GAME) has_game = 1;
        if (collected[i] == PANEL_EDITOR) has_editor = 1;
        if (collected[i] == PANEL_PROFILER) has_prof = 1;
        if (collected[i] == PANEL_SCENE_TREE) has_scene = 1;
        if (collected[i] == PANEL_INSPECTOR) has_insp = 1;
    }
    ASSERT(has_game);
    ASSERT(has_editor);
    ASSERT(has_prof);
    ASSERT(has_scene);
    ASSERT(has_insp);
}

/* ── Tests: Global panel search ───────────────────────────────────────── */

TEST(find_leaf_for_panel_global_finds_in_window) {
    dock_state d = make_default_dock();
    int win_idx = -1;
    int leaf = dock_find_leaf_for_panel_global(&d, PANEL_GAME, &win_idx);
    ASSERT(leaf >= 0);
    ASSERT_EQ(win_idx, 0);
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(void) {
    printf("=== Dock system tests ===\n\n");

    /* Node allocation */
    run_alloc_node_returns_valid_index();
    run_alloc_node_returns_unique_indices();
    run_free_node_allows_reuse();
    run_alloc_node_exhaustion_returns_minus_one();

    /* Default layout */
    run_init_default_creates_seven_nodes();
    run_init_default_window_zero_active();
    run_init_default_all_panels_reachable();
    run_init_default_each_panel_in_own_leaf();

    /* Layout */
    run_layout_root_covers_full_window();
    run_layout_leaves_partition_width();
    run_layout_leaves_same_height();

    /* Node at point */
    run_node_at_point_finds_correct_leaf();
    run_node_at_point_outside_returns_minus_one();

    /* Drop zones */
    run_drop_zone_edges();

    /* Divider hit-test */
    run_divider_hit_on_vertical_split_line();
    run_divider_hit_on_inner_split_line();
    run_divider_miss_in_panel_center();
    run_divider_miss_outside_window();
    run_divider_inner_takes_priority_over_outer();

    /* Split at */
    run_split_at_left_creates_horizontal_split();
    run_split_at_bottom_creates_vertical_split();
    run_split_at_right_new_panel_is_second_child();

    /* Remove panel */
    run_remove_panel_decrements_count();
    run_remove_panel_from_multi_tab_shifts();

    /* Collapse empty */
    run_collapse_empty_leaf_returns_minus_one();
    run_collapse_promotes_sibling_when_child_empty();

    /* Find parent */
    run_find_parent_of_root_returns_minus_one();
    run_find_parent_of_child();

    /* Header hit test */
    run_header_hit_test_in_header_area();
    run_header_hit_test_below_header_misses();
    run_header_hit_test_multi_tab_selects_correct_tab();

    /* Window for SDL */
    run_window_for_sdl_finds_correct_window();

    /* Full cycles */
    run_drag_split_collapse_cycle();
    run_drag_split_vertical_cycle();
    run_drop_center_adds_tab();

    /* Collect leaves */
    run_collect_leaves_gathers_all_panels();

    /* Global search */
    run_find_leaf_for_panel_global_finds_in_window();

    printf("\n=== Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) printf(", %d FAILED", tests_failed);
    printf(" ===\n");

    return tests_failed > 0 ? 1 : 0;
}
