# SDL3 GPU Migration Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace GLFW + GLAD/OpenGL with SDL3 + SDL3 GPU + SDL_shadercross, using a draw list architecture where the engine produces rendering data and the host renders it.

**Architecture:** Three-layer separation — engine DLL (pure C, TCC hot-reloaded) fills a `draw_list` struct, externals DLL (MSVC) consumes the draw list and renders via SDL3 GPU, core (MSVC) manages the SDL3 window and event loop. The engine has zero GPU dependencies.

**Tech Stack:** SDL3 (window/input/GPU), SDL_shadercross (runtime HLSL→SPIRV), SPIRV-Cross + DXC (prebuilt DLLs), stb_image (texture loading), TCC (hot-reload compiler)

---

## Phase 1: Get SDL3 and SDL_shadercross into the repo

### Task 1: Clone SDL3 source, extract Windows file list, add to lib/

**Context:** SDL3 has ~200 source files for a Windows build. We need to figure out exactly which files to compile. The approach: clone SDL3, run cmake to generate the build, extract the file list from the generated Ninja/MSVC project, then vendor those files.

**Files:**
- Create: `lib/SDL3/` (vendored SDL3 source)
- Create: `lib/SDL3/WINDOWS_SOURCES.txt` (list of .c files needed)

**Step 1: Clone SDL3**

```bash
cd C:\Users\andres\Development
git clone --depth 1 https://github.com/libsdl-org/SDL.git SDL3-temp
```

**Step 2: Run cmake to identify Windows source files**

```bash
cd SDL3-temp
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=ON -DSDL_STATIC=OFF
```

**Step 3: Extract the .c file list from the generated build**

Look at `build/build.ninja` or `build/CMakeFiles/SDL3-shared.dir/objects.a` to find every `.c` and `.cpp` file that gets compiled. Save this list.

**Step 4: Copy SDL3 source into lib/SDL3/**

Copy only the needed directories:
- `include/SDL3/` (all public headers)
- `src/` (source files identified in step 3)
- `LICENSE.txt`

Do NOT copy: `test/`, `docs/`, `cmake/`, `android-project/`, `Xcode/`, `VisualC*/`, `.github/`

**Step 5: Save the file list**

Create `lib/SDL3/WINDOWS_SOURCES.txt` with one .c file path per line (relative to `lib/SDL3/`). This will be the reference for build.c.

**Step 6: Clean up**

```bash
rm -rf C:\Users\andres\Development\SDL3-temp
```

**Verify:** The `lib/SDL3/include/SDL3/SDL.h` header exists and all source files listed in WINDOWS_SOURCES.txt exist.

**Commit:**
```bash
git add lib/SDL3/
git commit -m "vendor SDL3 source for Windows build"
```

---

### Task 2: Add SDL3 build target to build.c

**Context:** build.c currently has targets for tracy, glad, externals, core, engine, exe. Add a new `build_sdl3()` target that compiles all the Windows source files from Task 1 into `SDL3.dll`.

**Files:**
- Modify: `build.c`

**Step 1: Add OBJ_SDL3_DIR constant and ensure_dir**

```c
#define OBJ_SDL3_DIR "build\\obj\\sdl3"
```

Add `ensure_dir(OBJ_SDL3_DIR)` to `ensure_dirs()`.

**Step 2: Add build_sdl3() function**

Read the source file list from `lib/SDL3/WINDOWS_SOURCES.txt` that was created in Task 1. The function should:
- Compile each .c file with: `/MD /O2 /nologo /c /DSDL_BUILDING_SDL3 /Ilib\SDL3\include /Ilib\SDL3\src`
- Add Windows-specific defines: `/D_WINDOWS /DWIN32 /D_WIN32`
- Link into SDL3.dll with: `user32.lib gdi32.lib winmm.lib imm32.lib ole32.lib oleaut32.lib version.lib advapi32.lib setupapi.lib shell32.lib cfgmgr32.lib`

Since there are ~200 files, use a loop pattern similar to build_externals() but reading from the source list. Alternatively, hardcode the array (most reliable).

**Step 3: Wire into build_all()**

Add `build_sdl3()` call before `build_externals()` in `build_all()`.

**Step 4: Build and verify**

```bash
MSYS_NO_PATHCONV=1 ./build.exe sdl3
```

Expected: `build/Debug/SDL3.dll` and `build/Debug/SDL3.lib` are produced.

**Commit:**
```bash
git add build.c
git commit -m "add SDL3 build target to build.c"
```

---

### Task 3: Get SDL_shadercross source and prebuilt DLL dependencies

**Context:** SDL_shadercross is a single .c file (`SDL_shadercross.c`). It depends on:
- SPIRV-Cross (C++ library, prebuilt as `spirv-cross-c-shared.dll` — available in Vulkan SDK)
- DirectXShaderCompiler (prebuilt as `dxcompiler.dll` + `dxil.dll` — available from Microsoft releases)

We vendor the single source file and the prebuilt DLLs.

**Files:**
- Create: `lib/SDL_shadercross/` (source)
- Create: `lib/SDL_shadercross/external/` (prebuilt DLLs)

**Step 1: Clone SDL_shadercross**

```bash
cd C:\Users\andres\Development
git clone --depth 1 https://github.com/libsdl-org/SDL_shadercross.git SDL_shadercross-temp
```

**Step 2: Copy needed files**

```
lib/SDL_shadercross/
  src/SDL_shadercross.c
  include/SDL3_shadercross/SDL_shadercross.h
  LICENSE.txt
