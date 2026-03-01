# Profiler System Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add two independent in-editor profilers — a cache miss profiler (hardware counters) and a CPU profiler (timing + SQLite) — each with its own dockable panel.

**Architecture:** Two header-only modules (`cache_profiler.h`, `cpu_profiler.h`) using stb-style `IMPL` defines. State lives in externals (the `IMPL` translation unit). Engine calls zone functions through exported function pointers (cross-DLL). Each profiler gets a new `PanelId`, Clay context, layout function, and render pass — following the exact pattern of the existing profiler/scene-tree/inspector panels.

**Tech Stack:** C17, kpc framework (macOS hardware counters via dlsym), SQLite amalgamation, Clay UI, SDL3 GPU

---

### Task 1: Add panel IDs and editor_state fields

**Files:**
- Modify: `src/editor_types.h:7-16`
- Modify: `src/editor/editor.h:77-85` (panel_names)
- Modify: `src/editor/editor.h:684-811` (editor_state struct)

**Step 1: Add new PanelId entries**

In `src/editor_types.h`, add `PANEL_CACHE_PROFILER` and `PANEL_CPU_PROFILER` before `PANEL_COUNT`:

```c
typedef enum {
    PANEL_GAME,
    PANEL_EDITOR,
    PANEL_PROFILER,
    PANEL_SCENE_TREE,
    PANEL_INSPECTOR,
    PANEL_OUTLINE,
    PANEL_ASSETS,
    PANEL_CACHE_PROFILER,
    PANEL_CPU_PROFILER,
    PANEL_COUNT
} PanelId;
```

**Step 2: Add panel display names**

In `src/editor/editor.h:77-85`, add two entries to `panel_names[]`:

```c
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
```

**Step 3: Add editor_state fields**

In `src/editor/editor.h`, before the closing `dock_state *dock;` field (line 810), add:

```c
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
```

**Step 4: Add panels to default dock layout**

In `src/editor/editor.h:277-282`, add the new panels as tabs in the right dock node:

```c
    /* Right leaf: Profiler + Inspector + Cache Profiler + CPU Profiler tabs */
    right = dock_alloc_node(d);
    d->nodes[right].panels[0] = PANEL_PROFILER;
    d->nodes[right].panels[1] = PANEL_INSPECTOR;
    d->nodes[right].panels[2] = PANEL_CACHE_PROFILER;
    d->nodes[right].panels[3] = PANEL_CPU_PROFILER;
    d->nodes[right].panel_count = 4;
    d->nodes[right].active_tab = 1;
```

**Step 5: Build and verify**

Run: `./build.sh`
Expected: Compiles without errors. The two new panels exist but have no content yet.

**Step 6: Commit**

```
feat: add cache profiler and CPU profiler panel IDs
```

---

### Task 2: Create cache_profiler.h — data structures and stub API

**Files:**
- Create: `src/cache_profiler.h`

**Step 1: Write the header**

Create `src/cache_profiler.h` with:
- Include guard
- SoA data structure `cache_prof_frame`
- Zone stack for nesting
- Full API declarations
- Platform detection: macOS kpc backend, stubs elsewhere
- `CACHE_PROF_IMPL` section with global state and function bodies
- Non-IMPL section with `extern` declarations
- macOS: `dlopen("libkperf.dylib")` + `dlsym` for `kpc_set_counting`, `kpc_set_config`, `kpc_get_thread_counters`, `kpc_get_counter_count`
- `cache_zone_begin`/`cache_zone_end`: snapshot counters before/after, compute deltas, append to frame SoA arrays
- `cache_prof_sort_zones`: insertion sort by `l1d_miss` descending
- Graceful degradation: if dlopen/dlsym fails, `available = 0`, zone functions become no-ops

Key constants:
```c
#define CACHE_PROF_MAX_ZONES 32
#define CACHE_PROF_MAX_STACK 16

/* kpc constants */
#define KPC_CLASS_FIXED          (1u << 0)
#define KPC_CLASS_CONFIGURABLE   (1u << 1)
#define KPC_MAX_COUNTERS         16
```

