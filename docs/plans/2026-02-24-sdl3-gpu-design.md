# SDL3 GPU Migration Design

## Goal

Replace GLFW + GLAD/OpenGL with SDL3 + SDL3 GPU + SDL_shadercross. Introduce a draw list architecture so the engine DLL produces rendering data (pure C structs) and the host renders it via SDL3 GPU. This cleanly decouples the engine from any GPU library, making TCC hot-reload work without linking against graphics libraries.

## Architecture

Three layers, clean separation:

1. **Host (core.cpp)** — Window lifecycle, hot-reload, event loop. Uses SDL3 for window + events.
2. **Externals (externals.cpp)** — SDL3 GPU device, pipeline, shader compilation (via SDL_shadercross), texture loading, rendering from draw list. Stays as C++, built with MSVC.
3. **Engine DLL (engine.c)** — Game logic, physics, animation. Produces a draw list (plain C structs). No SDL3 dependency. Hot-reloaded via TCC.

```
Engine (TCC-compiled) → fills draw_list → Externals (MSVC) → SDL3 GPU renders
```

## Draw List

The engine fills a simple struct that the host consumes each frame. No SDL3 types, pure C.

```c
typedef struct draw_command {
    int texture_id;
    float x, y;
    float width, height;
    float uv_x, uv_y;
    float uv_w, uv_h;
    float tint_r, tint_g, tint_b, tint_a;
} draw_command;

typedef struct debug_line_command {
    float x1, y1, x2, y2;
    float r, g, b;
} debug_line_command;

typedef struct draw_list {
    draw_command sprites[256];
    int sprite_count;
    debug_line_command lines[128];
    int line_count;
    float view_matrix[16];
    float ortho_projection[16];
} draw_list;
```

The engine's `update_engine()` receives a `draw_list*` plus the game state, fills it with what to draw. The host reads it and issues SDL3 GPU commands.

## SDL3 Integration

### Dependencies replaced

- GLFW → SDL3 (window, events, gamepad input)
- GLAD + OpenGL → SDL3 GPU API
- GLSL shaders → HLSL source, compiled at runtime via SDL_shadercross

### Vendored source

- SDL3 source: `lib/SDL3/`
- SDL_shadercross source: `lib/SDL_shadercross/`
- Both built by `build.c` as DLLs

### Shaders

Two HLSL files in `assets/shaders/`:
- `sprite.hlsl` — vertex + fragment for textured quads
- `debug_lines.hlsl` — vertex + fragment for colored lines

Compiled at startup by externals via `SDL_ShaderCross_CompileGraphicsShaderFromHLSL()`.

### Rendering flow (host side, each frame)

1. `SDL_AcquireGPUCommandBuffer()`
2. `SDL_WaitAndAcquireGPUSwapchainTexture()`
3. Upload draw list sprite data to GPU vertex buffer via transfer buffer
4. `SDL_BeginGPURenderPass()` with clear color
5. Bind sprite pipeline, bind texture, `SDL_DrawGPUPrimitives()` for each texture batch
6. Bind debug line pipeline, `SDL_DrawGPUPrimitives()` for lines
7. `SDL_EndGPURenderPass()`
8. `SDL_SubmitGPUCommandBuffer()`

### game.h changes

All `GLuint` fields removed. The `game` struct becomes pure game state (input, entities, camera, dt). GPU handles live in externals only.

## Build System

### build.c targets (MSVC)

1. `SDL3.dll` — from vendored `lib/SDL3/`
2. `SDL_shadercross.dll` — from vendored `lib/SDL_shadercross/`
3. `externals.dll` — links SDL3, SDL_shadercross, stb_image. Owns GPU device, window, rendering.
4. `engine.dll` — pure C game logic. No GPU deps.
5. `core.exe` — links SDL3, externals. Window loop, hot-reload.

### TCC hot-reload

```
tcc.exe -Blib/tcc -shared -o build/Debug/engine.dll
    -Isrc -Isrc/engine
    src/engine/engine.c src/engine/renderer.c
    src/engine/physics.c src/engine/scene.c src/engine/debug_render.c
```

No library flags for SDL3 or GLAD. Pure C, zero external dependencies.

### Removed entirely

- `lib/glad/` (glad.c, glad.h, khrplatform.h)
- `lib/glfw/` (entire GLFW library)
- `opengl32.lib` from all link commands
- All `GLAD_GLAPI_EXPORT` defines
- `glad_loader.dll` build target

## What stays the same

- Engine game logic (physics, animation, scene, entity system)
- Hot-reload architecture (core.cpp watches files, reloads engine.dll)
- TCC for fast recompilation
- stb_image for texture loading
- `build.c` nobuild pattern