```

**Step 3: Get SPIRV-Cross prebuilt DLL**

If Vulkan SDK is installed, copy `spirv-cross-c-shared.dll` from the SDK bin directory. Otherwise, download from the Vulkan SDK releases page. Place in `lib/SDL_shadercross/external/`.

**Step 4: Get DXC prebuilt DLLs**

Download `dxcompiler.dll` and `dxil.dll` from https://github.com/microsoft/DirectXShaderCompiler/releases. Place in `lib/SDL_shadercross/external/`.

**Step 5: Verify files exist**

```
lib/SDL_shadercross/src/SDL_shadercross.c
lib/SDL_shadercross/include/SDL3_shadercross/SDL_shadercross.h
lib/SDL_shadercross/external/spirv-cross-c-shared.dll
lib/SDL_shadercross/external/dxcompiler.dll
lib/SDL_shadercross/external/dxil.dll
```

**Commit:**
```bash
git add lib/SDL_shadercross/
git commit -m "vendor SDL_shadercross source and prebuilt DLL deps"
```

---

### Task 4: Add SDL_shadercross build target to build.c

**Files:**
- Modify: `build.c`

**Step 1: Add build_shadercross() function**

Compile `SDL_shadercross.c` with:
```
/MD /O2 /nologo /c
/DSDL_SHADERCROSS_BUILDING
/Ilib\SDL_shadercross\include
/Ilib\SDL3\include
```

Link into `SDL_shadercross.dll` with:
```
SDL3.lib spirv-cross-c-shared.lib
```

Note: We need import libs (.lib) for the prebuilt DLLs. Generate them from the DLLs using `lib.exe /def:` or `dumpbin /exports` + `.def` file.

**Step 2: Copy runtime DLLs to build/Debug/**

The build function should copy `spirv-cross-c-shared.dll`, `dxcompiler.dll`, `dxil.dll` from `lib/SDL_shadercross/external/` to `build/Debug/` so they're found at runtime.

**Step 3: Wire into build_all()**

Add `build_shadercross()` after `build_sdl3()`.

**Step 4: Build and verify**

```bash
MSYS_NO_PATHCONV=1 ./build.exe shadercross
```

Expected: `build/Debug/SDL_shadercross.dll` produced.

**Commit:**
```bash
git add build.c
git commit -m "add SDL_shadercross build target to build.c"
```

---

## Phase 2: Draw list abstraction and engine refactor

### Task 5: Create draw_list.h shared header

**Context:** This is the interface between engine (producer) and host (consumer). Pure C, no GPU types.

**Files:**
- Create: `src/draw_list.h`

**Step 1: Write draw_list.h**

```c
#ifndef DRAW_LIST_H
#define DRAW_LIST_H

