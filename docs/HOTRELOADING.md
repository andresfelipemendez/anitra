# Hot Reloading Guide

Anitra Engine uses hot reloading to enable live code iteration. This document explains how it works and how to use it.

## How It Works

The engine implements a **three-tier DLL architecture**:

```
game.exe (main loop)
    ├── core.dll     (hot-reload coordinator)
    │   ├── externals.dll  (SDL3/GPU/UI - loaded once)
    │   ├── engine.dll     (gameplay logic - hot-reloadable)
    │   └── editor.dll     (UI panels - hot-reloadable)
```

### Hot Reload Workflow

1. **Game Loop** (`main.c`): Runs continuously, monitoring for reload events
2. **Reload Trigger**: Press `Ctrl+R` or send Windows event to trigger reload
3. **Tear-down Phase**: 
   - Call `destroy_engine()`
   - Call `end_externals()` (only for engine/editor)
4. **DLL Reload**:
   - Copy DLL files to `_copy` variants (bypasses file locks)
   - Load new DLL versions
5. **Re-initialize**:
   - Call `init_engine()`
   - Call `init_externals()` (if needed)

### Memory Persistence

Memory allocated in the **gameplay arena** survives reloads:

```c
typedef struct memory {
    arena arena;          // Main scratch arena (reset on reload)
    arena *gameplay;      // Sub-arena (PERSISTS across reloads)
    arena *editor_arena;  // Editor-specific allocations
    
    // Gameplay state - this survives reload!
    Scene scene;
    Camera camera;
} memory;
```

## Hot Reload Triggers

### Windows Events
- `Global\ReloadEvent` - Trigger engine.dll reload
- `Global\ReloadEditorEvent` - Trigger editor.dll reload
- `Global\ReloadCoreEvent` - Trigger core.dll reload (full restart)

### Usage from Code

```c
// To trigger engine hot-reload:
HANDLE hEvent = OpenEvent(EVENT_MODIFY_STATE, FALSE, "Global\\ReloadEvent");
SetEvent(hEvent);
```

## Best Practices

### 1. State Management
- **Gameplay state**: Store in `gameplay` arena (survives reload)
- **Temporary data**: Use main `arena` (reset on reload)

```c
// Good: gameplay state survives reload
if (!scene.entities) {
    scene.entity_capacity = 64;
    scene.entities = arena_alloc(g->gameplay, 
        scene.entity_capacity * sizeof(entity), 16, "entities");
}

// Bad: temporary data reset each frame
float temp_buffer[256]; // Use main arena instead
```

### 2. Idempotent Initialization
Ensure your `init` functions can be called multiple times:

```c
void init_engine(memory *g) {
    // Check if already initialized
    if (scene.entities) return;
    
    // Initialize...
}
```

### 3. Thread Safety
Hot reload runs on a separate thread - ensure thread-safe operations:

```c
// Use critical sections for shared data
EnterCriticalSection(&g->data_lock);
// Access shared data
LeaveCriticalSection(&g->data_lock);
```

## Debugging Hot Reload

### Common Issues

1. **DLL locked by process**
   - Solution: Use copy-to-`_copy` variant before loading

2. **Memory corruption after reload**
   - Solution: Ensure all pointers in gameplay arena are valid

3. **Stale state persists**
   - Solution: Add explicit reset logic in `destroy_engine()`

### Enable Hot Reloading
```c
// In core.c, ensure events are created:
HANDLE hEvent = CreateEvent(NULL, TRUE, FALSE, "Global\\ReloadEvent");
```

## Performance Considerations

- **Reload time**: Should be < 100ms for typical changes
- **Memory impact**: Gameplay arena grows monotonically - periodic restarts may be needed
- **GPU resources**: Re-acquire GPU device after reload if needed

## Future Improvements

- [ ] Incremental rebuild system (only recompile changed files)
- [ ] Hot-reload with state migration
- [ ] GUI hot-reload trigger button
- [ ] Automatic reload on file save
