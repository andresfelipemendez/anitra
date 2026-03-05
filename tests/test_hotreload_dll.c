/* ── Real DLL hot-reload integration tests ────────────────────────────
   Uses actual LoadLibrary/FreeLibrary to exercise the editor.dll
   hot-reload cycle end-to-end.

   The test exe acts as "externals" — it owns the memory (game_state,
   editor_state, arenas) and calls init_editor/update_editor through
   function pointers obtained via GetProcAddress.

   When FreeLibrary unloads the DLL, the OS zeros all file-scope globals
   (including Clay__currentContext and Clay__MeasureText) — exactly what
   happens in production.  LoadLibrary on the same DLL gives a fresh
   data segment, and init_editor's post-reload fixup re-wires them.

   Build:  see TCC_TEST_DLL_EDITOR_CMD + TCC_TEST_DLL_CMD in build_win32.c
   Run:    build.exe test
   ──────────────────────────────────────────────────────────────────── */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

/* Game/editor headers for struct definitions.
   We do NOT define CLAY_IMPLEMENTATION here — it's inside the DLL. */
#include "game.h"
#include "editor.h"
#include "arena.h"

/* ── Cross-platform dynamic library helpers ─────────────────────────── */

typedef void *dll_handle;

static dll_handle dll_load(const char *path) {
#ifdef _WIN32
    HMODULE h = LoadLibraryA(path);
    if (!h) fprintf(stderr, "LoadLibrary failed: %s (error %lu)\n", path, GetLastError());
    return (dll_handle)h;
#else
    void *h = dlopen(path, RTLD_NOW);
    if (!h) fprintf(stderr, "dlopen failed: %s (%s)\n", path, dlerror());
    return h;
#endif
}

static void dll_unload(dll_handle h) {
#ifdef _WIN32
    FreeLibrary((HMODULE)h);
#else
    dlclose(h);
#endif
}

static void *dll_sym(dll_handle h, const char *name) {
#ifdef _WIN32
    void *fn = (void *)GetProcAddress((HMODULE)h, name);
#else
    void *fn = dlsym(h, name);
#endif
    if (!fn) fprintf(stderr, "  [dll] symbol not found: %s\n", name);
    return fn;
}

/* ── Function pointer types (from export.h) ─────────────────────────── */

typedef void (*editor_init_fn)(game_state *gs, editor_state *es);
typedef void (*editor_destroy_fn)(game_state *gs, editor_state *es);
typedef void (*editor_update_fn)(game_state *gs, editor_state *es);
typedef int  (*editor_handle_event_fn)(game_state *gs, editor_state *es, void *event);

/* ── Editor DLL wrapper ─────────────────────────────────────────────── */

typedef struct {
    dll_handle            handle;
    editor_init_fn        init;
    editor_destroy_fn     destroy;
    editor_update_fn      update;
    editor_handle_event_fn handle_event;
} editor_dll;

static int editor_dll_load(editor_dll *ed, const char *path) {
    memset(ed, 0, sizeof(*ed));
    ed->handle = dll_load(path);
    if (!ed->handle) return -1;

    ed->init         = (editor_init_fn)        dll_sym(ed->handle, "init_editor");
    ed->destroy      = (editor_destroy_fn)     dll_sym(ed->handle, "destroy_editor");
    ed->update       = (editor_update_fn)      dll_sym(ed->handle, "update_editor");
    ed->handle_event = (editor_handle_event_fn)dll_sym(ed->handle, "editor_handle_event");

    if (!ed->init || !ed->destroy) {
        fprintf(stderr, "  [dll] missing required exports\n");
        dll_unload(ed->handle);
        ed->handle = NULL;
        return -1;
    }
    return 0;
}

static void editor_dll_unload(editor_dll *ed) {
    if (ed->handle) {
        dll_unload(ed->handle);
        ed->handle = NULL;
    }
}

