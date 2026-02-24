# 2-Layer Hot Reload with TCC

## Goal

Use TCC (Tiny C Compiler) for near-instant engine DLL recompilation during development, while keeping MSVC via `build.exe` for full/optimized builds.

## Architecture

**Layer 1 — MSVC (build.exe):** Full build of all targets. Optimized. Used for initial build and release.

**Layer 2 — TCC (tcc.exe):** Compiles only engine sources on file change. Unoptimized but near-instant (~milliseconds). Used during gameplay hot-reload.

## Changes Required

### 1. Copy tcc.exe to repo root
- Source: `tinycc/win32/tcc.exe`
- Destination: repo root, committed to git
- TCC needs its runtime headers/libs to compile; we'll point it at the Win SDK includes and our own lib/ headers via `-I` flags

### 2. Convert engine sources from C++ to C
The engine is already almost pure C. Minimal changes:
- Rename `.cpp` → `.c` for all engine sources
- Convert ~4 reference parameters to pointers
- Replace `<cassert>` with `<assert.h>`
- Move ImGui C++ calls out of engine into externals

### 3. Wrap ImGui calls in externals.dll
Engine currently calls `ImGui::SetCurrentContext()`, `ImGui::SetAllocatorFunctions()`, `ImGui::SetNextWindowPos()` directly. Move these into externals.dll as C-callable functions:
```c
// In externals.h (C-compatible)
void externals_imgui_set_context(void *ctx);
void externals_imgui_set_allocator(void *alloc, void *free_fn, void *user_data);
```
Engine calls these instead of ImGui:: namespace functions.

### 4. core.cpp calls tcc.exe for hot-reload
`compile_dll()` changes from:
```c
system("build.exe engine");
```
to:
```c
system("tcc.exe -shared -o build\\Debug\\engine.dll src\\engine\\engine.c src\\engine\\renderer.c ... -Isrc -Ilib\\glad ...");
```

### 5. build.exe engine still works
`build.c`'s engine target compiles `.c` files with MSVC (`cl.exe`). Same sources, different compiler. Used for optimized builds.

### 6. Update build.c
- Engine source list changes from `.cpp` to `.c`
- Remove C++-specific flags for engine target (no `/EHsc`, no `/std:c++20` — use C compilation)

## Dev Workflow

1. `build.exe` — full MSVC build, all targets
2. Run `AnitraEngine.exe`
3. Edit engine source → file watcher detects change
4. TCC recompiles engine.dll (~ms)
5. core.dll copies engine.dll → engine_copy.dll, reloads

## Files Changed
- `tcc.exe` — new (copied from tinycc build)
- `src/engine/*.cpp` → `src/engine/*.c` (renamed + minor edits)
- `src/engine/*.h` — add `extern "C"` guards, ptr params
- `src/externals/externals.cpp` — add ImGui C wrapper functions
- `src/externals/externals.h` — declare C wrapper functions
- `src/core/core.cpp` — compile_dll() uses tcc.exe
- `build.c` — engine target uses .c sources, C compilation flags
