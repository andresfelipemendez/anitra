/* ── Dock system PATH COVERAGE tests ──────────────────────────────────────
   Exercises every unique path through dock functions, including error paths,
   partial failures, and interaction paths that branch coverage misses.

   Standalone test binary — no SDL, no GPU, no arena needed.

   Build:  tcc\tcc.exe -Blib/tcc-windows -g -Isrc -Isrc/editor tests/test_dock_paths.c
   Run:    build\Debug\test_dock_paths.exe
   ────────────────────────────────────────────────────────────────────────── */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "editor.h"

/* ── Minimal test harness ─────────────────────────────────────────────── */

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    static void test_##name(void); \
    static void run_##name(void) { \
        tests_run++; \
        printf("  %-60s ", #name); \
        test_##name(); \
        tests_passed++; \
        printf("PASS\n"); \
    } \
    static void test_##name(void)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("FAIL\n    %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        tests_failed++; \
        tests_passed--; \
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

/* ═══════════════════════════════════════════════════════════════════════
 *  dock_add_panel_with_error — 7 paths
 * ═══════════════════════════════════════════════════════════════════════ */

TEST(add_panel_err_null_dock) {
    error_value err = ERRV_OK;
    int r = dock_add_panel_with_error(NULL, 0, PANEL_GAME, &err);
    ASSERT_EQ(r, -1);
    ASSERT_EQ(err.code, DOCK_ERR_INVALID_ARGS);
}

TEST(add_panel_err_index_negative) {
    dock_state d;
    error_value err = ERRV_OK;
    memset(&d, 0, sizeof(d));
    ASSERT_EQ(dock_add_panel_with_error(&d, -1, PANEL_GAME, &err), -1);
    ASSERT_EQ(err.code, DOCK_ERR_INVALID_NODE_INDEX);
}

TEST(add_panel_err_index_too_large) {
    dock_state d;
    error_value err = ERRV_OK;
    memset(&d, 0, sizeof(d));
    ASSERT_EQ(dock_add_panel_with_error(&d, MAX_DOCK_NODES, PANEL_GAME, &err), -1);
    ASSERT_EQ(err.code, DOCK_ERR_INVALID_NODE_INDEX);
}

TEST(add_panel_err_node_not_in_use) {
    dock_state d;
    error_value err = ERRV_OK;
    memset(&d, 0, sizeof(d));
    ASSERT_EQ(dock_add_panel_with_error(&d, 0, PANEL_GAME, &err), -1);
    ASSERT_EQ(err.code, DOCK_ERR_NODE_NOT_IN_USE);
}

TEST(add_panel_err_node_is_split) {
    dock_state d;
    error_value err = ERRV_OK;
    int split;
    memset(&d, 0, sizeof(d));
    split = dock_alloc_node(&d);
    d.nodes[split].type = DOCK_SPLIT_H;
    ASSERT_EQ(dock_add_panel_with_error(&d, split, PANEL_GAME, &err), -1);
    ASSERT_EQ(err.code, DOCK_ERR_NODE_NOT_TABS);
}

TEST(add_panel_err_node_full) {
    dock_state d;
    error_value err = ERRV_OK;
    int leaf, i;
    memset(&d, 0, sizeof(d));
    leaf = dock_alloc_node(&d);
    d.nodes[leaf].panel_count = MAX_TABS_PER_NODE;
    for (i = 0; i < MAX_TABS_PER_NODE; i++)
        d.nodes[leaf].panels[i] = (PanelId)i;
    ASSERT_EQ(dock_add_panel_with_error(&d, leaf, PANEL_GAME, &err), -1);
    ASSERT_EQ(err.code, DOCK_ERR_NODE_FULL);
}