Key structs:
```c
typedef struct {
    const char *names[CACHE_PROF_MAX_ZONES];
    uint64_t    l1d_miss[CACHE_PROF_MAX_ZONES];
    uint64_t    l1i_miss[CACHE_PROF_MAX_ZONES];
    uint64_t    branch_miss[CACHE_PROF_MAX_ZONES];
    uint64_t    instructions[CACHE_PROF_MAX_ZONES];
    uint64_t    cycles[CACHE_PROF_MAX_ZONES];
    uint16_t    count;
} cache_prof_frame;

typedef struct {
    uint64_t counters[KPC_MAX_COUNTERS];
} cache_prof_snapshot;
```

API functions:
```c
void cache_prof_init(void);
void cache_prof_shutdown(void);
void cache_zone_begin(const char *name);
void cache_zone_end(void);
void cache_prof_frame_reset(void);
int  cache_prof_available(void);
cache_prof_frame *cache_prof_get_frame(void);
```

**Step 2: Build and verify**

Run: `./build.sh`
Expected: Compiles. Header is not included anywhere yet, so this just validates syntax.

**Step 3: Commit**

```
feat: add cache_profiler.h with kpc backend and SoA data layout
```

---

### Task 3: Create cpu_profiler.h — data structures and stub API

**Files:**
- Create: `src/cpu_profiler.h`

**Step 1: Write the header**

Create `src/cpu_profiler.h` with:
- Include guard
- SoA data structure `cpu_prof_frame`
- Ring buffer of frames
- Zone stack for nesting
- Platform-specific timing: `clock_gettime_nsec_np` (macOS), `clock_gettime` (Linux), `QueryPerformanceCounter` (Windows)
- `CPU_PROF_IMPL` section with global state and function bodies
- Non-IMPL section with `extern` declarations
- No SQLite yet — just the timing core

Key constants:
```c
#define CPU_PROF_MAX_ZONES  64
#define CPU_PROF_MAX_FRAMES 300
#define CPU_PROF_MAX_STACK  16
```

Key structs:
```c
typedef struct {
    const char *names[CPU_PROF_MAX_ZONES];
    uint64_t    start_ns[CPU_PROF_MAX_ZONES];
    uint64_t    duration_ns[CPU_PROF_MAX_ZONES];
    uint16_t    depth[CPU_PROF_MAX_ZONES];
    uint16_t    count;
} cpu_prof_frame;
```

API functions:
```c
void cpu_prof_init(void);
void cpu_prof_shutdown(void);
void cpu_zone_begin(const char *name);
void cpu_zone_end(void);
void cpu_prof_frame_end(void);
cpu_prof_frame *cpu_prof_get_frame(void);
```

Platform timing function (static inline in header):
```c
static inline uint64_t cpu_prof_now_ns(void) {
#if defined(__APPLE__)
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#elif defined(_WIN32)
    /* QueryPerformanceCounter path */
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}
```

**Step 2: Build and verify**

Run: `./build.sh`
Expected: Compiles.

**Step 3: Commit**

```
feat: add cpu_profiler.h with cross-platform timing and SoA ring buffer
```

---

### Task 4: Wire profilers into externals (IMPL + init/shutdown)

**Files:**
- Modify: `src/externals/externals_runtime.c:12` (includes area)
- Modify: `src/externals/externals_runtime.c` (init_externals function)
- Modify: `src/externals/externals_runtime.c` (destroy_externals function)

**Step 1: Add IMPL includes**

Near the top of `externals_runtime.c` (after the existing `#include` block around line 12), add:

```c
#define CACHE_PROF_IMPL
#include "cache_profiler.h"
#define CPU_PROF_IMPL
#include "cpu_profiler.h"
```

**Step 2: Add init/shutdown calls**

In `init_externals` (find the existing init function), add after GPU device creation:

```c
cache_prof_init();
cpu_prof_init();
```

In `destroy_externals`, add before GPU device release:

```c
cache_prof_shutdown();
cpu_prof_shutdown();
```

**Step 3: Add UIRenderState for new panels**

Near line 845-848 where the existing `ui_profiler`, `ui_scene_tree`, etc. are declared, add:

```c
static UIRenderState ui_cache_profiler = {0};
static UIRenderState ui_cpu_profiler = {0};
```

**Step 4: Build and verify**

Run: `./build.sh`
Expected: Compiles. Profilers initialize at startup and shut down at exit.

**Step 5: Commit**

```
feat: wire cache and CPU profiler init/shutdown into externals
```