/* ── Test harness ────────────────────────────────────────────────────── */

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    static void test_##name(void); \
    static void run_##name(void) { \
        tests_run++; \
        printf("  %-60s ", #name); \
        fflush(stdout); \
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
        printf("FAIL\n    %s:%d: %s == %.4f, expected %.4f\n", \
               __FILE__, __LINE__, #a, _a, _b); \
        tests_failed++; \
        tests_passed--; \
        return; \
    } \
} while(0)

/* ── Test DLL path ──────────────────────────────────────────────────── */

#ifdef _WIN32
#define TEST_EDITOR_DLL "build\\Debug\\test_editor.dll"
#else
#define TEST_EDITOR_DLL "build/Debug/test_editor.so"
#endif

/* ── Arena + state allocation ────────────────────────────────────────
   Mirrors what externals does: allocate a big chunk, set up arenas,
   initialize game_state and editor_state with valid arena pointers.

   game_state and editor_state are arena-allocated (NOT on the stack).
   editor_state alone is ~400KB which would overflow TCC's 1MB stack.
   ──────────────────────────────────────────────────────────────────── */

#define TEST_ARENA_SIZE (64 * 1024 * 1024)  /* 64 MB — room for gs, es, 50 MB editor sub-arena */

typedef struct {
    game_state   *gs;           /* arena-allocated */
    editor_state *es;           /* arena-allocated */
    arena         root_arena;   /* small (~3KB), fine on stack */
    uint8_t      *arena_backing;
} test_state;

static int test_state_init(test_state *ts) {
    memset(ts, 0, sizeof(*ts));

    /* Single heap allocation for the arena backing — just like externals */
    ts->arena_backing = (uint8_t *)malloc(TEST_ARENA_SIZE);
    if (!ts->arena_backing) return -1;
    memset(ts->arena_backing, 0, TEST_ARENA_SIZE);

    /* Initialize the root arena */
    arena_init(&ts->root_arena, ts->arena_backing, TEST_ARENA_SIZE);

    /* Allocate game_state and editor_state from the arena */
    ts->gs = (game_state *)arena_alloc(&ts->root_arena,
                sizeof(game_state), 16, "game_state");
    ts->es = (editor_state *)arena_alloc(&ts->root_arena,
                sizeof(editor_state), 16, "editor_state");
    if (!ts->gs || !ts->es) { free(ts->arena_backing); return -1; }

    /* Create editor sub-arena from root (50 MB, matching production).
       Each Clay context needs ~5 MB, so 8 * 5 = 40 MB minimum. */
    arena *ed_arena = arena_alloc_subarena(&ts->root_arena, 50 * 1024 * 1024, 16, "editor_arena");
    if (!ed_arena) { free(ts->arena_backing); return -1; }

    /* Wire arenas into editor_state — this is what externals does */
    ts->es->root_arena   = &ts->root_arena;
    ts->es->editor_arena = ed_arena;

    /* Allocate and initialize dock_state from editor_arena.
       In production, externals does this (externals_runtime.c:3237)
       then calls dock_init_default (externals_runtime.c:3263). */
    ts->es->dock = arena_alloc(ed_arena, sizeof(dock_state), 16, "dock_state");
    if (!ts->es->dock) { free(ts->arena_backing); return -1; }
    dock_init_default((dock_state *)ts->es->dock);

    /* Set up minimal game_state for init_editor to work */
    ts->gs->boot_prof_begin = NULL;  /* no profiling */
    ts->gs->boot_prof_end   = NULL;
    ts->gs->project_loaded  = 0;     /* no project — skips file I/O */
    ts->gs->project_path[0] = '\0';

    return 0;
}

static void test_state_destroy(test_state *ts) {
    free(ts->arena_backing);
    ts->arena_backing = NULL;
}

/* ── Tests ───────────────────────────────────────────────────────────── */