TEST(add_panel_success) {
    dock_state d;
    error_value err;
    int leaf, r;
    memset(&d, 0, sizeof(d));
    err.code = 99;
    leaf = dock_alloc_node(&d);
    r = dock_add_panel_with_error(&d, leaf, PANEL_EDITOR, &err);
    ASSERT_EQ(r, 0);
    ASSERT(ERRV_IS_OK(err));
    ASSERT_EQ(d.nodes[leaf].panel_count, 1);
    ASSERT_EQ(d.nodes[leaf].panels[0], PANEL_EDITOR);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  dock_remove_panel_with_error — 7 paths
 * ═══════════════════════════════════════════════════════════════════════ */

TEST(remove_panel_err_null_dock) {
    error_value err = ERRV_OK;
    ASSERT_EQ(dock_remove_panel_with_error(NULL, 0, PANEL_GAME, &err), -1);
    ASSERT_EQ(err.code, DOCK_ERR_INVALID_ARGS);
}

TEST(remove_panel_err_index_out_of_range) {
    dock_state d;
    error_value err = ERRV_OK;
    memset(&d, 0, sizeof(d));
    ASSERT_EQ(dock_remove_panel_with_error(&d, -1, PANEL_GAME, &err), -1);
    ASSERT_EQ(err.code, DOCK_ERR_INVALID_NODE_INDEX);
}

TEST(remove_panel_err_not_in_use) {
    dock_state d;
    error_value err = ERRV_OK;
    memset(&d, 0, sizeof(d));
    ASSERT_EQ(dock_remove_panel_with_error(&d, 0, PANEL_GAME, &err), -1);
    ASSERT_EQ(err.code, DOCK_ERR_NODE_NOT_IN_USE);
}

TEST(remove_panel_err_not_tabs) {
    dock_state d;
    error_value err = ERRV_OK;
    int split;
    memset(&d, 0, sizeof(d));
    split = dock_alloc_node(&d);
    d.nodes[split].type = DOCK_SPLIT_V;
    ASSERT_EQ(dock_remove_panel_with_error(&d, split, PANEL_GAME, &err), -1);
    ASSERT_EQ(err.code, DOCK_ERR_NODE_NOT_TABS);
}

TEST(remove_panel_err_not_found) {
    dock_state d;
    error_value err = ERRV_OK;
    int leaf;
    memset(&d, 0, sizeof(d));
    leaf = dock_alloc_node(&d);
    d.nodes[leaf].panels[0] = PANEL_GAME;
    d.nodes[leaf].panel_count = 1;
    ASSERT_EQ(dock_remove_panel_with_error(&d, leaf, PANEL_EDITOR, &err), -1);
    ASSERT_EQ(err.code, DOCK_ERR_PANEL_NOT_FOUND);
}

TEST(remove_panel_active_tab_no_clamp) {
    /* Path: panel found, active_tab < new panel_count — no adjustment */
    dock_state d;
    error_value err;
    int leaf;
    memset(&d, 0, sizeof(d));
    leaf = dock_alloc_node(&d);
    d.nodes[leaf].panels[0] = PANEL_GAME;
    d.nodes[leaf].panels[1] = PANEL_EDITOR;
    d.nodes[leaf].panels[2] = PANEL_PROFILER;
    d.nodes[leaf].panel_count = 3;
    d.nodes[leaf].active_tab = 0; /* viewing GAME, remove PROFILER (last) */
    err.code = 99;
    ASSERT_EQ(dock_remove_panel_with_error(&d, leaf, PANEL_PROFILER, &err), 0);
    ASSERT(ERRV_IS_OK(err));
    ASSERT_EQ(d.nodes[leaf].panel_count, 2);
    ASSERT_EQ(d.nodes[leaf].active_tab, 0); /* unchanged — still in range */
}

TEST(remove_panel_active_tab_clamped) {
    /* Path: panel found, active_tab >= new panel_count — clamp down */
    dock_state d;
    error_value err;
    int leaf;
    memset(&d, 0, sizeof(d));
    leaf = dock_alloc_node(&d);
    d.nodes[leaf].panels[0] = PANEL_GAME;
    d.nodes[leaf].panels[1] = PANEL_EDITOR;
    d.nodes[leaf].panel_count = 2;
    d.nodes[leaf].active_tab = 1; /* viewing EDITOR (index 1) */
    err.code = 99;
    ASSERT_EQ(dock_remove_panel_with_error(&d, leaf, PANEL_EDITOR, &err), 0);
    ASSERT(ERRV_IS_OK(err));
    ASSERT_EQ(d.nodes[leaf].panel_count, 1);
    ASSERT_EQ(d.nodes[leaf].active_tab, 0); /* clamped to last valid */
}

/* ═══════════════════════════════════════════════════════════════════════
 *  dock_split_with_error — 6 paths
 * ═══════════════════════════════════════════════════════════════════════ */

TEST(split_err_null_dock) {
    error_value err = ERRV_OK;
    ASSERT_EQ(dock_split_with_error(NULL, 0, 1, &err), -1);
    ASSERT_EQ(err.code, DOCK_ERR_INVALID_ARGS);
}

TEST(split_err_index_out_of_range) {
    dock_state d;
    error_value err = ERRV_OK;
    memset(&d, 0, sizeof(d));
    ASSERT_EQ(dock_split_with_error(&d, -1, 1, &err), -1);
    ASSERT_EQ(err.code, DOCK_ERR_INVALID_NODE_INDEX);
}

TEST(split_err_not_in_use) {
    dock_state d;
    error_value err = ERRV_OK;
    memset(&d, 0, sizeof(d));
    ASSERT_EQ(dock_split_with_error(&d, 0, 1, &err), -1);
    ASSERT_EQ(err.code, DOCK_ERR_NODE_NOT_IN_USE);
}

TEST(split_err_alloc_exhausted) {
    /* Fill all but 1 node — split needs 2 new nodes, only 1 available */
    dock_state d;
    error_value err = ERRV_OK;
    int i, target;
    memset(&d, 0, sizeof(d));
    for (i = 0; i < MAX_DOCK_NODES - 1; i++)
        dock_alloc_node(&d);
    target = 0; /* first allocated node */
    ASSERT_EQ(dock_split_with_error(&d, target, 1, &err), -1);
    ASSERT_EQ(err.code, DOCK_ERR_ALLOC_NODE_FAILED);
}

TEST(split_success_horizontal) {
    dock_state d;
    error_value err;
    int leaf, new_leaf;
    memset(&d, 0, sizeof(d));
    leaf = dock_alloc_node(&d);
    d.nodes[leaf].panels[0] = PANEL_GAME;
    d.nodes[leaf].panel_count = 1;
    err.code = 99;
    new_leaf = dock_split_with_error(&d, leaf, 1, &err);
    ASSERT(ERRV_IS_OK(err));
    ASSERT(new_leaf >= 0);
    ASSERT_EQ(d.nodes[leaf].type, DOCK_SPLIT_H);
    ASSERT(d.nodes[leaf].children[0] >= 0);
    ASSERT_EQ(d.nodes[leaf].children[1], new_leaf);
}

TEST(split_success_vertical) {
    dock_state d;
    error_value err;
    int leaf, new_leaf;
    memset(&d, 0, sizeof(d));
    leaf = dock_alloc_node(&d);
    d.nodes[leaf].panels[0] = PANEL_GAME;
    d.nodes[leaf].panel_count = 1;
    err.code = 99;
    new_leaf = dock_split_with_error(&d, leaf, 0, &err);
    ASSERT(ERRV_IS_OK(err));
    ASSERT(new_leaf >= 0);
    ASSERT_EQ(d.nodes[leaf].type, DOCK_SPLIT_V);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  dock_split_at_with_error — 8 paths
 * ═══════════════════════════════════════════════════════════════════════ */

TEST(split_at_err_null_dock) {
    error_value err = ERRV_OK;
    ASSERT_EQ(dock_split_at_with_error(NULL, 0, 0, PANEL_GAME, DROP_LEFT,
              NULL, NULL, &err), -1);
    ASSERT_EQ(err.code, DOCK_ERR_INVALID_ARGS);
}

TEST(split_at_err_index_out_of_range) {
    dock_state d;
    error_value err = ERRV_OK;
    memset(&d, 0, sizeof(d));
    ASSERT_EQ(dock_split_at_with_error(&d, 0, -1, PANEL_GAME, DROP_LEFT,
              NULL, NULL, &err), -1);
    ASSERT_EQ(err.code, DOCK_ERR_INVALID_NODE_INDEX);
}

TEST(split_at_err_not_in_use) {
    dock_state d;
    error_value err = ERRV_OK;
    memset(&d, 0, sizeof(d));
    ASSERT_EQ(dock_split_at_with_error(&d, 0, 0, PANEL_GAME, DROP_LEFT,
              NULL, NULL, &err), -1);
    ASSERT_EQ(err.code, DOCK_ERR_NODE_NOT_IN_USE);
}

TEST(split_at_err_invalid_zone_none) {
    dock_state d;
    error_value err = ERRV_OK;
    int leaf;
    memset(&d, 0, sizeof(d));
    leaf = dock_alloc_node(&d);
    ASSERT_EQ(dock_split_at_with_error(&d, 0, leaf, PANEL_GAME, DROP_NONE,
              NULL, NULL, &err), -1);
    ASSERT_EQ(err.code, DOCK_ERR_INVALID_DROP_ZONE);
}

TEST(split_at_err_invalid_zone_center) {
    dock_state d;
    error_value err = ERRV_OK;
    int leaf;
    memset(&d, 0, sizeof(d));
    leaf = dock_alloc_node(&d);
    ASSERT_EQ(dock_split_at_with_error(&d, 0, leaf, PANEL_GAME, DROP_CENTER,
              NULL, NULL, &err), -1);
    ASSERT_EQ(err.code, DOCK_ERR_INVALID_DROP_ZONE);
}

TEST(split_at_left_new_panel_is_first_child) {
    dock_state d;
    error_value err;
    int leaf, new_leaf = -1, existing_leaf = -1;
    memset(&d, 0, sizeof(d));
    leaf = dock_alloc_node(&d);
    d.nodes[leaf].panels[0] = PANEL_GAME;
    d.nodes[leaf].panel_count = 1;
    err.code = 99;
    ASSERT_EQ(dock_split_at_with_error(&d, 0, leaf, PANEL_EDITOR, DROP_LEFT,
              &new_leaf, &existing_leaf, &err), 0);
    ASSERT(ERRV_IS_OK(err));
    ASSERT_EQ(d.nodes[leaf].type, DOCK_SPLIT_H);
    ASSERT_EQ(d.nodes[leaf].children[0], new_leaf);     /* new panel first */
    ASSERT_EQ(d.nodes[leaf].children[1], existing_leaf); /* original second */
    ASSERT_EQ(d.nodes[new_leaf].panels[0], PANEL_EDITOR);
    ASSERT_EQ(d.nodes[existing_leaf].panels[0], PANEL_GAME);
}

TEST(split_at_top_new_panel_is_first_child) {
    dock_state d;
    error_value err;
    int leaf, new_leaf = -1, existing_leaf = -1;
    memset(&d, 0, sizeof(d));
    leaf = dock_alloc_node(&d);
    d.nodes[leaf].panels[0] = PANEL_GAME;
    d.nodes[leaf].panel_count = 1;
    err.code = 99;
    ASSERT_EQ(dock_split_at_with_error(&d, 0, leaf, PANEL_EDITOR, DROP_TOP,
              &new_leaf, &existing_leaf, &err), 0);
    ASSERT(ERRV_IS_OK(err));
    ASSERT_EQ(d.nodes[leaf].type, DOCK_SPLIT_V);
    ASSERT_EQ(d.nodes[leaf].children[0], new_leaf);
    ASSERT_EQ(d.nodes[leaf].children[1], existing_leaf);
}

TEST(split_at_right_new_panel_is_second) {
    dock_state d;
    error_value err;
    int leaf, new_leaf = -1, existing_leaf = -1;
    memset(&d, 0, sizeof(d));
    leaf = dock_alloc_node(&d);
    d.nodes[leaf].panels[0] = PANEL_GAME;
    d.nodes[leaf].panel_count = 1;
    err.code = 99;
    ASSERT_EQ(dock_split_at_with_error(&d, 0, leaf, PANEL_EDITOR, DROP_RIGHT,
              &new_leaf, &existing_leaf, &err), 0);
    ASSERT(ERRV_IS_OK(err));
    ASSERT_EQ(d.nodes[leaf].children[0], existing_leaf);
    ASSERT_EQ(d.nodes[leaf].children[1], new_leaf);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  dock_collapse_empty — 9 paths
 * ═══════════════════════════════════════════════════════════════════════ */

TEST(collapse_err_null_dock) {
    error_value err = ERRV_OK;
    int root = -1;
    ASSERT_EQ(dock_collapse_empty_with_error(NULL, 0, &root, &err), -1);
    ASSERT_EQ(err.code, DOCK_ERR_INVALID_ARGS);
}

TEST(collapse_err_null_output) {
    dock_state d;
    error_value err = ERRV_OK;
    memset(&d, 0, sizeof(d));
    ASSERT_EQ(dock_collapse_empty_with_error(&d, 0, NULL, &err), -1);
    ASSERT_EQ(err.code, DOCK_ERR_INVALID_OUTPUT);
}

TEST(collapse_negative_index_returns_minus_one) {
    dock_state d;
    int root = 99;
    memset(&d, 0, sizeof(d));
    ASSERT_EQ(dock_collapse_empty_with_error(&d, -1, &root, NULL), 0);
    ASSERT_EQ(root, -1);
}

TEST(collapse_index_too_large) {
    dock_state d;
    int root = 99;
    error_value err = ERRV_OK;
    memset(&d, 0, sizeof(d));
    ASSERT_EQ(dock_collapse_empty_with_error(&d, MAX_DOCK_NODES, &root, &err), -1);
    ASSERT_EQ(err.code, DOCK_ERR_INVALID_NODE_INDEX);
}

TEST(collapse_not_in_use) {
    dock_state d;
    int root = 99;
    error_value err = ERRV_OK;
    memset(&d, 0, sizeof(d));
    ASSERT_EQ(dock_collapse_empty_with_error(&d, 0, &root, &err), -1);
    ASSERT_EQ(err.code, DOCK_ERR_NODE_NOT_IN_USE);
}

TEST(collapse_tabs_nonempty_keeps_node) {
    dock_state d;
    int leaf, root = -1;
    memset(&d, 0, sizeof(d));
    leaf = dock_alloc_node(&d);
    d.nodes[leaf].panels[0] = PANEL_GAME;
    d.nodes[leaf].panel_count = 1;
    dock_collapse_empty_with_error(&d, leaf, &root, NULL);
    ASSERT_EQ(root, leaf);
    ASSERT_EQ(d.nodes[leaf].in_use, 1);
}

TEST(collapse_tabs_empty_frees_node) {
    dock_state d;
    int leaf, root = 99;
    memset(&d, 0, sizeof(d));
    leaf = dock_alloc_node(&d);
    d.nodes[leaf].panel_count = 0;
    dock_collapse_empty_with_error(&d, leaf, &root, NULL);
    ASSERT_EQ(root, -1);
    ASSERT_EQ(d.nodes[leaf].in_use, 0);
}

TEST(collapse_split_both_children_empty) {
    dock_state d;
    int split, left, right, root = -1;
    memset(&d, 0, sizeof(d));
    left = dock_alloc_node(&d);
    d.nodes[left].panel_count = 0;
    right = dock_alloc_node(&d);
    d.nodes[right].panel_count = 0;
    split = dock_alloc_node(&d);
    d.nodes[split].type = DOCK_SPLIT_H;
    d.nodes[split].children[0] = left;
    d.nodes[split].children[1] = right;
    dock_collapse_empty_with_error(&d, split, &root, NULL);
    ASSERT_EQ(root, -1);
    ASSERT_EQ(d.nodes[split].in_use, 0);
    ASSERT_EQ(d.nodes[left].in_use, 0);
    ASSERT_EQ(d.nodes[right].in_use, 0);
}

TEST(collapse_split_left_empty_promote_right) {
    dock_state d;
    int split, left, right, root = -1;
    memset(&d, 0, sizeof(d));
    left = dock_alloc_node(&d);
    d.nodes[left].panel_count = 0;
    right = dock_alloc_node(&d);
    d.nodes[right].panels[0] = PANEL_GAME;
    d.nodes[right].panel_count = 1;
    split = dock_alloc_node(&d);
    d.nodes[split].type = DOCK_SPLIT_H;
    d.nodes[split].children[0] = left;
    d.nodes[split].children[1] = right;
    dock_collapse_empty_with_error(&d, split, &root, NULL);
    ASSERT_EQ(root, right);
    ASSERT_EQ(d.nodes[split].in_use, 0);
    ASSERT_EQ(d.nodes[left].in_use, 0);
    ASSERT_EQ(d.nodes[right].in_use, 1);
}

TEST(collapse_split_right_empty_promote_left) {
    dock_state d;
    int split, left, right, root = -1;
    memset(&d, 0, sizeof(d));
    left = dock_alloc_node(&d);
    d.nodes[left].panels[0] = PANEL_GAME;
    d.nodes[left].panel_count = 1;
    right = dock_alloc_node(&d);
    d.nodes[right].panel_count = 0;
    split = dock_alloc_node(&d);
    d.nodes[split].type = DOCK_SPLIT_H;
    d.nodes[split].children[0] = left;
    d.nodes[split].children[1] = right;
    dock_collapse_empty_with_error(&d, split, &root, NULL);
    ASSERT_EQ(root, left);
    ASSERT_EQ(d.nodes[split].in_use, 0);
    ASSERT_EQ(d.nodes[right].in_use, 0);
    ASSERT_EQ(d.nodes[left].in_use, 1);
}

TEST(collapse_split_both_alive) {
    dock_state d;
    int split, left, right, root = -1;
    memset(&d, 0, sizeof(d));
    left = dock_alloc_node(&d);
    d.nodes[left].panels[0] = PANEL_GAME;
    d.nodes[left].panel_count = 1;
    right = dock_alloc_node(&d);
    d.nodes[right].panels[0] = PANEL_EDITOR;
    d.nodes[right].panel_count = 1;
    split = dock_alloc_node(&d);
    d.nodes[split].type = DOCK_SPLIT_V;
    d.nodes[split].children[0] = left;
    d.nodes[split].children[1] = right;
    dock_collapse_empty_with_error(&d, split, &root, NULL);
    ASSERT_EQ(root, split);
    ASSERT_EQ(d.nodes[split].in_use, 1);
    ASSERT_EQ(d.nodes[left].in_use, 1);
    ASSERT_EQ(d.nodes[right].in_use, 1);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  dock_set_active_tab — 4 paths
 * ═══════════════════════════════════════════════════════════════════════ */

TEST(set_active_tab_invalid_index) {
    dock_state d;
    memset(&d, 0, sizeof(d));
    dock_set_active_tab(&d, -1, 0);
    dock_set_active_tab(&d, MAX_DOCK_NODES, 0);
}

TEST(set_active_tab_not_tabs_node) {
    dock_state d;
    int split;
    memset(&d, 0, sizeof(d));
    split = dock_alloc_node(&d);
    d.nodes[split].type = DOCK_SPLIT_H;
    d.nodes[split].panel_count = 2;
    dock_set_active_tab(&d, split, 1);
    ASSERT_EQ(d.nodes[split].active_tab, 0); /* unchanged */
}

TEST(set_active_tab_in_range) {
    dock_state d;
    int leaf;
    memset(&d, 0, sizeof(d));
    leaf = dock_alloc_node(&d);
    d.nodes[leaf].panels[0] = PANEL_GAME;
    d.nodes[leaf].panels[1] = PANEL_EDITOR;
    d.nodes[leaf].panel_count = 2;
    dock_set_active_tab(&d, leaf, 1);
    ASSERT_EQ(d.nodes[leaf].active_tab, 1);
}

TEST(set_active_tab_out_of_range) {
    dock_state d;
    int leaf;
    memset(&d, 0, sizeof(d));
    leaf = dock_alloc_node(&d);
    d.nodes[leaf].panels[0] = PANEL_GAME;
    d.nodes[leaf].panel_count = 1;
    d.nodes[leaf].active_tab = 0;
    dock_set_active_tab(&d, leaf, 5);
    ASSERT_EQ(d.nodes[leaf].active_tab, 0); /* unchanged */
}

/* ═══════════════════════════════════════════════════════════════════════
 *  dock_layout — boundary paths
 * ═══════════════════════════════════════════════════════════════════════ */

TEST(layout_invalid_window_index) {
    dock_state d;
    memset(&d, 0, sizeof(d));
    dock_layout(&d, -1, 800, 600);
    dock_layout(&d, MAX_DOCK_WINDOWS, 800, 600);
}

TEST(layout_window_not_in_use) {
    dock_state d;
    memset(&d, 0, sizeof(d));
    d.windows[0].in_use = 0;
    dock_layout(&d, 0, 800, 600);
}

TEST(layout_vertical_split_divides_height) {
    dock_state d;
    int root, top, bottom;
    memset(&d, 0, sizeof(d));
    top = dock_alloc_node(&d);
    d.nodes[top].panels[0] = PANEL_GAME;
    d.nodes[top].panel_count = 1;
    bottom = dock_alloc_node(&d);
    d.nodes[bottom].panels[0] = PANEL_EDITOR;
    d.nodes[bottom].panel_count = 1;
    root = dock_alloc_node(&d);
    d.nodes[root].type = DOCK_SPLIT_V;
    d.nodes[root].ratio = 0.5f;
    d.nodes[root].children[0] = top;
    d.nodes[root].children[1] = bottom;
    d.windows[0].in_use = 1;
    d.windows[0].root_node = root;
    dock_layout(&d, 0, 800, 828); /* 828 - 28 menu = 800 content */
    ASSERT_FLOAT_EQ(d.nodes[top].h, 400.0f, 1.0f);
    ASSERT_FLOAT_EQ(d.nodes[bottom].h, 400.0f, 1.0f);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  dock_leaf_for_panel — boundary paths
 * ═══════════════════════════════════════════════════════════════════════ */

TEST(leaf_for_panel_invalid_index) {
    dock_state d;
    memset(&d, 0, sizeof(d));
    ASSERT_EQ(dock_leaf_for_panel(&d, -1, PANEL_GAME), -1);
    ASSERT_EQ(dock_leaf_for_panel(&d, MAX_DOCK_NODES, PANEL_GAME), -1);
}

TEST(leaf_for_panel_not_in_use) {
    dock_state d;
    memset(&d, 0, sizeof(d));
    ASSERT_EQ(dock_leaf_for_panel(&d, 0, PANEL_GAME), -1);
}

TEST(leaf_for_panel_not_found_in_tabs) {
    dock_state d;
    int leaf;
    memset(&d, 0, sizeof(d));
    leaf = dock_alloc_node(&d);
    d.nodes[leaf].panels[0] = PANEL_GAME;
    d.nodes[leaf].panel_count = 1;
    ASSERT_EQ(dock_leaf_for_panel(&d, leaf, PANEL_EDITOR), -1);
}

TEST(leaf_for_panel_found_in_right_child) {
    dock_state d;
    int split, left, right;
    memset(&d, 0, sizeof(d));
    left = dock_alloc_node(&d);
    d.nodes[left].panels[0] = PANEL_GAME;
    d.nodes[left].panel_count = 1;
    right = dock_alloc_node(&d);
    d.nodes[right].panels[0] = PANEL_EDITOR;
    d.nodes[right].panel_count = 1;
    split = dock_alloc_node(&d);
    d.nodes[split].type = DOCK_SPLIT_H;
    d.nodes[split].children[0] = left;
    d.nodes[split].children[1] = right;
    ASSERT_EQ(dock_leaf_for_panel(&d, split, PANEL_EDITOR), right);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  dock_find_leaf_for_panel_global — boundary paths
 * ═══════════════════════════════════════════════════════════════════════ */

TEST(find_global_not_found) {
    dock_state d;
    int win_idx = 99;
    memset(&d, 0, sizeof(d));
    dock_init(&d);
    ASSERT_EQ(dock_find_leaf_for_panel_global(&d, PANEL_OUTLINE, &win_idx), -1);
    ASSERT_EQ(win_idx, -1);
}

TEST(find_global_null_out_window) {
    dock_state d;
    memset(&d, 0, sizeof(d));
    dock_init(&d);
    ASSERT(dock_find_leaf_for_panel_global(&d, PANEL_GAME, NULL) >= 0);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  dock_drop_zone — null path
 * ═══════════════════════════════════════════════════════════════════════ */

TEST(drop_zone_null_node) {
    ASSERT_EQ(dock_drop_zone(NULL, 100.0f, 100.0f), DROP_NONE);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  dock_get_panel_rect — 3 paths
 * ═══════════════════════════════════════════════════════════════════════ */

TEST(get_panel_rect_invalid_index) {
    dock_state d;
    float x = -1, y = -1, w = -1, h = -1;
    memset(&d, 0, sizeof(d));
    dock_get_panel_rect(&d, -1, 0, &x, &y, &w, &h);
    ASSERT_FLOAT_EQ(x, -1.0f, 0.01f); /* unchanged — early return */
}

TEST(get_panel_rect_not_tabs) {
    dock_state d;
    float x = -1, y = -1, w = -1, h = -1;
    int split;
    memset(&d, 0, sizeof(d));
    split = dock_alloc_node(&d);
    d.nodes[split].type = DOCK_SPLIT_H;
    dock_get_panel_rect(&d, split, 0, &x, &y, &w, &h);
    ASSERT_FLOAT_EQ(x, -1.0f, 0.01f); /* unchanged */
}

TEST(get_panel_rect_success) {
    dock_state d;
    float x = 0, y = 0, w = 0, h = 0;
    int leaf;
    memset(&d, 0, sizeof(d));
    leaf = dock_alloc_node(&d);
    d.nodes[leaf].x = 100; d.nodes[leaf].y = 200;
    d.nodes[leaf].w = 400; d.nodes[leaf].h = 300;
    dock_get_panel_rect(&d, leaf, 0, &x, &y, &w, &h);
    ASSERT_FLOAT_EQ(x, 100.0f, 0.01f);
    ASSERT_FLOAT_EQ(y, 200.0f + DOCK_HEADER_HEIGHT, 0.01f);
    ASSERT_FLOAT_EQ(w, 400.0f, 0.01f);
    ASSERT_FLOAT_EQ(h, 300.0f - DOCK_HEADER_HEIGHT, 0.01f);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  dock_free_subtree — paths
 * ═══════════════════════════════════════════════════════════════════════ */

TEST(free_subtree_invalid_index) {
    dock_state d;
    memset(&d, 0, sizeof(d));
    dock_free_subtree(&d, -1);
    dock_free_subtree(&d, MAX_DOCK_NODES);
}

TEST(free_subtree_frees_entire_tree) {
    dock_state d;
    int split, left, right;
    memset(&d, 0, sizeof(d));
    left = dock_alloc_node(&d);
    d.nodes[left].panels[0] = PANEL_GAME;
    d.nodes[left].panel_count = 1;
    right = dock_alloc_node(&d);
    d.nodes[right].panels[0] = PANEL_EDITOR;
    d.nodes[right].panel_count = 1;
    split = dock_alloc_node(&d);
    d.nodes[split].type = DOCK_SPLIT_H;
    d.nodes[split].children[0] = left;
    d.nodes[split].children[1] = right;
    dock_free_subtree(&d, split);
    ASSERT_EQ(d.nodes[split].in_use, 0);
    ASSERT_EQ(d.nodes[left].in_use, 0);
    ASSERT_EQ(d.nodes[right].in_use, 0);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  dock_window_for_sdl — null path
 * ═══════════════════════════════════════════════════════════════════════ */

TEST(window_for_sdl_null_returns_minus_one) {
    dock_state d;
    memset(&d, 0, sizeof(d));
    ASSERT_EQ(dock_window_for_sdl(&d, NULL), -1);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  dock_find_parent — deeper paths
 * ═══════════════════════════════════════════════════════════════════════ */

TEST(find_parent_invalid_root) {
    dock_state d;
    int slot = 99;
    memset(&d, 0, sizeof(d));
    ASSERT_EQ(dock_find_parent(&d, -1, 0, &slot), -1);
}

TEST(find_parent_tabs_node_returns_minus_one) {
    dock_state d;
    int leaf, slot = 99;
    memset(&d, 0, sizeof(d));
    leaf = dock_alloc_node(&d);
    d.nodes[leaf].panels[0] = PANEL_GAME;
    d.nodes[leaf].panel_count = 1;
    ASSERT_EQ(dock_find_parent(&d, leaf, 99, &slot), -1);
}

TEST(find_parent_in_slot_zero) {
    dock_state d;
    int split, left, right, slot = -1;
    memset(&d, 0, sizeof(d));
    left = dock_alloc_node(&d);
    right = dock_alloc_node(&d);
    split = dock_alloc_node(&d);
    d.nodes[split].type = DOCK_SPLIT_H;
    d.nodes[split].children[0] = left;
    d.nodes[split].children[1] = right;
    ASSERT_EQ(dock_find_parent(&d, split, left, &slot), split);
    ASSERT_EQ(slot, 0);
}

TEST(find_parent_in_slot_one) {
    dock_state d;
    int split, left, right, slot = -1;
    memset(&d, 0, sizeof(d));
    left = dock_alloc_node(&d);
    right = dock_alloc_node(&d);
    split = dock_alloc_node(&d);
    d.nodes[split].type = DOCK_SPLIT_H;
    d.nodes[split].children[0] = left;
    d.nodes[split].children[1] = right;
    ASSERT_EQ(dock_find_parent(&d, split, right, &slot), split);
    ASSERT_EQ(slot, 1);
}

TEST(find_parent_null_out_slot) {
    dock_state d;
    int split, left, right;
    memset(&d, 0, sizeof(d));
    left = dock_alloc_node(&d);
    right = dock_alloc_node(&d);
    split = dock_alloc_node(&d);
    d.nodes[split].type = DOCK_SPLIT_H;
    d.nodes[split].children[0] = left;
    d.nodes[split].children[1] = right;
    ASSERT_EQ(dock_find_parent(&d, split, left, NULL), split);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  header_hit_test_node — missing paths
 * ═══════════════════════════════════════════════════════════════════════ */

TEST(header_hit_invalid_index) {
    dock_state d;
    int out_node = -1;
    PanelId out_panel = PANEL_GAME;
    int out_tab = -1;
    memset(&d, 0, sizeof(d));
    ASSERT_EQ(header_hit_test_node(&d, -1, 0, 0, &out_node, &out_panel, &out_tab), 0);
}

TEST(header_hit_split_found_in_right) {
    dock_state d;
    int split, left, right;
    int out_node = -1;
    PanelId out_panel = PANEL_GAME;
    int out_tab = -1;
    memset(&d, 0, sizeof(d));
    left = dock_alloc_node(&d);
    d.nodes[left].panels[0] = PANEL_GAME;
    d.nodes[left].panel_count = 1;
    d.nodes[left].x = 0; d.nodes[left].y = 0;
    d.nodes[left].w = 400; d.nodes[left].h = 600;
    right = dock_alloc_node(&d);
    d.nodes[right].panels[0] = PANEL_EDITOR;
    d.nodes[right].panel_count = 1;
    d.nodes[right].x = 400; d.nodes[right].y = 0;
    d.nodes[right].w = 400; d.nodes[right].h = 600;
    split = dock_alloc_node(&d);
    d.nodes[split].type = DOCK_SPLIT_H;
    d.nodes[split].children[0] = left;
    d.nodes[split].children[1] = right;
    ASSERT(header_hit_test_node(&d, split, 600.0f, 10.0f,
                                 &out_node, &out_panel, &out_tab));
    ASSERT_EQ(out_panel, PANEL_EDITOR);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  dock_divider_at_point — V-split divider path
 * ═══════════════════════════════════════════════════════════════════════ */

TEST(divider_hit_on_horizontal_divider_line) {
    dock_state d;
    int split, top, bottom;
    float div_y;
    memset(&d, 0, sizeof(d));
    top = dock_alloc_node(&d);
    d.nodes[top].panels[0] = PANEL_GAME;
    d.nodes[top].panel_count = 1;
    bottom = dock_alloc_node(&d);
    d.nodes[bottom].panels[0] = PANEL_EDITOR;
    d.nodes[bottom].panel_count = 1;
    split = dock_alloc_node(&d);
    d.nodes[split].type = DOCK_SPLIT_V;
    d.nodes[split].ratio = 0.5f;
    d.nodes[split].children[0] = top;
    d.nodes[split].children[1] = bottom;
    d.windows[0].in_use = 1;
    d.windows[0].root_node = split;
    dock_layout(&d, 0, 800, 828);
    div_y = d.nodes[split].y + d.nodes[split].h * 0.5f;
    ASSERT_EQ(dock_divider_at_point(&d, split, 400.0f, div_y), split);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  dock_node_at_point — boundary paths
 * ═══════════════════════════════════════════════════════════════════════ */

TEST(node_at_point_invalid_index) {
    dock_state d;
    memset(&d, 0, sizeof(d));
    ASSERT_EQ(dock_node_at_point(&d, -1, 0, 0), -1);
}

TEST(node_at_point_not_in_use) {
    dock_state d;
    memset(&d, 0, sizeof(d));
    ASSERT_EQ(dock_node_at_point(&d, 0, 0, 0), -1);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Main
 * ═══════════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("\n=== Dock Path Coverage Tests ===\n\n");

    /* dock_add_panel_with_error — 7 paths */
    run_add_panel_err_null_dock();
    run_add_panel_err_index_negative();
    run_add_panel_err_index_too_large();
    run_add_panel_err_node_not_in_use();
    run_add_panel_err_node_is_split();
    run_add_panel_err_node_full();
    run_add_panel_success();

    /* dock_remove_panel_with_error — 7 paths */
    run_remove_panel_err_null_dock();
    run_remove_panel_err_index_out_of_range();
    run_remove_panel_err_not_in_use();
    run_remove_panel_err_not_tabs();
    run_remove_panel_err_not_found();
    run_remove_panel_active_tab_no_clamp();
    run_remove_panel_active_tab_clamped();

    /* dock_split_with_error — 6 paths */
    run_split_err_null_dock();
    run_split_err_index_out_of_range();
    run_split_err_not_in_use();
    run_split_err_alloc_exhausted();
    run_split_success_horizontal();
    run_split_success_vertical();

    /* dock_split_at_with_error — 8 paths */
    run_split_at_err_null_dock();
    run_split_at_err_index_out_of_range();
    run_split_at_err_not_in_use();
    run_split_at_err_invalid_zone_none();
    run_split_at_err_invalid_zone_center();
    run_split_at_left_new_panel_is_first_child();
    run_split_at_top_new_panel_is_first_child();
    run_split_at_right_new_panel_is_second();

    /* dock_collapse_empty — 9 paths */
    run_collapse_err_null_dock();
    run_collapse_err_null_output();
    run_collapse_negative_index_returns_minus_one();
    run_collapse_index_too_large();
    run_collapse_not_in_use();
    run_collapse_tabs_nonempty_keeps_node();
    run_collapse_tabs_empty_frees_node();
    run_collapse_split_both_children_empty();
    run_collapse_split_left_empty_promote_right();
    run_collapse_split_right_empty_promote_left();
    run_collapse_split_both_alive();

    /* dock_set_active_tab — 4 paths */
    run_set_active_tab_invalid_index();
    run_set_active_tab_not_tabs_node();
    run_set_active_tab_in_range();
    run_set_active_tab_out_of_range();

    /* dock_layout — boundary paths */
    run_layout_invalid_window_index();
    run_layout_window_not_in_use();
    run_layout_vertical_split_divides_height();

    /* dock_leaf_for_panel — boundary paths */
    run_leaf_for_panel_invalid_index();
    run_leaf_for_panel_not_in_use();
    run_leaf_for_panel_not_found_in_tabs();
    run_leaf_for_panel_found_in_right_child();

    /* dock_find_leaf_for_panel_global — boundary paths */
    run_find_global_not_found();
    run_find_global_null_out_window();

    /* dock_drop_zone */
    run_drop_zone_null_node();

    /* dock_get_panel_rect — 3 paths */
    run_get_panel_rect_invalid_index();
    run_get_panel_rect_not_tabs();
    run_get_panel_rect_success();

    /* dock_free_subtree */
    run_free_subtree_invalid_index();
    run_free_subtree_frees_entire_tree();

    /* dock_window_for_sdl */
    run_window_for_sdl_null_returns_minus_one();

    /* dock_find_parent — deeper paths */
    run_find_parent_invalid_root();
    run_find_parent_tabs_node_returns_minus_one();
    run_find_parent_in_slot_zero();
    run_find_parent_in_slot_one();
    run_find_parent_null_out_slot();

    /* header_hit_test_node */
    run_header_hit_invalid_index();
    run_header_hit_split_found_in_right();

    /* dock_divider_at_point — V-split */
    run_divider_hit_on_horizontal_divider_line();

    /* dock_node_at_point — boundary */
    run_node_at_point_invalid_index();
    run_node_at_point_not_in_use();

    printf("\n=== Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) printf(", %d FAILED", tests_failed);
    printf(" ===\n");

    return tests_failed > 0 ? 1 : 0;
}