#define MAX_DRAW_COMMANDS 256
#define MAX_DEBUG_LINES 128

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
    draw_command sprites[MAX_DRAW_COMMANDS];
    int sprite_count;
    debug_line_command lines[MAX_DEBUG_LINES];
    int line_count;
    float view_matrix[16];
    float ortho_projection[16];
} draw_list;

#endif /* DRAW_LIST_H */
```

**Verify:** File compiles when included from a .c file (no syntax errors).

**Commit:**
```bash
git add src/draw_list.h
git commit -m "add draw_list.h shared header"
```

---

### Task 6: Strip GL types from game.h and debug_render.h

**Context:** The `game` struct currently has `GLuint` fields for shaders, textures, VAOs, and uniform locations. The `debug_renderer` has GL object IDs. All of these move to the host side (externals). The engine only needs: entities, input, camera, timing, and a `draw_list*`.

**Files:**
- Modify: `src/game.h`
- Modify: `src/engine/debug_render.h`

**Step 1: Update game.h**

Remove these fields from `game` struct:
- `struct GLFWwindow *window` — moves to externals (SDL_Window*)
- `GLuint textures[TEXTURE_COUNT]` — moves to externals
- `GLuint quad_VAO` — removed entirely (SDL3 uses different model)
- `GLuint sprite_shader` — moves to externals
- `GLuint translation_loc` through `GLuint sprite_size_loc` — removed (no uniforms in draw list model)
- `debug_renderer debug_renderer` — simplified (no GL objects)
- `render_entities_func`, `render_sprite_func`, `load_texture_func` — removed (engine pushes to draw_list)

Add:
- `#include "draw_list.h"`
- `draw_list draw_list;` field to game struct

Remove `#include <glad.h>` from game.h.

The `sprite` struct currently has `GLuint texture` — change to `int texture_id` (TextureID enum).

Remove `render_sprite_func` and `load_texture_func` typedefs (they use GLuint).

**Step 2: Update debug_render.h**

Remove all `GLuint` fields from `debug_renderer`. Remove `#include <glad.h>`.

The debug_renderer becomes a simple line buffer (already is, minus the GL objects):
```c
typedef struct {
    float* vertex_buffer;
    int max_lines;
    int current_line_count;
} debug_renderer;
```

**Step 3: Verify compilation**

The engine .c files will have compile errors (they reference removed fields). That's expected — Task 7 fixes them.

**Commit:**
```bash
git add src/game.h src/engine/debug_render.h
git commit -m "strip GL types from game.h and debug_render.h"
```

---

### Task 7: Refactor engine to use draw_list instead of GL calls

**Context:** Currently renderer.c calls `glUseProgram`, `glDrawElements`, etc. directly. Change all rendering functions to push `draw_command` entries to `g->draw_list` instead.

**Files:**
- Modify: `src/engine/renderer.c`
- Modify: `src/engine/renderer.h`
- Modify: `src/engine/engine.c`
- Modify: `src/engine/debug_render.c`

**Step 1: Rewrite renderer.h**

Remove all `GLuint` parameters. Functions now take `game*` and push to draw_list:
```c
#ifndef RENDERER_H
#define RENDERER_H
#include <game.h>

void update_camera_matrix(camera* cam, float* matrix);
void update_animation(game* g);
void render_tile(game* g, int tile, float x, float y);
void render_tiles(game* g);
void render_entities(game* g);
void render_health_bar(game* g, float x, float y, float health, float max_health);
```

**Step 2: Rewrite renderer.c**