---

### Task 5: Export zone functions for cross-DLL use

**Files:**
- Modify: `src/export.h`
- Modify: `src/externals/externals_runtime.c`
- Modify: `src/engine/engine.c`
- Modify: `src/core/core.c`

**Step 1: Add function pointer typedefs to export.h**

Add after the existing `DECLARE_` macros (around line 121):

```c
/* Profiler zone function pointers — called by engine.dll, implemented in externals.dll */
typedef void (*cache_zone_begin_fn)(const char *name);
typedef void (*cache_zone_end_fn)(void);
typedef void (*cpu_zone_begin_fn)(const char *name);
typedef void (*cpu_zone_end_fn)(void);
```

**Step 2: Add exported wrapper functions in externals_runtime.c**

Since externals owns the IMPL, add thin wrappers that engine can call through function pointers:

```c
EXPORT void ext_cache_zone_begin(const char *name) { cache_zone_begin(name); }
EXPORT void ext_cache_zone_end(void) { cache_zone_end(); }
EXPORT void ext_cpu_zone_begin(const char *name) { cpu_zone_begin(name); }
EXPORT void ext_cpu_zone_end(void) { cpu_zone_end(); }
```

**Step 3: Add function pointer storage in engine.c**

In `src/engine/engine.c`, add near the top:

```c
/* Profiler zone function pointers — set by core.dll, call into externals.dll */
static void (*pfn_cache_zone_begin)(const char *name) = NULL;
static void (*pfn_cache_zone_end)(void) = NULL;
static void (*pfn_cpu_zone_begin)(const char *name) = NULL;
static void (*pfn_cpu_zone_end)(void) = NULL;

EXPORT void assign_profiler_fns(
    void (*czb)(const char *), void (*cze)(void),
    void (*cpzb)(const char *), void (*cpze)(void))
{
    pfn_cache_zone_begin = czb;
    pfn_cache_zone_end = cze;
    pfn_cpu_zone_begin = cpzb;
    pfn_cpu_zone_end = cpze;
}

#define ENGINE_CACHE_ZONE_BEGIN(name) do { if (pfn_cache_zone_begin) pfn_cache_zone_begin(name); } while(0)
#define ENGINE_CACHE_ZONE_END()      do { if (pfn_cache_zone_end) pfn_cache_zone_end(); } while(0)
#define ENGINE_CPU_ZONE_BEGIN(name)  do { if (pfn_cpu_zone_begin) pfn_cpu_zone_begin(name); } while(0)
#define ENGINE_CPU_ZONE_END()        do { if (pfn_cpu_zone_end) pfn_cpu_zone_end(); } while(0)
```

**Step 4: Wire up in core.c**

In `src/core/core.c`, after loading engine and externals DLLs, look up the functions and connect them:

```c
/* After loading engine and externals DLLs: */
void (*ext_czb)(const char *) = getfunction(externals_lib, "ext_cache_zone_begin");
void (*ext_cze)(void)         = getfunction(externals_lib, "ext_cache_zone_end");
void (*ext_cpzb)(const char *)= getfunction(externals_lib, "ext_cpu_zone_begin");
void (*ext_cpze)(void)        = getfunction(externals_lib, "ext_cpu_zone_end");

typedef void (*assign_profiler_fns_t)(void(*)(const char*), void(*)(void), void(*)(const char*), void(*)(void));
assign_profiler_fns_t assign_pfns = getfunction(engine_lib, "assign_profiler_fns");
if (assign_pfns) assign_pfns(ext_czb, ext_cze, ext_cpzb, ext_cpze);
```

**Step 5: Build and verify**

Run: `./build.sh`
Expected: Compiles. Cross-DLL zone dispatch is wired up but not called yet.

**Step 6: Commit**

```
feat: export profiler zone functions for cross-DLL dispatch
```

---

### Task 6: Instrument engine.c with profiler zones

**Files:**
- Modify: `src/engine/engine.c:1416-1467` (update_engine function)

**Step 1: Add zones to update_engine**

Wrap the key sections of `update_engine()` (line 1416) with zone calls:

