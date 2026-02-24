# TCC Hot-Reload Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Use TCC for near-instant engine DLL hot-reload during development, keeping MSVC for full builds.

**Architecture:** Engine sources converted from C++ to C. On file change, core.cpp calls tcc.exe directly to recompile engine.dll in milliseconds. build.exe (MSVC) still works for optimized/release builds. Shared headers get extern "C" guards so both compilers can use them.

**Tech Stack:** TCC 0.9.28rc (x86_64 Windows), MSVC cl.exe, C99

---

### Task 1: Copy tcc.exe to repo root

**Files:**
- Copy: `tcc.exe` from `C:\Users\andres\Development\tinycc\win32\tcc.exe` to repo root
- Modify: `.gitignore` — do NOT ignore tcc.exe (it should be tracked)

**Step 1: Copy the binary**

```bash
cp C:/Users/andres/Development/tinycc/win32/tcc.exe ./tcc.exe
```

**Step 2: Verify it works**

```bash
./tcc.exe -v
```
Expected: `tcc version 0.9.28rc ... (x86_64 Windows)`

**Step 3: Commit**

```bash
git add tcc.exe
git commit -m "add tcc.exe for fast engine hot-reload"
```

---

### Task 2: Make shared headers C-compatible

The engine shares headers with C++ modules (core, externals). These headers need to compile as both C and C++.

**Files:**
- Modify: `src/export.h`
- Modify: `src/game.h`
- Modify: `src/scene.h`

**Step 1: Fix export.h — make EXPORT work in both C and C++**

Current `src/export.h` line 2:
```c
#define EXPORT extern "C" __declspec(dllexport)
```

Change to:
```c
#ifdef __cplusplus
#define EXPORT extern "C" __declspec(dllexport)
#else
#define EXPORT __declspec(dllexport)
#endif
```

This way C files get plain `__declspec(dllexport)` and C++ files get `extern "C"` linkage.

**Step 2: Fix game.h — remove C++ default member initializers**

C does not support default member initializers (`int frame = -1;`). Remove them from all structs. The fields that had defaults need explicit initialization where structs are created (handled in Task 4 when converting scene.cpp).

In `src/game.h`:

Change `struct keyframe` (line 39-41):
```c
struct keyframe {
    int frame;
};
```
(Remove `= -1`. Scene.cpp already initializes keyframes explicitly, and uninitialized keyframes will be zero — but scene.cpp needs to set `-1` where needed. Actually, looking at scene.cpp, keyframes are initialized as `{}` meaning 0, or `{2},{3}` meaning specific frames. The default `-1` was for "no keyframe". We need to initialize to `-1` explicitly.)

Change `struct animator` (line 51-56):
```c
struct animator {
     animation_clip animation;
     float timer;
     int   frame_index;
     int playing;
};
```
(Remove `= 0.0f`, `= 0`, `= true`. Change `bool` to `int`. These are zero-initialized by designated initializers in scene.cpp already.)

Change `struct camera` (line 104-107):
```c
struct camera {
    vec2 position;
    float zoom;
};
```
(Remove `= {0,160}` and `= 1`. These need explicit initialization where the camera is created — that happens in externals.cpp.)

Change `bool` usage: Add at the top of game.h, after the existing includes:
```c
#ifndef __cplusplus
#include <stdbool.h>
#endif
```

**Step 3: Fix scene.h — typedef Scene for C**

In `src/scene.h`, change line 12-17:
```c
typedef struct Scene {
    int entity_count;
    entity entities[8];
} Scene;
```
This makes `Scene` work as a type name in C without the `struct` keyword.

**Step 4: Verify MSVC still compiles everything**