Remove `#include <glad.h>`. Remove all `gl*` calls.

`render_sprite_pixel_perfect()` becomes a draw_list push:
```c
static void push_sprite(draw_list* dl, int texture_id, float x, float y,
                        float w, float h, float uv_x, float uv_y,
                        float uv_w, float uv_h,
                        float r, float g, float b, float a) {
    if (dl->sprite_count >= MAX_DRAW_COMMANDS) return;
    draw_command* cmd = &dl->sprites[dl->sprite_count++];
    cmd->texture_id = texture_id;
    cmd->x = x; cmd->y = y;
    cmd->width = w; cmd->height = h;
    cmd->uv_x = uv_x; cmd->uv_y = uv_y;
    cmd->uv_w = uv_w; cmd->uv_h = uv_h;
    cmd->tint_r = r; cmd->tint_g = g;
    cmd->tint_b = b; cmd->tint_a = a;
}
```

Convert each render function to use `push_sprite`:
- `render_sprite_pixel_perfect()` → compute UV from pixel_rect, call push_sprite
- `render_scaled_sprite()` → push_sprite with full UV (0,0,1,1)
- `render_health_bar()` → push_sprite for fill + frame
- `render_entities()` → loop entities, push_sprite for each
- `render_tile()` → push_sprite
- `render_tiles()` → loop tiles, push_sprite each

Keep `update_camera_matrix()` and `update_animation()` unchanged (pure math, no GL).

**Step 3: Update engine.c**

Remove `#include <glad.h>`. Remove the debug GL rendering code (lines 56-73 in current engine.c).

The debug line rendering now pushes to `g->draw_list.lines[]` instead of GL buffer:
```c
// In update_engine(), after calling render functions:
// Copy debug lines from debug_renderer to draw_list
for (int i = 0; i < g->debug_renderer.current_line_count; i++) {
    if (g->draw_list.line_count >= MAX_DEBUG_LINES) break;
    int idx = i * 10;
    debug_line_command* line = &g->draw_list.lines[g->draw_list.line_count++];
    line->x1 = g->debug_renderer.vertex_buffer[idx];
    line->y1 = g->debug_renderer.vertex_buffer[idx+1];
    line->r = g->debug_renderer.vertex_buffer[idx+2];
    line->g = g->debug_renderer.vertex_buffer[idx+3];
    line->b = g->debug_renderer.vertex_buffer[idx+4];
    line->x2 = g->debug_renderer.vertex_buffer[idx+5];
    line->y2 = g->debug_renderer.vertex_buffer[idx+6];
}
```

**Step 4: Update update_engine() flow**

```c
EXPORT void update_engine(game *g) {
    if (!g) return;
    g->draw_list.sprite_count = 0;
    g->draw_list.line_count = 0;
    g->debug_renderer.current_line_count = 0;

    update_input(g);
    update_animation(g);
    update_camera_matrix(&g->camera, g->draw_list.view_matrix);

    // These now push to draw_list instead of calling GL
    render_tiles(g);
    render_entities(g);

    // Copy debug lines to draw_list
    // ... (code from step 3)
}
```

**Verify:** Engine compiles with MSVC (`build.exe engine`). It won't render yet (host still uses GL), but it should compile.

**Commit:**
```bash
git add src/engine/ src/game.h
git commit -m "refactor engine to push draw_list instead of GL calls"
```

---

### Task 8: Write HLSL shaders

**Files:**
- Create: `assets/shaders/sprite.hlsl`
- Create: `assets/shaders/debug_lines.hlsl`

**Step 1: Write sprite.hlsl**

Equivalent to the current GLSL sprite shader. Takes position + UV, applies projection*view*translation, samples texture with tint.

