# Profiler System Design

Two independent, in-editor profilers alongside the existing memory profiler.

## Overview

| Profiler | Purpose | Storage | Panel |
|----------|---------|---------|-------|
| Memory (existing) | Arena allocation visualization | In-memory | `PANEL_PROFILER` |
| Cache | Hardware counter zones (L1D, branch, etc.) | In-memory per-frame | `PANEL_CACHE_PROFILER` |
| CPU | Timing zones (Tracy-like) | Ring buffer + SQLite | `PANEL_CPU_PROFILER` |

## Cache Profiler

### Files

- `src/cache_profiler.h` — header-only module (API + platform backends via `#ifdef`)

### API

```c
cache_prof_init();                    // dlopen platform counter library
cache_zone_begin("physics");          // snapshot counters, push stack
cache_zone_end();                     // compute deltas, append to frame
cache_prof_get_zones(&zones, &count); // read sorted results
cache_prof_shutdown();                // cleanup
```

### Data Layout (SoA)

```c
#define CACHE_PROF_MAX_ZONES 32

typedef struct {
    const char *names[CACHE_PROF_MAX_ZONES];
    uint64_t    l1d_miss[CACHE_PROF_MAX_ZONES];
    uint64_t    l1i_miss[CACHE_PROF_MAX_ZONES];
    uint64_t    branch_miss[CACHE_PROF_MAX_ZONES];
    uint64_t    instructions[CACHE_PROF_MAX_ZONES];
    uint64_t    cycles[CACHE_PROF_MAX_ZONES];
    uint16_t    count;
} cache_prof_frame;
```

Results sorted descending by `l1d_miss` before display.

### macOS Backend (kpc)

- Dynamic lookup via `dlopen("libkperf.dylib")` + `dlsym`
- Functions: `kpc_set_counting`, `kpc_set_config`, `kpc_get_thread_counters`
- Counter classes: `KPC_CLASS_FIXED` (instructions, cycles) + `KPC_CLASS_CONFIGURABLE` (L1D miss, branch miss)
- PMC event IDs are chip-specific (M1 initially, comment noting M2/M3/M4 may differ)

### Graceful Degradation

- If `dlopen` or any `dlsym` fails: `cache_prof_available = 0`
- Zone macros become no-ops
- Panel shows "Hardware counters not available on this platform"

### Linux/Windows

- Stub implementations (API surface ready for `perf_event_open` / PDH)
- Zone macros are no-ops, `cache_prof_available = 0`

## CPU Profiler

### Files

- `src/cpu_profiler.h` — header-only module (API + timing + SoA storage)
- `lib/sqlite/sqlite3.c` + `sqlite3.h` — SQLite amalgamation (new dependency)

### API

```c
cpu_prof_init(const char *db_path);   // open SQLite DB, init ring buffer
cpu_zone_begin("physics");            // record timestamp, push stack
cpu_zone_end();                       // compute duration, append to frame
cpu_prof_frame_end();                 // advance ring buffer, batch-write to SQLite
cpu_prof_get_zones(&zones, &count);   // read current frame results
cpu_prof_shutdown();                  // flush + close DB
```

### Data Layout (SoA)

```c
#define CPU_PROF_MAX_ZONES  64
#define CPU_PROF_MAX_FRAMES 300  /* ~5s at 60fps */

typedef struct {
    const char *names[CPU_PROF_MAX_ZONES];
    uint64_t    start_ns[CPU_PROF_MAX_ZONES];
    uint64_t    duration_ns[CPU_PROF_MAX_ZONES];
    uint16_t    depth[CPU_PROF_MAX_ZONES];
    uint16_t    count;
} cpu_prof_frame;
```

Ring buffer: `cpu_prof_frame frames[CPU_PROF_MAX_FRAMES]` with write index.

### Timing Sources

| Platform | Function |
|----------|----------|
| macOS | `clock_gettime_nsec_np(CLOCK_UPTIME_RAW)` |
| Linux | `clock_gettime(CLOCK_MONOTONIC)` |
| Windows | `QueryPerformanceCounter` |

### SQLite Layer

- **DB location:** `build/profile.db`
- **Schema:**
  ```sql
  CREATE TABLE zones (
      frame_id    INTEGER,
      name        TEXT,
      duration_ns INTEGER,
      depth       INTEGER
  );
  CREATE INDEX idx_zones_frame ON zones(frame_id);
  ```
- **Write strategy:** Batch-write every 60 frames to avoid per-frame I/O
- **Queryable:** Find slowest frames, compare runs, aggregate by zone name