```c
EXPORT void update_engine(game_state *gs) {
    int i;
    int anim_updated = 0;
    if (!gs) return;

    ENGINE_CPU_ZONE_BEGIN("update_engine");
    ENGINE_CACHE_ZONE_BEGIN("update_engine");

    gs->dl.sprite_count = 0;
    gs->dl.line_count = 0;
    gs->dl.mesh_count = 0;
    gs->dbg.current_line_count = 0;

    if (gs->editor_play_mode) {
        ENGINE_CPU_ZONE_BEGIN("input");
        update_input(gs);
        ENGINE_CPU_ZONE_END();

        ENGINE_CPU_ZONE_BEGIN("physics");
        ENGINE_CACHE_ZONE_BEGIN("physics");
        run_character_controller_system(gs);
        apply_movement(gs);
        collision(gs);
        ENGINE_CACHE_ZONE_END();
        ENGINE_CPU_ZONE_END();
    }

    ENGINE_CPU_ZONE_BEGIN("mesh_sync");
    sync_primary_mesh3d_asset(gs);
    sync_mesh_camera_from_components(gs);
    build_mesh_draw_commands(gs);
    ENGINE_CPU_ZONE_END();

    if (gs->editor_play_mode) {
        ENGINE_CPU_ZONE_BEGIN("animation");
        ENGINE_CACHE_ZONE_BEGIN("animation");
        if (gs->animation_components) {
            animation_component *ac = query_primary_animation_component_for_mesh(gs);
            if (ac) {
                update_mesh3d_from_animation_component(gs, ac);
                anim_updated = 1;
            }
        }
        if (!anim_updated) {
            animation_component fallback = {0, 1, (int)gs->mesh3d.active_clip, gs->mesh3d.anim_time, 1.0f};
            update_mesh3d_from_animation_component(gs, &fallback);
        }
        ENGINE_CACHE_ZONE_END();
        ENGINE_CPU_ZONE_END();
    } else {
        gs->animation_transition_count = 0;
    }

    for (i = 0; i < gs->dbg.current_line_count; i++) {
        int idx;
        debug_line_command* line;
        if (gs->dl.line_count >= gs->dl.line_capacity) break;
        idx = i * 10;
        line = &gs->dl.lines[gs->dl.line_count++];
        line->x1 = gs->dbg.vertex_buffer[idx];
        line->y1 = gs->dbg.vertex_buffer[idx+1];
        line->r = gs->dbg.vertex_buffer[idx+2];
        line->g = gs->dbg.vertex_buffer[idx+3];
        line->b = gs->dbg.vertex_buffer[idx+4];
        line->x2 = gs->dbg.vertex_buffer[idx+5];
        line->y2 = gs->dbg.vertex_buffer[idx+6];
    }

    ENGINE_CACHE_ZONE_END();
    ENGINE_CPU_ZONE_END();
}
```

**Step 2: Build and verify**

Run: `./build.sh`
Expected: Compiles. Zones are called each frame (no-ops if function pointers are NULL).

**Step 3: Commit**

```
feat: instrument engine update with cache and CPU profiler zones
```

---

### Task 7: Add frame reset and frame end to the update loop

**Files:**
- Modify: `src/externals/externals_runtime.c` (update_externals function, around line 2431)

**Step 1: Add frame boundaries**

In `update_externals()`, add frame reset before `g_update` and frame end after editor update:

```c
// Before g_update (around line 2670):
cache_prof_frame_reset();

// After g_editor_update (around line 2646):
cpu_prof_frame_end();
```

**Step 2: Build and verify**

Run: `./build.sh`
Expected: Compiles. Each frame resets cache profiler zones and advances CPU profiler ring buffer.

**Step 3: Commit**

```
feat: add profiler frame boundaries to update loop
```

---

### Task 8: Add Clay context init and event handling for new panels

**Files:**
- Modify: `src/editor/editor.c` (init_editor function, around line 2130)
- Modify: `src/editor/editor.c` (handle_editor_event function, around line 4549-4648)

**Step 1: Add Clay context initialization**

In `init_editor()`, after the existing Clay context init blocks (after line 2206), add two new blocks following the exact pattern:

```c
    if (!e->cache_prof_clay_ctx && es->editor_arena) {
        uint32_t clay_size = (uint32_t)Clay_MinMemorySize();
        arena *clay_sub = arena_alloc_subarena(es->editor_arena, clay_size, 16, "clay_cache_prof");
        if (clay_sub) {
            Clay_Arena ca = Clay_CreateArenaWithCapacityAndMemory(clay_size, clay_sub->base);
            Clay_ErrorHandler err = {0};
            e->cache_prof_clay_ctx = Clay_Initialize(ca, (Clay_Dimensions){800, 600}, err);
            Clay_SetCurrentContext((Clay_Context *)e->cache_prof_clay_ctx);
            Clay_SetMeasureTextFunction(profiler_measure_text, e);
        }
    }

    if (!e->cpu_prof_clay_ctx && es->editor_arena) {
        uint32_t clay_size = (uint32_t)Clay_MinMemorySize();
        arena *clay_sub = arena_alloc_subarena(es->editor_arena, clay_size, 16, "clay_cpu_prof");
        if (clay_sub) {
            Clay_Arena ca = Clay_CreateArenaWithCapacityAndMemory(clay_size, clay_sub->base);
            Clay_ErrorHandler err = {0};
            e->cpu_prof_clay_ctx = Clay_Initialize(ca, (Clay_Dimensions){800, 600}, err);
            Clay_SetCurrentContext((Clay_Context *)e->cpu_prof_clay_ctx);
            Clay_SetMeasureTextFunction(profiler_measure_text, e);
        }
    }
```

**Step 2: Add event handling**

In `handle_editor_event()`, in the mouse motion section (around line 4549), add hit testing for both new panels:

```c
            if (panel_event_hit(d, PANEL_CACHE_PROFILER, evwin, ev->motion.x, ev->motion.y, &lx, &ly)) {
                e->cache_prof_mouse_x = lx;
                e->cache_prof_mouse_y = ly;
            }
            if (panel_event_hit(d, PANEL_CPU_PROFILER, evwin, ev->motion.x, ev->motion.y, &lx, &ly)) {
                e->cpu_prof_mouse_x = lx;
                e->cpu_prof_mouse_y = ly;
            }
```

In the scroll section (around line 4601), add:

```c
            if (panel_event_hit(d, PANEL_CACHE_PROFILER, evwin, mx, my, NULL, NULL)) {
                e->cache_prof_scroll_y += ev->wheel.y * 3.0f;
                consumed = 1;
            }
            if (panel_event_hit(d, PANEL_CPU_PROFILER, evwin, mx, my, NULL, NULL)) {
                e->cpu_prof_scroll_y += ev->wheel.y * 3.0f;
                consumed = 1;
            }
```

In the mouse-down section (around line 4616), add:

```c
            e->cache_prof_mouse_down = panel_event_hit(d, PANEL_CACHE_PROFILER, evwin,
                ev->button.x, ev->button.y, &lx, &ly);
            if (e->cache_prof_mouse_down) {
                e->cache_prof_mouse_x = lx;
                e->cache_prof_mouse_y = ly;
                e->cache_prof_click = 1;
            }

            e->cpu_prof_mouse_down = panel_event_hit(d, PANEL_CPU_PROFILER, evwin,
                ev->button.x, ev->button.y, &lx, &ly);
            if (e->cpu_prof_mouse_down) {
                e->cpu_prof_mouse_x = lx;
                e->cpu_prof_mouse_y = ly;
                e->cpu_prof_click = 1;
            }
```

In the mouse-up section (around line 4645), add:

```c
        e->cache_prof_mouse_down = 0;
        e->cpu_prof_mouse_down = 0;
```

**Step 3: Build and verify**

Run: `./build.sh`
Expected: Compiles. Clay contexts are created, events are dispatched to new panels.

**Step 4: Commit**

```
feat: add Clay context and event handling for profiler panels
```

---

### Task 9: Add cache_profiler_layout() — Clay UI

**Files:**
- Modify: `src/editor/editor.c` (add layout function before `update_editor`, add `#include "cache_profiler.h"` at top)

**Step 1: Add include**

At the top of `editor.c`, add (without IMPL define — editor only reads data):

```c
#include "cache_profiler.h"
```

**Step 2: Write the layout function**

Add `cache_profiler_layout()` before `update_editor()` (around line 3822). Follow the pattern of `profiler_layout()` but simpler — no grid, just a sorted text list.