```hlsl
// Vertex shader
cbuffer Constants : register(b0) {
    float4x4 projection;
    float4x4 view;
};

struct VSInput {
    float2 pos : POSITION;
    float2 uv : TEXCOORD0;
    float2 translation : TEXCOORD1;
    float2 sprite_offset : TEXCOORD2;
    float2 sprite_size : TEXCOORD3;
    float4 tint : COLOR0;
};

struct VSOutput {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float4 tint : COLOR0;
};

VSOutput VSMain(VSInput input) {
    VSOutput output;
    float2 worldPos = input.pos + input.translation;
    output.pos = mul(mul(projection, view), float4(worldPos, 0.0, 1.0));
    output.uv = input.sprite_offset + (input.uv * input.sprite_size);
    output.tint = input.tint;
    return output;
}

// Fragment shader
Texture2D tex : register(t0);
SamplerState samp : register(s0);

float4 PSMain(VSOutput input) : SV_Target {
    float4 texColor = tex.Sample(samp, input.uv);
    return texColor * input.tint;
}
```

Note: The exact vertex layout will depend on how we pack sprite data into vertex buffers. This may need adjustment during implementation.

**Step 2: Write debug_lines.hlsl**

```hlsl
cbuffer Constants : register(b0) {
    float4x4 projection;
    float4x4 view;
};

struct VSInput {
    float2 pos : POSITION;
    float3 color : COLOR0;
};

struct VSOutput {
    float4 pos : SV_Position;
    float3 color : COLOR0;
};

VSOutput VSMain(VSInput input) {
    VSOutput output;
    output.pos = mul(mul(projection, view), float4(input.pos, 0.0, 1.0));
    output.color = input.color;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target {
    return float4(input.color, 1.0);
}
```