/* Test 1: Load DLL, call init_editor, verify Clay contexts are created */
TEST(dll_load_and_init_creates_clay_contexts) {
    test_state ts;
    ASSERT(test_state_init(&ts) == 0);

    editor_dll ed;
    ASSERT(editor_dll_load(&ed, TEST_EDITOR_DLL) == 0);

    /* First init — should create all 8 Clay contexts */
    ed.init(ts.gs, ts.es);

    ASSERT(ts.es->initialized == 1);
    ASSERT(ts.es->profiler_clay_ctx        != NULL);
    ASSERT(ts.es->scene_tree_clay_ctx      != NULL);
    ASSERT(ts.es->inspector_clay_ctx       != NULL);
    ASSERT(ts.es->project_browser_clay_ctx != NULL);
    ASSERT(ts.es->menu_bar_clay_ctx        != NULL);
    ASSERT(ts.es->editor_toolbar_clay_ctx  != NULL);
    ASSERT(ts.es->cache_prof_clay_ctx      != NULL);
    ASSERT(ts.es->cpu_prof_clay_ctx        != NULL);
    ASSERT(ts.es->clay_editor == ts.es->profiler_clay_ctx);

    editor_dll_unload(&ed);
    test_state_destroy(&ts);
}

/* Test 2: Full reload cycle — unload DLL, reload, call init again.
   This is the REAL test: FreeLibrary zeros Clay__currentContext in the
   DLL's data segment.  The second init_editor must detect e->initialized
   and re-wire the Clay callbacks. */
TEST(dll_reload_preserves_clay_contexts) {
    test_state ts;
    ASSERT(test_state_init(&ts) == 0);

    editor_dll ed;
    ASSERT(editor_dll_load(&ed, TEST_EDITOR_DLL) == 0);

    /* First init — creates Clay contexts */
    ed.init(ts.gs, ts.es);
    ASSERT(ts.es->initialized == 1);

    /* Save Clay context pointers (they live in our arena, not in DLL memory) */
    void *saved_profiler  = ts.es->profiler_clay_ctx;
    void *saved_scene     = ts.es->scene_tree_clay_ctx;
    void *saved_inspector = ts.es->inspector_clay_ctx;
    void *saved_browser   = ts.es->project_browser_clay_ctx;
    void *saved_menu      = ts.es->menu_bar_clay_ctx;
    void *saved_toolbar   = ts.es->editor_toolbar_clay_ctx;
    void *saved_cache     = ts.es->cache_prof_clay_ctx;
    void *saved_cpu       = ts.es->cpu_prof_clay_ctx;

    /* RELOAD: unload + reload the DLL.
       FreeLibrary zeros all file-scope globals in the DLL, including
       Clay__currentContext, Clay__MeasureText, Clay__QueryScrollOffset.
       LoadLibrary on the same path gives a fresh data segment. */
    editor_dll_unload(&ed);
    ASSERT(editor_dll_load(&ed, TEST_EDITOR_DLL) == 0);

    /* Second init — triggers the post-reload fixup */
    ed.init(ts.gs, ts.es);

    /* Clay context pointers must be unchanged (they live in our arena) */
    ASSERT(ts.es->profiler_clay_ctx        == saved_profiler);
    ASSERT(ts.es->scene_tree_clay_ctx      == saved_scene);
    ASSERT(ts.es->inspector_clay_ctx       == saved_inspector);
    ASSERT(ts.es->project_browser_clay_ctx == saved_browser);
    ASSERT(ts.es->menu_bar_clay_ctx        == saved_menu);
    ASSERT(ts.es->editor_toolbar_clay_ctx  == saved_toolbar);
    ASSERT(ts.es->cache_prof_clay_ctx      == saved_cache);
    ASSERT(ts.es->cpu_prof_clay_ctx        == saved_cpu);
    ASSERT(ts.es->clay_editor == saved_profiler);
    ASSERT(ts.es->initialized == 1);

    editor_dll_unload(&ed);
    test_state_destroy(&ts);
}