The function should:
1. Get the Clay context from `e->cache_prof_clay_ctx`
2. Find dock node for `PANEL_CACHE_PROFILER`
3. Set layout dimensions, pointer state, scroll state
4. `Clay_BeginLayout()`
5. Read the frame via `cache_prof_get_frame()`
6. If `cache_prof_available()` is false, show "Hardware counters not available"
7. Otherwise, render sorted zone rows:
   - Title: "Cache Profiler"
   - Column headers: Name | L1D Miss | Branch Miss | Instructions | Cycles
   - One row per zone with `snprintf` formatting, sorted by `l1d_miss` descending
8. `Clay_EndLayout()` -> store commands in `e->cache_prof_cmd_count/array`
9. Consume `cache_prof_click` and `cache_prof_scroll_y`

**Step 3: Call from update_editor**

In `update_editor()` (around line 3884), add after `profiler_layout`:

```c
    cache_profiler_layout(gs, es);
```

**Step 4: Build and verify**

Run: `./build.sh`
Expected: Compiles. Panel renders "Hardware counters not available" on non-macOS, or zone data on macOS.

**Step 5: Commit**

```
feat: add cache profiler panel Clay layout
```

---

### Task 10: Add cpu_profiler_layout() — Clay UI

**Files:**
- Modify: `src/editor/editor.c` (add layout function, add `#include "cpu_profiler.h"` at top)

**Step 1: Add include**

At the top of `editor.c`, add (without IMPL define):

```c
#include "cpu_profiler.h"
```

**Step 2: Write the layout function**

Add `cpu_profiler_layout()` following the same pattern as Task 9. This function:
1. Get the Clay context from `e->cpu_prof_clay_ctx`
2. Find dock node for `PANEL_CPU_PROFILER`
3. Set layout dimensions, pointer state, scroll state
4. `Clay_BeginLayout()`
5. Read the frame via `cpu_prof_get_frame()`
6. Sort zones by `duration_ns` descending
7. Render rows:
   - Title: "CPU Profiler"
   - Column headers: Name | Duration | Depth
   - Duration formatted as us or ms depending on magnitude
   - One row per zone
8. `Clay_EndLayout()` -> store commands
9. Consume click and scroll state

**Step 3: Call from update_editor**

In `update_editor()`, add after `cache_profiler_layout`:

```c
    cpu_profiler_layout(gs, es);
```

**Step 4: Build and verify**

Run: `./build.sh`
Expected: Compiles. Panel renders zone timing data.

**Step 5: Commit**

```
feat: add CPU profiler panel Clay layout
```

---

### Task 11: Add prepare + render passes in externals

**Files:**
- Modify: `src/externals/externals_runtime.c` (add prepare functions + render passes)

**Step 1: Add prepare functions**

After `inspector_prepare()` (around line 1275), add two new prepare functions following the `scene_tree_prepare` pattern:

```c
static void cache_profiler_prepare(SDL_GPUCommandBuffer *cmd_buf, memory *g) {
    if (g->editor.cache_prof_cmd_count <= 0 || !g->editor.cache_prof_cmd_array)
        return;
    Clay_RenderCommandArray commands;
    commands.length = g->editor.cache_prof_cmd_count;
    commands.internalArray = (Clay_RenderCommand *)g->editor.cache_prof_cmd_array;
    ui_build_vertices(&ui_cache_profiler, commands);
    ui_upload(cmd_buf, &ui_cache_profiler);
}

static void cpu_profiler_prepare(SDL_GPUCommandBuffer *cmd_buf, memory *g) {
    if (g->editor.cpu_prof_cmd_count <= 0 || !g->editor.cpu_prof_cmd_array)
        return;
    Clay_RenderCommandArray commands;
    commands.length = g->editor.cpu_prof_cmd_count;
    commands.internalArray = (Clay_RenderCommand *)g->editor.cpu_prof_cmd_array;
    ui_build_vertices(&ui_cpu_profiler, commands);
    ui_upload(cmd_buf, &ui_cpu_profiler);
}
```

**Step 2: Call prepare in update_externals**

In the prepare section (around line 2728-2745), add:

```c
    if (panel_color[PANEL_CACHE_PROFILER]) {
        cache_profiler_prepare(cmd_buf, m);
    }
    if (panel_color[PANEL_CPU_PROFILER]) {
        cpu_profiler_prepare(cmd_buf, m);
    }
```

**Step 3: Add render passes**