**Verify:** Files exist in `assets/shaders/`. (Can't test compilation until SDL_shadercross is wired up.)

**Commit:**
```bash
git add assets/shaders/
git commit -m "add HLSL shaders for sprite and debug line rendering"
```

---

## Phase 3: Rewrite host side with SDL3

### Task 9: Rewrite externals.cpp with SDL3

**Context:** This is the biggest single task. externals.cpp currently uses GLFW for window/input and OpenGL for all rendering. Replace everything with SDL3 window, SDL3 events for input, SDL3 GPU for rendering, and SDL_shadercross for shader compilation.

**Files:**
- Modify: `src/externals/externals.cpp`
- Modify: `src/externals/externals.h`

**Step 1: Replace includes**

Remove:
```cpp
#include <glad.h>
#include <GLFW/glfw3.h>
```

Add:
```cpp
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3_shadercross/SDL_shadercross.h>
```

**Step 2: Add SDL3 GPU state struct**

```cpp
static SDL_Window* window = NULL;
static SDL_GPUDevice* gpu_device = NULL;
static SDL_GPUGraphicsPipeline* sprite_pipeline = NULL;
static SDL_GPUGraphicsPipeline* line_pipeline = NULL;
static SDL_GPUSampler* sampler = NULL;
static SDL_GPUTexture* textures[TEXTURE_COUNT] = {0};
static int texture_widths[TEXTURE_COUNT] = {0};
static int texture_heights[TEXTURE_COUNT] = {0};
```

**Step 3: Rewrite init_externals()**

Replace GLFW init + GLAD loader with:
```cpp
SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
window = SDL_CreateWindow("Anitra Engine", 800, 600, SDL_WINDOW_RESIZABLE);
gpu_device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL, true, NULL);
SDL_ClaimWindowForGPUDevice(gpu_device, window);
SDL_ShaderCross_Init();
```

Then:
- Load and compile HLSL shaders via SDL_shadercross
- Create sprite_pipeline and line_pipeline via SDL_CreateGPUGraphicsPipeline
- Create sampler via SDL_CreateGPUSampler (nearest filter for pixel art)
- Load textures via stb_image + SDL_CreateGPUTexture + transfer buffer upload

**Step 4: Rewrite update_externals()**

Replace the GLFW input polling with SDL3 event polling:
```cpp
SDL_Event event;
while (SDL_PollEvent(&event)) {
    if (event.type == SDL_EVENT_QUIT) g->play = 0;
}
// Keyboard state
const bool* keys = SDL_GetKeyboardState(NULL);
// Gamepad state
SDL_Gamepad* gamepad = SDL_GetGamepads(NULL) ? SDL_OpenGamepad(...) : NULL;
```

Replace GL rendering with SDL3 GPU command buffer:
```cpp
SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu_device);
SDL_GPUTexture* swapchain;
SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swapchain, NULL, NULL);

// Begin render pass
SDL_GPUColorTargetInfo color_target = {0};
color_target.texture = swapchain;
color_target.load_op = SDL_GPU_LOADOP_CLEAR;
color_target.clear_color = (SDL_FColor){0.45f, 0.55f, 0.60f, 1.0f};
color_target.store_op = SDL_GPU_STOREOP_STORE;

SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color_target, 1, NULL);

// Render sprites from draw_list
// Upload vertex data, bind pipeline, bind textures, draw

// Render debug lines from draw_list

SDL_EndGPURenderPass(pass);
SDL_SubmitGPUCommandBuffer(cmd);
```

**Step 5: Rewrite texture loading**

Replace GL texture creation with SDL3 GPU texture creation:
```cpp
SDL_GPUTexture* create_gpu_texture(const char* filepath, int* out_w, int* out_h) {
    int w, h, channels;
    unsigned char* data = stbi_load(filepath, &w, &h, &channels, 4); // force RGBA

    SDL_GPUTextureCreateInfo tex_info = {0};
    tex_info.type = SDL_GPU_TEXTURETYPE_2D;
    tex_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    tex_info.width = w;
    tex_info.height = h;
    tex_info.layer_count_or_depth = 1;
    tex_info.num_levels = 1;
    tex_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;

    SDL_GPUTexture* texture = SDL_CreateGPUTexture(gpu_device, &tex_info);

    // Upload via transfer buffer
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(gpu_device, ...);
    void* mapped = SDL_MapGPUTransferBuffer(gpu_device, transfer, false);
    memcpy(mapped, data, w * h * 4);
    SDL_UnmapGPUTransferBuffer(gpu_device, transfer);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu_device);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_UploadToGPUTexture(copy, ...);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);

    SDL_ReleaseGPUTransferBuffer(gpu_device, transfer);
    stbi_image_free(data);

    *out_w = w; *out_h = h;
    return texture;
}
```

**Step 6: Implement draw_list rendering**

The core rendering loop reads from `g->draw_list`:
- Sort/batch sprites by texture_id for efficiency
- For each batch: upload vertex data, bind texture, draw
- For debug lines: upload line vertex data, bind line pipeline, draw

**Verify:** Build with `build.exe externals`. Window opens, sprites render.

**Commit:**
```bash
git add src/externals/
git commit -m "rewrite externals with SDL3 GPU rendering"
```

---

### Task 10: Update core.cpp for SDL3

**Context:** core.cpp currently uses GLFW window checks (`glfwWindowShouldClose`). The window is now managed by SDL3 in externals, so core.cpp just needs minor updates.

**Files:**
- Modify: `src/core/core.cpp`

**Step 1: Remove GLFW-related code**

The window is created in externals.cpp. core.cpp shouldn't reference GLFW or SDL3 directly — it communicates through the `game` struct's `play` field.

**Step 2: Update compile_dll() TCC command**

Remove GLAD/GLFW flags:
```cpp
std::string command =
    "cd /d " + cwd +
    " && tcc.exe -Blib/tcc -shared"
    " -o build\\Debug\\engine.dll"
    " -Isrc -Isrc/engine"
    " src/engine/engine.c"
    " src/engine/renderer.c"
    " src/engine/physics.c"
    " src/engine/scene.c"
    " src/engine/debug_render.c";
```

No library links needed — engine is pure C with zero external dependencies.

**Step 3: Verify hot-reload still works**

1. Build everything with `build.exe`
2. Run `AnitraEngine.exe`
3. Edit a .c file in `src/engine/`
4. Verify TCC recompiles and engine reloads

**Commit:**
```bash
git add src/core/core.cpp
git commit -m "update core.cpp TCC command for SDL3 migration"
```

---

## Phase 4: Build system update and cleanup

### Task 11: Update build.c — replace GLAD/GLFW with SDL3

**Files:**
- Modify: `build.c`

**Step 1: Remove build_glad() function entirely**

**Step 2: Update COMMON_INCLUDES**

Remove GLAD and GLFW paths, add SDL3:
```c
#define COMMON_INCLUDES \
    "/Isrc /Isrc\\core /Isrc\\engine /Isrc\\externals /Iinclude " \
    "/Ilib\\SDL3\\include /Ilib\\SDL_shadercross\\include /Ilib\\tracy\\public"
```

**Step 3: Update build_externals()**

Link against SDL3.lib and SDL_shadercross.lib instead of glad_loader.lib, opengl32.lib, glfw3.lib:
```c
DEBUG_DIR "\\SDL3.lib "
DEBUG_DIR "\\SDL_shadercross.lib "
```

Add SDL3 include path.

**Step 4: Update build_engine()**

Remove `/DGLFW_STATIC /DGLAD_GLAPI_EXPORT` defines. Remove glad_loader.lib and glfw3.lib from link. Engine links only externals.lib (or nothing if pure draw_list).

**Step 5: Update build_core()**

Replace glad_loader.lib and glfw3.lib with SDL3.lib.

**Step 6: Update build_exe()**

Replace opengl32.lib and glfw3.lib with SDL3.lib.

**Step 7: Update build_all() order**

```c
build_sdl3()         // NEW
build_shadercross()  // NEW
build_tracy()
build_externals()
build_core()
build_engine()
build_exe()
```

**Step 8: Full clean build**

```bash
MSYS_NO_PATHCONV=1 ./build.exe clean && MSYS_NO_PATHCONV=1 ./build.exe
```

**Commit:**
```bash
git add build.c
git commit -m "update build.c: replace GLAD/GLFW with SDL3"
```

---

### Task 12: Delete old dependencies

**Files:**
- Delete: `lib/glad/` (glad.c, glad.h, khrplatform.h)
- Delete: `lib/glfw/` (entire directory)
- Delete: `lib/glew/` (if unused)
- Delete: `lib/imgui-1.90.9/` (if still present)

**Step 1: Remove old lib directories**

```bash
rm -rf lib/glad lib/glfw lib/glew lib/imgui-1.90.9
```

**Step 2: Remove any remaining GL references**

Search all source files for `glad.h`, `GLFW`, `GLuint`, `opengl32.lib` and remove any remnants.

**Step 3: Full clean build to verify nothing depends on removed files**

```bash
MSYS_NO_PATHCONV=1 ./build.exe clean && MSYS_NO_PATHCONV=1 ./build.exe
```

**Commit:**
```bash
git add -A
git commit -m "remove GLAD, GLFW, and other unused libraries"
```

---

### Task 13: End-to-end verification

**Step 1: Full MSVC build**

```bash
MSYS_NO_PATHCONV=1 ./build.exe clean && MSYS_NO_PATHCONV=1 ./build.exe
```

All targets build without errors.

**Step 2: Run the game**

```bash
./build/Debug/AnitraEngine.exe
```

- Window opens (SDL3)
- Sprites render (tiles, player, enemies)
- Input works (keyboard + gamepad)
- Health bars display

**Step 3: TCC hot-reload**

- Edit `src/engine/scene.c` (change enemy position or tile layout)
- Save
- Verify engine.dll recompiles via TCC in <100ms
- Verify game hot-reloads without restart

**Step 4: Incremental build**

- Touch one engine source file
- Run `build.exe engine`
- Verify only changed file recompiles

**Commit:**
```bash
git add -A
git commit -m "SDL3 GPU migration complete"
```