/* Test 3: Editor state survives the real DLL reload */
TEST(dll_reload_preserves_editor_state) {
    test_state ts;
    ASSERT(test_state_init(&ts) == 0);

    editor_dll ed;
    ASSERT(editor_dll_load(&ed, TEST_EDITOR_DLL) == 0);
    ed.init(ts.gs, ts.es);

    /* Mutate editor state (mimicking a few frames of use) */
    ts.es->cam_pos.x = 42.0f;
    ts.es->cam_pos.y = -7.5f;
    ts.es->cam_pos.z = 100.0f;
    ts.es->cam_yaw   = 2.1f;
    ts.es->cam_pitch  = -0.4f;
    ts.es->scene_selected_entity = 5;
    ts.es->scene_selection_mask[5] = 1;
    ts.es->scene_selection_count = 1;
    ts.es->prof_scroll_pos = -150.0f;

    /* RELOAD */
    editor_dll_unload(&ed);
    ASSERT(editor_dll_load(&ed, TEST_EDITOR_DLL) == 0);
    ed.init(ts.gs, ts.es);

    /* State must persist — it lives in our arena, not DLL memory */
    ASSERT_FLOAT_EQ(ts.es->cam_pos.x, 42.0f, 0.001f);
    ASSERT_FLOAT_EQ(ts.es->cam_pos.y, -7.5f, 0.001f);
    ASSERT_FLOAT_EQ(ts.es->cam_pos.z, 100.0f, 0.001f);
    ASSERT_FLOAT_EQ(ts.es->cam_yaw,   2.1f,  0.001f);
    ASSERT_FLOAT_EQ(ts.es->cam_pitch, -0.4f,  0.001f);
    ASSERT_EQ(ts.es->scene_selected_entity, 5);
    ASSERT_EQ(ts.es->scene_selection_mask[5], 1);
    ASSERT_EQ(ts.es->scene_selection_count, 1);
    ASSERT_FLOAT_EQ(ts.es->prof_scroll_pos, -150.0f, 0.001f);

    editor_dll_unload(&ed);
    test_state_destroy(&ts);
}

/* Test 4: Multiple reload cycles (10x) */
TEST(dll_ten_reload_cycles) {
    test_state ts;
    ASSERT(test_state_init(&ts) == 0);

    editor_dll ed;
    ASSERT(editor_dll_load(&ed, TEST_EDITOR_DLL) == 0);
    ed.init(ts.gs, ts.es);

    void *original_ctx = ts.es->profiler_clay_ctx;
    ASSERT(original_ctx != NULL);

    int cycle;
    for (cycle = 0; cycle < 10; cycle++) {
        editor_dll_unload(&ed);
        ASSERT(editor_dll_load(&ed, TEST_EDITOR_DLL) == 0);
        ed.init(ts.gs, ts.es);

        /* Context pointer must remain stable through every cycle */
        ASSERT(ts.es->profiler_clay_ctx == original_ctx);
        ASSERT(ts.es->initialized == 1);
    }

    editor_dll_unload(&ed);
    test_state_destroy(&ts);
}

/* Test 5: Stress test — 50 rapid reload cycles with state mutation */
TEST(dll_stress_fifty_reload_cycles) {
    test_state ts;
    ASSERT(test_state_init(&ts) == 0);

    editor_dll ed;
    ASSERT(editor_dll_load(&ed, TEST_EDITOR_DLL) == 0);
    ed.init(ts.gs, ts.es);
    ts.es->cam_pos.x = 1.0f;

    int cycle;
    for (cycle = 0; cycle < 50; cycle++) {
        /* Mutate state each cycle */
        ts.es->cam_pos.x += 1.0f;

        editor_dll_unload(&ed);
        ASSERT(editor_dll_load(&ed, TEST_EDITOR_DLL) == 0);
        ed.init(ts.gs, ts.es);

        ASSERT(ts.es->profiler_clay_ctx != NULL);
        ASSERT(ts.es->initialized == 1);
    }

    /* After 50 cycles: 1.0 + 50 * 1.0 = 51.0 */
    ASSERT_FLOAT_EQ(ts.es->cam_pos.x, 51.0f, 0.001f);

    editor_dll_unload(&ed);
    test_state_destroy(&ts);
}