After the inspector render pass (around line 3310), add two new render passes following the `SCENE_TREE` pattern:

```c
    // --- CACHE PROFILER PANEL RENDER PASS (offscreen) ---
    if (panel_color[PANEL_CACHE_PROFILER] && panel_depth[PANEL_CACHE_PROFILER]) {
        Uint32 cpw = (Uint32)panel_tex_w[PANEL_CACHE_PROFILER];
        Uint32 cph = (Uint32)panel_tex_h[PANEL_CACHE_PROFILER];

        SDL_GPUColorTargetInfo cp_ct = {0};
        cp_ct.texture = panel_color[PANEL_CACHE_PROFILER];
        cp_ct.clear_color = (SDL_FColor){0.10f, 0.10f, 0.12f, 1.0f};
        cp_ct.load_op = SDL_GPU_LOADOP_CLEAR;
        cp_ct.store_op = SDL_GPU_STOREOP_STORE;

        SDL_GPUDepthStencilTargetInfo cp_dt = {0};
        cp_dt.texture = panel_depth[PANEL_CACHE_PROFILER];
        cp_dt.clear_depth = 1.0f;
        cp_dt.load_op = SDL_GPU_LOADOP_CLEAR;
        cp_dt.store_op = SDL_GPU_STOREOP_DONT_CARE;
        cp_dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        cp_dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;

        SDL_GPURenderPass *cp_pass = SDL_BeginGPURenderPass(cmd_buf, &cp_ct, 1, &cp_dt);
        {
            uniform_data cp_uniforms;
            float cp_lw = (float)cpw / display_density;
            float cp_lh = (float)cph / display_density;
            float cp_ortho[16] = {
                2.0f/cp_lw,    0,             0,     0,
                0,            -2.0f/cp_lh,    0,     0,
                0,             0,            -1.0f,  0,
               -1.0f,          1.0f,          0,     1.0f
            };
            float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            memcpy(cp_uniforms.projection, cp_ortho, sizeof(cp_ortho));
            memcpy(cp_uniforms.view, identity, sizeof(identity));
            ui_draw(cp_pass, cmd_buf, &cp_uniforms, &ui_cache_profiler);
        }
        SDL_EndGPURenderPass(cp_pass);
    }

    // --- CPU PROFILER PANEL RENDER PASS (offscreen) ---
    if (panel_color[PANEL_CPU_PROFILER] && panel_depth[PANEL_CPU_PROFILER]) {
        Uint32 cpuw = (Uint32)panel_tex_w[PANEL_CPU_PROFILER];
        Uint32 cpuh = (Uint32)panel_tex_h[PANEL_CPU_PROFILER];

        SDL_GPUColorTargetInfo cpu_ct = {0};
        cpu_ct.texture = panel_color[PANEL_CPU_PROFILER];
        cpu_ct.clear_color = (SDL_FColor){0.10f, 0.10f, 0.12f, 1.0f};
        cpu_ct.load_op = SDL_GPU_LOADOP_CLEAR;
        cpu_ct.store_op = SDL_GPU_STOREOP_STORE;

        SDL_GPUDepthStencilTargetInfo cpu_dt = {0};
        cpu_dt.texture = panel_depth[PANEL_CPU_PROFILER];
        cpu_dt.clear_depth = 1.0f;
        cpu_dt.load_op = SDL_GPU_LOADOP_CLEAR;
        cpu_dt.store_op = SDL_GPU_STOREOP_DONT_CARE;
        cpu_dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        cpu_dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;

        SDL_GPURenderPass *cpu_pass = SDL_BeginGPURenderPass(cmd_buf, &cpu_ct, 1, &cpu_dt);
        {
            uniform_data cpu_uniforms;
            float cpu_lw = (float)cpuw / display_density;
            float cpu_lh = (float)cpuh / display_density;
            float cpu_ortho[16] = {
                2.0f/cpu_lw,    0,              0,     0,
                0,             -2.0f/cpu_lh,    0,     0,
                0,              0,             -1.0f,  0,
               -1.0f,           1.0f,           0,     1.0f
            };
            float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            memcpy(cpu_uniforms.projection, cpu_ortho, sizeof(cpu_ortho));
            memcpy(cpu_uniforms.view, identity, sizeof(identity));
            ui_draw(cpu_pass, cmd_buf, &cpu_uniforms, &ui_cpu_profiler);
        }
        SDL_EndGPURenderPass(cpu_pass);
    }
```