### Display

Sorted list of zones by duration (descending), rendered via Clay text rows.

## Editor Integration

### New Panel IDs

```c
typedef enum {
    PANEL_GAME,
    PANEL_EDITOR,
    PANEL_PROFILER,        /* existing memory profiler */
    PANEL_SCENE_TREE,
    PANEL_INSPECTOR,
    PANEL_OUTLINE,
    PANEL_ASSETS,
    PANEL_CACHE_PROFILER,  /* NEW */
    PANEL_CPU_PROFILER,    /* NEW */
    PANEL_COUNT
} PanelId;
```

Each new panel gets:
- Its own Clay context (allocated from `editor_arena`, survives hot-reload)
- A layout function (`cache_profiler_layout()`, `cpu_profiler_layout()`)
- Render command output (`cmd_count` + `cmd_array` in `editor_state`)
- Mouse/scroll input state in `editor_state`
- Panel name in `panel_names[]`

### editor_state Additions

```c
/* Cache profiler panel */
void *cache_prof_clay_ctx;
int   cache_prof_cmd_count;
void *cache_prof_cmd_array;
float cache_prof_mouse_x, cache_prof_mouse_y;
float cache_prof_scroll_y;
int   cache_prof_mouse_down;
int   cache_prof_click;

/* CPU profiler panel */
void *cpu_prof_clay_ctx;
int   cpu_prof_cmd_count;
void *cpu_prof_cmd_array;
float cpu_prof_mouse_x, cpu_prof_mouse_y;
float cpu_prof_scroll_y;
int   cpu_prof_mouse_down;
int   cpu_prof_click;
```

### Frame Flow

```
update_externals(memory *m)
├── timing, events
├── g_update(&m->game)            // engine update — instrumented with zones
├── g_editor_update(...)          // editor update
│   ├── profiler_layout()         // existing memory profiler
│   ├── cache_profiler_layout()   // NEW — reads cache_prof_get_zones()
│   ├── cpu_profiler_layout()     // NEW — reads cpu_prof_get_zones()
│   ├── scene_tree_layout()
│   ├── ...
│   └── cpu_prof_frame_end()      // advance ring buffer, batch SQLite write
└── rendering
```

### Zone Placement in engine.c

Cache zones and CPU zones placed independently:

```c
void update_engine(game_state *gs) {
    cpu_zone_begin("engine_update");
    cache_zone_begin("engine_update");

    cpu_zone_begin("physics");
    cache_zone_begin("physics");
    collision(gs);
    apply_movement(gs);
    cache_zone_end();
    cpu_zone_end();

    cpu_zone_begin("animation");
    cache_zone_begin("animation");
    // animation sampling + skinning
    cache_zone_end();
    cpu_zone_end();

    cache_zone_end();
    cpu_zone_end();
}
```

### Build System

- **Engine** (`src/engine/*.c`): Picks up `cache_profiler.h` and `cpu_profiler.h` automatically via glob
- **Editor** (`src/editor/editor.c`): Already includes from `src/`, no changes needed
- **Externals**: Add `lib/sqlite/sqlite3.c` to the compile commands in `build_macos.c`, `build_win32.c`, `build_linux.c`
- **SQLite**: Add `-Ilib/sqlite` to include paths

### Default Dock Layout

Add both new panels as tabs alongside the existing Profiler in the right dock:

```c
d->nodes[right].panels[0] = PANEL_PROFILER;
d->nodes[right].panels[1] = PANEL_INSPECTOR;
d->nodes[right].panels[2] = PANEL_CACHE_PROFILER;  // NEW
d->nodes[right].panels[3] = PANEL_CPU_PROFILER;     // NEW
d->nodes[right].panel_count = 4;
```

## Global State

Both profilers use file-scope globals inside their headers, gated by an `IMPL` define:

```c
// In exactly one .c file (externals_runtime.c):
#define CACHE_PROF_IMPL
#include "cache_profiler.h"
#define CPU_PROF_IMPL
#include "cpu_profiler.h"

// Everywhere else (engine.c, editor.c):
#include "cache_profiler.h"  // zone macros only
#include "cpu_profiler.h"
```

This follows the stb-style single-header pattern. The `IMPL` translation unit owns the global state. Other translation units get `extern` declarations or inline no-op macros.

**Cross-DLL consideration:** Since engine.dll and externals.dll are separate, the zone macros in engine.c need to call through function pointers assigned at init (same pattern as `assign_update` / `assign_editor_update`). The `IMPL` lives in externals, and engine gets thin wrapper functions exported from externals.