```bash
MSYS_NO_PATHCONV=1 ./build.exe clean && MSYS_NO_PATHCONV=1 ./build.exe
```
Expected: All targets build successfully. (This verifies the header changes don't break C++ code.)

**Step 5: Commit**

```bash
git add src/export.h src/game.h src/scene.h
git commit -m "make shared headers C-compatible"
```

---

### Task 3: Move ImGui calls from engine to externals

Engine.cpp currently calls ImGui C++ API directly (lines 77-81). These must move to externals.dll since externals stays C++.

**Files:**
- Modify: `src/externals/externals.h` — add wrapper declarations
- Modify: `src/externals/externals.cpp` — add wrapper implementations
- Modify: `src/engine/engine.cpp` — replace ImGui calls with wrapper calls

**Step 1: Add wrapper declarations to externals.h**

In `src/externals/externals.h`, before the `#endif`, add:

```c
DECLARE_FUNC_VOID_pGAME(externals_imgui_setup)
```

**Step 2: Add wrapper implementation to externals.cpp**

In `src/externals/externals.cpp`, add a new exported function:

```cpp
EXPORT void externals_imgui_setup(game *g) {
    if (!g || !g->ctx) return;
    ImGui::SetCurrentContext(g->ctx);
    ImGui::SetAllocatorFunctions(g->alloc_func, g->free_func, g->user_data);
    ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_FirstUseEver);
}
```

(This requires externals.cpp to include imgui.h — check if it already does.)

**Step 3: Replace ImGui calls in engine.cpp**

Replace lines 77-81 in `src/engine/engine.cpp`:
```c
    ImGui::SetCurrentContext(g->ctx);
    ImGui::SetAllocatorFunctions(g->alloc_func, g->free_func, g->user_data);

    ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_FirstUseEver);
```

With:
```c
    externals_imgui_setup(g);
```

Also remove these includes from engine.cpp (no longer needed):
```c
#define IMGUI_USER_CONFIG "myimguiconfig.h"
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
```

And add the externals header if not already included:
```c
#include <externals.h>
```
(Already included on line 2 — good.)

**Step 4: Verify MSVC builds**

```bash
MSYS_NO_PATHCONV=1 ./build.exe clean && MSYS_NO_PATHCONV=1 ./build.exe
```
Expected: All targets build successfully.

**Step 5: Commit**

```bash
git add src/externals/externals.h src/externals/externals.cpp src/engine/engine.cpp
git commit -m "move imgui calls from engine to externals"
```

---

### Task 4: Convert engine sources from C++ to C

**Files:**
- Rename: `src/engine/engine.cpp` → `src/engine/engine.c`
- Rename: `src/engine/renderer.cpp` → `src/engine/renderer.c`
- Rename: `src/engine/physics.cpp` → `src/engine/physics.c`
- Rename: `src/engine/scene.cpp` → `src/engine/scene.c`
- Rename: `src/engine/debug_render.cpp` → `src/engine/debug_render.c`
- Modify: `src/engine/physics.h`
- Modify: `src/engine/physics.c` (after rename)
- Modify: `src/engine/renderer.h`
- Modify: `src/engine/renderer.c` (after rename)
- Modify: `src/engine/scene.c` (after rename)
- Modify: `src/engine/engine.c` (after rename)

**Step 1: Rename all engine .cpp → .c**

```bash
cd src/engine
git mv engine.cpp engine.c
git mv renderer.cpp renderer.c
git mv physics.cpp physics.c
git mv scene.cpp scene.c
git mv debug_render.cpp debug_render.c
```

**Step 2: Fix physics — refs to ptrs**

In `src/engine/physics.h`, change line 7:
```c
bool bbox_collide(const rect* a, const rect* b);
```

In `src/engine/physics.c`:

Change line 4:
```c
#include <assert.h>
```
(was `<cassert>`)

Change line 7, function signature:
```c
bool bbox_collide(const rect* a, const rect* b) {
```

Update the body to use `->` instead of `.`:
```c
    float a_left = a->x - a->w * 0.5f;
    float a_right = a->x + a->w * 0.5f;
    float a_top = a->y + a->h * 0.5f;
    float a_bottom = a->y - a->h * 0.5f;

    float b_left = b->x - b->w * 0.5f;
    float b_right = b->x + b->w * 0.5f;
    float b_top = b->y + b->h * 0.5f;
    float b_bottom = b->y - b->h * 0.5f;
```

Change line 49 (local variable):
```c
        const animator* pa = &player->current_animation;
```
(was `const animator& pa = player->current_animation;`)

Update all `pa.` to `pa->` in the following lines (50-63).

Update call sites — line 64:
```c
            if(bbox_collide(&hit_box, &e->collider.rect)){
```
(was `bbox_collide(hit_box, e->collider.rect)`)

Line 69:
```c
        if (bbox_collide(&player->collider.rect, &e->collider.rect)) {
```

Line 124:
```c
        if (bbox_collide(&player->collider.rect, &other->collider.rect)) {
```

**Step 3: Fix renderer — ref to ptr**

In `src/engine/renderer.h`, change line 4:
```c
rect pixel_to_uv(pixel_rect p, sprite_sheet* s);
```

In `src/engine/renderer.c`, change line 5:
```c
rect pixel_to_uv(pixel_rect p, sprite_sheet* s) {
```

Update body to use `->`:
```c
    uv.x = (float)p.x / (float)s->width;
    uv.y = (float)p.y / (float)s->height;
    uv.w = (float)p.w / (float)s->width;
    uv.h = (float)p.h / (float)s->height;
```

**Step 4: Fix scene.c — explicit initialization for removed defaults**

In `src/engine/scene.c`, the keyframe initializations `{}` now need explicit `-1` since the default was removed:

Change `player_walk_down` (line 49):
```c
    .keyframes = {{-1},{-1}},
```

Change `slime_idle` (line 56, currently has no keyframes field — add it):
```c
    .keyframes = {{-1},{-1}},
```

Change animator fields in scene entities — add explicit zeros:
```c
            .current_animation = {
                .animation = player_attack_anim,
                .timer = 0.0f,
                .frame_index = 0,
                .playing = 1,
            },
```
(Repeat for slime entity.)

**Step 5: Fix engine.c — remove C++ includes, add stdbool**

The ImGui includes were already removed in Task 3. Just verify engine.c compiles cleanly as C. The `bool` type in engine.cpp line 23 (`bool attack_pressed`) needs `<stdbool.h>`. Add at top if not already provided through game.h:

Check: game.h already includes `<stdbool.h>` (added in Task 2). Engine includes game.h via externals.h → export.h path, or via scene.h. Verify this works.

**Step 6: Verify MSVC builds**

```bash
MSYS_NO_PATHCONV=1 ./build.exe clean && MSYS_NO_PATHCONV=1 ./build.exe
```
Expected: All targets build. Note: build.c still references `.cpp` — this will fail. That's expected — we fix it in Task 5.

Actually, update build.c first (Task 5) before testing.

**Step 7: Commit (after Task 5 is done)**

```bash
git add src/engine/
git commit -m "convert engine sources from C++ to C"
```

---

### Task 5: Update build.c for C engine sources

**Files:**
- Modify: `build.c` — engine source list and compile flags

**Step 1: Update source file paths**

In `build.c`, the `build_engine()` function's sources array (around line 448):
```c
    static const char *sources[] = {
        "src\\engine\\engine.c",
        "src\\engine\\renderer.c",
        "src\\engine\\physics.c",
        "src\\engine\\scene.c",
        "src\\engine\\debug_render.c"
    };
```

**Step 2: Update compile flags — use C instead of C++**

The engine compile command (around line 474) currently uses `/std:c++20 /EHsc`. Change to C compilation:
```c
            snprintf(cmd, sizeof(cmd),
                "\"%s\" /TC /MD /Zi /Od /nologo /c "
                "/DGLFW_STATIC /DGLAD_GLAPI_EXPORT "
                COMMON_INCLUDES " "
                "/Fo%s "
                "/Fd" OBJ_ENGINE_DIR "\\engine.pdb "
                "%s",
                msvc_cl, objs[i], sources[i]);
```

Key changes:
- Remove `/std:c++20 /EHsc` (C++ flags)
- Add `/TC` (force C compilation)
- Remove `/DTRACY_ENABLE` (engine doesn't use Tracy)

**Step 3: Test full MSVC build**

```bash
MSYS_NO_PATHCONV=1 ./build.exe clean && MSYS_NO_PATHCONV=1 ./build.exe
```
Expected: All 6 targets build successfully.

**Step 4: Test incremental build**

```bash
MSYS_NO_PATHCONV=1 ./build.exe engine
```
Expected: "engine is up to date."

**Step 5: Commit both Task 4 and Task 5 together**

```bash
git add build.c src/engine/
git commit -m "convert engine to C, update build.c"
```

---

### Task 6: Update core.cpp to use TCC for hot-reload

**Files:**
- Modify: `src/core/core.cpp` — compile_dll() calls tcc.exe

**Step 1: Replace compile_dll() body**

In `src/core/core.cpp`, replace the `compile_dll()` function (lines 77-84):

```cpp
void compile_dll() {
  std::string cwd = getCurrentWorkingDirectory();
  std::string command =
      cwd + "\\tcc.exe"
      " -shared"
      " -o " + cwd + "\\build\\Debug\\engine.dll"
      " -DGLAD_GLAPI_EXPORT"
      " -Isrc -Isrc/engine -Isrc/externals -Iinclude"
      " -Ilib/glad -Ilib/glfw/include -Ilib/tracy/public"
      " src/engine/engine.c"
      " src/engine/renderer.c"
      " src/engine/physics.c"
      " src/engine/scene.c"
      " src/engine/debug_render.c"
      " build/Debug/externals.lib"
      " build/Debug/glad_loader.lib"
      " opengl32.lib";
  std::cout << "TCC compiling engine: " << command << std::endl;
  system(command.c_str());
}
```

Note: TCC doesn't need TracyClient.lib since engine doesn't use Tracy. We pass the .lib files directly rather than `-L`/`-l` for clarity.

**Step 2: Verify the hot-reload path works**

1. Run `build.exe` to do initial full build
2. Run TCC compile manually:
```bash
./tcc.exe -shared -o build/Debug/engine.dll -DGLAD_GLAPI_EXPORT -Isrc -Isrc/engine -Isrc/externals -Iinclude -Ilib/glad -Ilib/glfw/include -Ilib/tracy/public src/engine/engine.c src/engine/renderer.c src/engine/physics.c src/engine/scene.c src/engine/debug_render.c build/Debug/externals.lib build/Debug/glad_loader.lib opengl32.lib
```
Expected: `build/Debug/engine.dll` is produced with no errors.

**Step 3: Commit**

```bash
git add src/core/core.cpp
git commit -m "use tcc for engine hot-reload"
```

---

### Task 7: End-to-end verification

**Step 1: Clean build with MSVC**

```bash
MSYS_NO_PATHCONV=1 ./build.exe clean && MSYS_NO_PATHCONV=1 ./build.exe
```
Expected: All 6 targets built successfully.

**Step 2: TCC engine build**

```bash
MSYS_NO_PATHCONV=1 ./tcc.exe -shared -o build/Debug/engine.dll -DGLAD_GLAPI_EXPORT -Isrc -Isrc/engine -Isrc/externals -Iinclude -Ilib/glad -Ilib/glfw/include -Ilib/tracy/public src/engine/engine.c src/engine/renderer.c src/engine/physics.c src/engine/scene.c src/engine/debug_render.c build/Debug/externals.lib build/Debug/glad_loader.lib opengl32.lib
```
Expected: engine.dll produced successfully.

**Step 3: Incremental MSVC build after TCC**

```bash
MSYS_NO_PATHCONV=1 ./build.exe engine
```
Expected: Rebuilds engine (since TCC overwrote the DLL and timestamps differ).

**Step 4: Final commit if any fixes were needed**

```bash
git add -A && git commit -m "tcc hot-reload working"
```