**Step 4: Build and verify**

Run: `./build.sh`
Expected: Compiles. Both panels render their Clay UI content.

**Step 5: Commit**

```
feat: add prepare and render passes for profiler panels
```

---

### Task 12: Add SQLite and cpu_profiler persistence

**Files:**
- Create: `lib/sqlite/sqlite3.c` + `lib/sqlite/sqlite3.h` (SQLite amalgamation download)
- Modify: `src/cpu_profiler.h` (add SQLite batch-write)
- Modify: `build.c:763-786` (add sqlite3.c to externals compile, all platforms)

**Step 1: Download SQLite amalgamation**

Download `sqlite3.c` and `sqlite3.h` into `lib/sqlite/`.

**Step 2: Add SQLite to build commands**

In `build.c`, in the externals build section (the `snprintf` calls around lines 757, 772, 783), add:
- ` -Ilib/sqlite` to the include flags
- ` lib/sqlite/sqlite3.c` to the source files

**Step 3: Add SQLite to cpu_profiler.h**

In the `CPU_PROF_IMPL` section, add SQLite integration:

```c
#include "sqlite3.h"

static sqlite3 *cpu_prof_db = NULL;
static sqlite3_stmt *cpu_prof_insert_stmt = NULL;
static uint64_t cpu_prof_frame_id = 0;
#define CPU_PROF_BATCH_SIZE 60
```

Add `cpu_prof_db_init()` called from `cpu_prof_init()`:
- Opens `build/profile.db`
- Creates `zones` table and index if not exists
- Prepares insert statement

Add `cpu_prof_db_flush()` called from `cpu_prof_frame_end()` every `CPU_PROF_BATCH_SIZE` frames:
- BEGIN transaction
- Insert all zone records from the last batch
- COMMIT transaction

Add `cpu_prof_db_shutdown()` called from `cpu_prof_shutdown()`:
- Final flush
- Finalize statement
- Close database

**Step 4: Build and verify**

Run: `./build.sh`
Expected: Compiles. `build/profile.db` is created on first run.

**Step 5: Commit**

```
feat: add SQLite persistence for CPU profiler
```

---

### Task 13: Manual smoke test

**Step 1: Run the engine**

Run: `./build.sh` then run the engine executable.

**Step 2: Verify panels exist**

- Check that "Cache Profiler" and "CPU Profiler" tabs appear in the right dock
- Click each tab and verify content renders
- Cache Profiler: should show zone data sorted by L1D misses (macOS) or "not available" message
- CPU Profiler: should show zone timing data sorted by duration

**Step 3: Verify SQLite**

Query the database to verify data is being written:
```
sqlite3 build/profile.db "SELECT name, AVG(duration_ns)/1000 as avg_us FROM zones GROUP BY name ORDER BY avg_us DESC;"
```

Expected: Shows average microseconds per zone name.

**Step 4: Verify hot-reload**

- Edit engine.c (add a comment)
- Wait for file watcher to trigger rebuild
- Verify profiler panels still work after reload

**Step 5: Commit any fixes**

---

## Summary of files touched

| File | Action |
|------|--------|
| `src/editor_types.h` | Add 2 PanelId entries |
| `src/editor/editor.h` | Add panel names + editor_state fields + default dock |
| `src/editor/editor.c` | Add Clay init, event handling, 2 layout functions, includes |
| `src/cache_profiler.h` | Create — header-only, kpc backend, SoA |
| `src/cpu_profiler.h` | Create — header-only, timing, ring buffer, SQLite |
| `src/externals/externals_runtime.c` | IMPL includes, init/shutdown, UIRenderState, prepare, render passes, frame boundaries, export zone fns |
| `src/engine/engine.c` | Zone instrumentation, assign_profiler_fns, wrapper macros |
| `src/core/core.c` | Wire zone function pointers between DLLs |
| `src/export.h` | Zone function pointer typedefs |
| `build.c` | Add sqlite3.c to externals source list + include path |
| `lib/sqlite/sqlite3.c` | Download — SQLite amalgamation |
| `lib/sqlite/sqlite3.h` | Download — SQLite amalgamation |