/* Test 6: All 8 Clay contexts are non-NULL and distinct after reload */
TEST(dll_reload_all_eight_contexts_distinct) {
    test_state ts;
    ASSERT(test_state_init(&ts) == 0);

    editor_dll ed;
    ASSERT(editor_dll_load(&ed, TEST_EDITOR_DLL) == 0);
    ed.init(ts.gs, ts.es);

    /* Reload */
    editor_dll_unload(&ed);
    ASSERT(editor_dll_load(&ed, TEST_EDITOR_DLL) == 0);
    ed.init(ts.gs, ts.es);

    /* All 8 must be non-NULL */
    void *ctxs[] = {
        ts.es->profiler_clay_ctx,
        ts.es->scene_tree_clay_ctx,
        ts.es->inspector_clay_ctx,
        ts.es->project_browser_clay_ctx,
        ts.es->menu_bar_clay_ctx,
        ts.es->editor_toolbar_clay_ctx,
        ts.es->cache_prof_clay_ctx,
        ts.es->cpu_prof_clay_ctx,
    };
    int i, j;
    for (i = 0; i < 8; i++) {
        ASSERT(ctxs[i] != NULL);
    }
    /* All must be distinct (each is a separate subarena allocation) */
    for (i = 0; i < 8; i++) {
        for (j = i + 1; j < 8; j++) {
            ASSERT(ctxs[i] != ctxs[j]);
        }
    }

    editor_dll_unload(&ed);
    test_state_destroy(&ts);
}

/* Test 7: Dock state persists across DLL reload */
TEST(dll_reload_dock_state_persists) {
    test_state ts;
    ASSERT(test_state_init(&ts) == 0);

    editor_dll ed;
    ASSERT(editor_dll_load(&ed, TEST_EDITOR_DLL) == 0);
    ed.init(ts.gs, ts.es);

    /* Dock state is allocated in editor_arena by init_editor.
       Verify it was created. */
    ASSERT(ts.es->dock != NULL);
    dock_state *d = (dock_state *)ts.es->dock;
    ASSERT(d->initialized == 1);
    int root_before = d->windows[0].root_node;

    /* Reload */
    editor_dll_unload(&ed);
    ASSERT(editor_dll_load(&ed, TEST_EDITOR_DLL) == 0);
    ed.init(ts.gs, ts.es);

    /* Dock state pointer and content must survive */
    ASSERT(ts.es->dock != NULL);
    d = (dock_state *)ts.es->dock;
    ASSERT_EQ(d->initialized, 1);
    ASSERT_EQ(d->windows[0].root_node, root_before);

    editor_dll_unload(&ed);
    test_state_destroy(&ts);
}

/* Test 8: Double-load without unload doesn't crash
   (edge case: what if someone accidentally loads twice?) */
TEST(dll_double_load_without_crash) {
    test_state ts;
    ASSERT(test_state_init(&ts) == 0);

    editor_dll ed1, ed2;
    ASSERT(editor_dll_load(&ed1, TEST_EDITOR_DLL) == 0);
    ed1.init(ts.gs, ts.es);

    /* Load again without unloading — Windows ref-counts DLLs */
    ASSERT(editor_dll_load(&ed2, TEST_EDITOR_DLL) == 0);

    /* Both handles should work; clean up both */
    editor_dll_unload(&ed2);
    editor_dll_unload(&ed1);
    test_state_destroy(&ts);
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("\n=== Hot-Reload DLL Integration Tests ===\n\n");

    run_dll_load_and_init_creates_clay_contexts();
    run_dll_reload_preserves_clay_contexts();
    run_dll_reload_preserves_editor_state();
    run_dll_ten_reload_cycles();
    run_dll_stress_fifty_reload_cycles();
    run_dll_reload_all_eight_contexts_distinct();
    run_dll_reload_dock_state_persists();
    run_dll_double_load_without_crash();

    printf("\n=== Results ===\n");
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);
    printf("  Total:  %d\n", tests_run);

    if (tests_passed == tests_run)
        printf("  ALL TESTS PASSED\n");

    return tests_failed > 0 ? 1 : 0;
}
