/* ── Inspector integration tests ──────────────────────────────────────────
   Tests inspector view switching between entity and asset selection.
   Standalone test binary — no SDL, no GPU, no Clay needed.

   Build:  cl /Isrc /Isrc/editor tests/test_inspector.c /Fe:build/test_inspector.exe
   Run:    build/test_inspector.exe
   ────────────────────────────────────────────────────────────────────────── */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "editor.h"

/* ── Stubs for collab functions (declared in collab_client.h) ────────── */

void collab_init(collab_state *cs) { (void)cs; }
int  collab_connect(collab_state *cs, const char *host, uint16_t port)
     { (void)cs; (void)host; (void)port; return -1; }
void collab_disconnect(collab_state *cs) { (void)cs; }
void collab_update(collab_state *cs, struct game_state *gs)
     { (void)cs; (void)gs; }
void collab_emit_op(collab_state *cs, collab_op_type type,
                    int entity_index, const void *payload, uint16_t payload_len)
     { (void)cs; (void)type; (void)entity_index; (void)payload; (void)payload_len; }
void collab_send_cursor(collab_state *cs, int entity_index)
     { (void)cs; (void)entity_index; }

/* ── Minimal test harness (same as test_dock.c) ─────────────────────── */

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

/* ── Selection helpers (replicated from editor.c static functions) ──── */

static void selection_clear(editor_state *e) {
    memset(e->scene_selection_mask, 0, sizeof(e->scene_selection_mask));
    e->scene_selection_count = 0;
    e->scene_selected_entity = -1;
}

static void selection_set_single(editor_state *e, int entity_index) {
    selection_clear(e);
    if (entity_index < 0 || entity_index >= SCENE_TREE_MAX_ENTITIES) return;
    e->scene_selection_mask[entity_index] = 1;
    e->scene_selection_count = 1;
    e->scene_selected_entity = entity_index;
    e->pb_selected_asset_key[0] = '\0';
    e->pb_selected_asset_type = -1;
    if (e->collab.connected)
        collab_send_cursor(&e->collab, entity_index);
}

/* ── Inspector view decision (replicated from inspector_layout) ──────── */

static int inspector_has_asset_selected(const editor_state *e) {
    return (e->pb_selected_asset_key[0] != '\0' &&
            e->pb_selected_asset_type >= 0 &&
            e->pb_selected_asset_type <= 3);
}

static int inspector_has_entity_selected(const editor_state *e) {
    return (e->scene_selected_entity >= 0);
}

/* ── Helper: set up asset selection ──────────────────────────────────── */

static void select_asset(editor_state *e, const char *key, const char *path, int type) {
    snprintf(e->pb_selected_asset_key, sizeof(e->pb_selected_asset_key), "%s", key);
    snprintf(e->pb_selected_asset_path, sizeof(e->pb_selected_asset_path), "%s", path);
    e->pb_selected_asset_type = type;
}

/* ── Tests ───────────────────────────────────────────────────────────── */

TEST(select_asset_shows_asset_in_inspector) {
    editor_state e;
    memset(&e, 0, sizeof(e));
    e.scene_selected_entity = -1;
    e.pb_selected_asset_type = -1;

    select_asset(&e, "sword_model", "assets/models/sword.glb", 0);

    ASSERT(inspector_has_asset_selected(&e));
    ASSERT(!inspector_has_entity_selected(&e));
}

TEST(select_entity_shows_entity_in_inspector) {
    editor_state e;
    memset(&e, 0, sizeof(e));
    e.scene_selected_entity = -1;
    e.pb_selected_asset_type = -1;

    selection_set_single(&e, 3);

    ASSERT(inspector_has_entity_selected(&e));
    ASSERT(!inspector_has_asset_selected(&e));
}

TEST(select_entity_after_asset_switches_inspector_to_entity) {
    editor_state e;
    memset(&e, 0, sizeof(e));
    e.scene_selected_entity = -1;
    e.pb_selected_asset_type = -1;

    /* 1. Select an asset — inspector shows asset view */
    select_asset(&e, "sword_model", "assets/models/sword.glb", 0);
    ASSERT(inspector_has_asset_selected(&e));

    /* 2. Click an entity — inspector should switch to entity view */
    selection_set_single(&e, 5);

    /* Entity must be selected */
    ASSERT(inspector_has_entity_selected(&e));
    ASSERT_EQ(e.scene_selected_entity, 5);

    /* Asset selection must be cleared so inspector shows entity, not asset */
    ASSERT(!inspector_has_asset_selected(&e));
}

TEST(select_asset_after_entity_switches_inspector_to_asset) {
    editor_state e;
    memset(&e, 0, sizeof(e));
    e.scene_selected_entity = -1;
    e.pb_selected_asset_type = -1;

    /* 1. Select an entity */
    selection_set_single(&e, 2);
    ASSERT(inspector_has_entity_selected(&e));

    /* 2. Select an asset — inspector should switch to asset view */
    select_asset(&e, "anim_run", "assets/animations/run.glb", 1);

    ASSERT(inspector_has_asset_selected(&e));
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(void) {
    printf("\n=== Inspector Integration Tests ===\n\n");

    run_select_asset_shows_asset_in_inspector();
    run_select_entity_shows_entity_in_inspector();
    run_select_entity_after_asset_switches_inspector_to_entity();
    run_select_asset_after_entity_switches_inspector_to_asset();

    printf("\n=== Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) printf(", %d FAILED", tests_failed);
    printf(" ===\n");

    return tests_failed > 0 ? 1 : 0;
}
