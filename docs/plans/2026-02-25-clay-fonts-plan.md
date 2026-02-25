# Clay + GPU Vector Fonts + HarfBuzz Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add Clay UI layout, GPU vector font rendering (Bezier fragment shader), and HarfBuzz text shaping to the engine so we can build editor panels.

**Architecture:** Clay computes layout on CPU, outputs render commands. Font system extracts Bezier curves from .ttf via stb_truetype, uploads to GPU storage buffers. Fragment shader evaluates winding number per pixel with 8x8 grid acceleration. HarfBuzz shapes text (ligatures, kerning) before glyph emission. All lives in the externals DLL.

**Tech Stack:** Clay (C99 single header), stb_truetype (single header), HarfBuzz (C++ amalgamated build), HLSL shaders, SDL3 GPU API

---

### Task 1: Vendor dependencies

**Files:**
- Create: `lib/clay/clay.h`
- Create: `src/stb_truetype.h`
- Create: `lib/harfbuzz/` (clone src/ directory)
- Create: `assets/fonts/Inter-Regular.ttf`

**Step 1: Download Clay**

```bash
curl -L -o lib/clay/clay.h https://raw.githubusercontent.com/nicbarker/clay/main/clay.h
```

Verify: `head -5 lib/clay/clay.h` should show Clay header comment.

**Step 2: Download stb_truetype**

```bash
curl -L -o src/stb_truetype.h https://raw.githubusercontent.com/nothings/stb/master/stb_truetype.h
```

**Step 3: Clone HarfBuzz source**

```bash
git clone --depth=1 https://github.com/harfbuzz/harfbuzz.git lib/harfbuzz-src
```

We only need `lib/harfbuzz-src/src/` — the amalgamated file is `src/harfbuzz.cc` plus headers in `src/`.

**Step 4: Download Inter font**

```bash
mkdir -p assets/fonts
curl -L -o assets/fonts/Inter-Regular.ttf "https://github.com/rsms/inter/raw/master/docs/font-files/Inter-Regular.otf"
```

Note: Inter distributes as .otf (which stb_truetype can read — it handles both TrueType and OpenType).

**Step 5: Commit**

```bash
git add lib/clay/ src/stb_truetype.h assets/fonts/
git commit -m "vendor: add Clay, stb_truetype, Inter font"
```

Note: HarfBuzz is large — we may want to add just the src/ directory or use the amalgamated approach. Commit separately:

```bash
git add lib/harfbuzz-src/src/
git commit -m "vendor: add HarfBuzz source (amalgamated build)"
```

---

### Task 2: Build HarfBuzz into externals

**Files:**
- Modify: `build.c` (externals target)

**Context:** HarfBuzz provides an amalgamated file `harfbuzz.cc` that compiles the entire library as a single C++ compilation unit. Like Tracy, it needs C++ compilation. We compile it to a .obj and link it into the externals DLL.

**Step 1: Add HarfBuzz compilation to build.c**

In `build.c`, after the `build_externals()` function, or within it, add compilation of HarfBuzz:

```c
/* Compile HarfBuzz amalgamated (C++, like Tracy) */
if (needs_rebuild("lib\\harfbuzz-src\\src\\harfbuzz.cc",
                   OBJ_EXT_DIR "\\harfbuzz.obj")) {
    snprintf(cmd, sizeof(cmd),
        "\"%s\" /std:c++17 /EHsc /MD /Zi /Od /nologo /c "
        "/DHB_MINI "
        "/DHB_NO_FALLBACK_SHAPE "
        "/DHB_NO_MT "
        "/Ilib\\harfbuzz-src\\src "
        "/Fo" OBJ_EXT_DIR "\\harfbuzz.obj "
        "/Fd" OBJ_EXT_DIR "\\harfbuzz.pdb "
        "lib\\harfbuzz-src\\src\\harfbuzz.cc",
        msvc_cl);
    if (run_cmd(cmd) != 0) return 1;
}
```

Then add `harfbuzz.obj` to the externals link command.

**Step 2: Verify it compiles**

Run: `build.exe externals`
Expected: HarfBuzz compiles, externals.dll links successfully.

**Step 3: Commit**

```bash
git add build.c
git commit -m "build: compile HarfBuzz amalgamated into externals"
```

---

### Task 3: Font loading — extract Bezier curves from .ttf

**Files:**
- Modify: `src/externals/externals.cpp`

**Context:** This task adds the CPU-side font system: load .ttf with stb_truetype, extract glyph outlines as quadratic Bezier curves, build grid acceleration. No GPU upload yet.

**Step 1: Add stb_truetype implementation and data structures**

At the top of `externals.cpp`, after the stb_image include:

```cpp
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
```

Add font data structures:

```cpp
struct BezierCurve {
    float p0x, p0y;  // start
    float p1x, p1y;  // control
    float p2x, p2y;  // end
};

struct GpuGridCell {
    uint16_t index_offset;
    uint16_t index_count;
};

struct GlyphInfo {
    uint32_t curve_offset;
    uint32_t curve_count;
    float bbox_min_x, bbox_min_y;
    float bbox_max_x, bbox_max_y;
    float advance_width;
    float left_bearing;
    uint32_t grid_offset;  // into grid cell buffer
    uint16_t grid_cols;
    uint16_t grid_rows;
    uint16_t _pad;
};

#define MAX_CURVES      16384
#define MAX_GRID_CELLS  (256 * 64)   // 256 glyphs * 8x8 grid
#define MAX_GRID_INDICES (256 * 64 * 8) // avg 8 curves per cell
#define GRID_COLS 8
#define GRID_ROWS 8

struct FontData {
    stbtt_fontinfo stb_info;
    unsigned char *ttf_buffer;

    // CPU-side data (also uploaded to GPU)
    BezierCurve curves[MAX_CURVES];
    uint32_t curve_count;

    GlyphInfo glyphs[256];  // ASCII range

    GpuGridCell grid_cells[MAX_GRID_CELLS];
    uint32_t grid_cell_count;

    uint16_t grid_indices[MAX_GRID_INDICES];
    uint32_t grid_index_count;

    float ascent, descent, line_gap;
};

static FontData editor_font;
```

**Step 2: Write font_load() — extract curves and build grids**

```cpp
static int bezier_intersects_rect(BezierCurve *c,
    float min_x, float min_y, float max_x, float max_y)
{
    // Conservative: check if any control point is in the rect,
    // or if the curve's bounding box overlaps the cell
    float cx_min = fminf(fminf(c->p0x, c->p1x), c->p2x);
    float cx_max = fmaxf(fmaxf(c->p0x, c->p1x), c->p2x);
    float cy_min = fminf(fminf(c->p0y, c->p1y), c->p2y);
    float cy_max = fmaxf(fmaxf(c->p0y, c->p1y), c->p2y);
    return !(cx_max < min_x || cx_min > max_x ||
             cy_max < min_y || cy_min > max_y);
}

static int font_load(FontData *font, const char *path) {
    // Read file
    size_t size = 0;
    font->ttf_buffer = (unsigned char*)SDL_LoadFile(path, &size);
    if (!font->ttf_buffer) {
        fprintf(stderr, "Failed to load font: %s\n", path);
        return -1;
    }

    if (!stbtt_InitFont(&font->stb_info, font->ttf_buffer, 0)) {
        fprintf(stderr, "stbtt_InitFont failed for %s\n", path);
        return -1;
    }

    // Get font metrics
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&font->stb_info, &ascent, &descent, &line_gap);
    float scale = stbtt_ScaleForPixelHeight(&font->stb_info, 1.0f);
    font->ascent = ascent * scale;
    font->descent = descent * scale;
    font->line_gap = line_gap * scale;

    font->curve_count = 0;
    font->grid_cell_count = 0;
    font->grid_index_count = 0;

    // Extract glyphs for ASCII 32-126
    for (int codepoint = 32; codepoint < 127; codepoint++) {
        int glyph_index = stbtt_FindGlyphIndex(&font->stb_info, codepoint);
        GlyphInfo *gi = &font->glyphs[codepoint];

        // Metrics
        int advance, lsb;
        stbtt_GetGlyphHMetrics(&font->stb_info, glyph_index, &advance, &lsb);
        gi->advance_width = advance * scale;
        gi->left_bearing = lsb * scale;

        // Bounding box
        int x0, y0, x1, y1;
        stbtt_GetGlyphBox(&font->stb_info, glyph_index, &x0, &y0, &x1, &y1);
        gi->bbox_min_x = x0 * scale;
        gi->bbox_min_y = y0 * scale;
        gi->bbox_max_x = x1 * scale;
        gi->bbox_max_y = y1 * scale;

        // Extract shape
        stbtt_vertex *vertices = NULL;
        int num_verts = stbtt_GetGlyphShape(&font->stb_info, glyph_index, &vertices);

        gi->curve_offset = font->curve_count;
        gi->curve_count = 0;

        float cx = 0, cy = 0; // current point
        for (int v = 0; v < num_verts; v++) {
            stbtt_vertex *vert = &vertices[v];
            float vx = vert->x * scale;
            float vy = vert->y * scale;

            if (vert->type == STBTT_vmove) {
                cx = vx; cy = vy;
            } else if (vert->type == STBTT_vline) {
                // Line -> degenerate Bezier
                BezierCurve *c = &font->curves[font->curve_count++];
                c->p0x = cx; c->p0y = cy;
                c->p1x = (cx + vx) * 0.5f;
                c->p1y = (cy + vy) * 0.5f;
                c->p2x = vx; c->p2y = vy;
                gi->curve_count++;
                cx = vx; cy = vy;
            } else if (vert->type == STBTT_vcurve) {
                BezierCurve *c = &font->curves[font->curve_count++];
                c->p0x = cx; c->p0y = cy;
                c->p1x = vert->cx * scale;
                c->p1y = vert->cy * scale;
                c->p2x = vx; c->p2y = vy;
                gi->curve_count++;
                cx = vx; cy = vy;
            }
        }
        stbtt_FreeShape(&font->stb_info, vertices);

        // Build 8x8 grid acceleration
        gi->grid_offset = font->grid_cell_count;
        gi->grid_cols = GRID_COLS;
        gi->grid_rows = GRID_ROWS;

        float gw = gi->bbox_max_x - gi->bbox_min_x;
        float gh = gi->bbox_max_y - gi->bbox_min_y;

        if (gw < 0.001f || gh < 0.001f || gi->curve_count == 0) {
            // Empty glyph (space, etc.) — no grid needed
            for (int cell = 0; cell < GRID_COLS * GRID_ROWS; cell++) {
                font->grid_cells[font->grid_cell_count + cell].index_offset = (uint16_t)font->grid_index_count;
                font->grid_cells[font->grid_cell_count + cell].index_count = 0;
            }
            font->grid_cell_count += GRID_COLS * GRID_ROWS;
            continue;
        }

        float cell_w = gw / GRID_COLS;
        float cell_h = gh / GRID_ROWS;

        for (int row = 0; row < GRID_ROWS; row++) {
            for (int col = 0; col < GRID_COLS; col++) {
                float cmin_x = gi->bbox_min_x + col * cell_w;
                float cmin_y = gi->bbox_min_y + row * cell_h;
                float cmax_x = cmin_x + cell_w;
                float cmax_y = cmin_y + cell_h;

                GpuGridCell *cell = &font->grid_cells[font->grid_cell_count];
                cell->index_offset = (uint16_t)font->grid_index_count;
                cell->index_count = 0;

                for (uint32_t ci = 0; ci < gi->curve_count; ci++) {
                    BezierCurve *curve = &font->curves[gi->curve_offset + ci];
                    if (bezier_intersects_rect(curve, cmin_x, cmin_y, cmax_x, cmax_y)) {
                        font->grid_indices[font->grid_index_count++] = (uint16_t)ci;
                        cell->index_count++;
                    }
                }

                font->grid_cell_count++;
            }
        }
    }

    printf("Font loaded: %d curves, %d grid cells, %d grid indices\n",
           font->curve_count, font->grid_cell_count, font->grid_index_count);
    return 0;
}
```

**Step 3: Call font_load in init_externals**

After texture loading, before pipeline creation:

```cpp
if (font_load(&editor_font, "assets\\fonts\\Inter-Regular.ttf") != 0) {
    fprintf(stderr, "Failed to load editor font\n");
    return -1;
}
```

**Step 4: Verify it compiles and loads**

Run: `build.exe externals && build.exe exe && build\Debug\AnitraEngine.exe`
Expected: prints `Font loaded: N curves, N grid cells, N grid indices`

**Step 5: Commit**

```bash
git add src/externals/externals.cpp src/stb_truetype.h
git commit -m "font: load .ttf and extract Bezier curves with grid acceleration"
```

---

### Task 4: Upload font data to GPU storage buffers

**Files:**
- Modify: `src/externals/externals.cpp`

**Step 1: Add GPU buffer handles**

```cpp
static SDL_GPUBuffer *font_curve_buffer = NULL;
static SDL_GPUBuffer *font_glyph_buffer = NULL;
static SDL_GPUBuffer *font_grid_cell_buffer = NULL;
static SDL_GPUBuffer *font_grid_index_buffer = NULL;
```

**Step 2: Write font_upload_to_gpu()**

After font_load completes, upload all four buffers. Each buffer is created with `SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ` and populated via a transfer buffer + copy pass.

```cpp
static int font_upload_to_gpu(FontData *font) {
    // Helper: create storage buffer, upload data via transfer buffer
    auto upload_buffer = [](const void *data, uint32_t size, SDL_GPUBufferUsageFlags usage) -> SDL_GPUBuffer* {
        SDL_GPUBufferCreateInfo buf_info = {};
        buf_info.usage = usage;
        buf_info.size = size;
        SDL_GPUBuffer *buf = SDL_CreateGPUBuffer(gpu_device, &buf_info);

        SDL_GPUTransferBufferCreateInfo xfer_info = {};
        xfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        xfer_info.size = size;
        SDL_GPUTransferBuffer *xfer = SDL_CreateGPUTransferBuffer(gpu_device, &xfer_info);

        void *mapped = SDL_MapGPUTransferBuffer(gpu_device, xfer, false);
        memcpy(mapped, data, size);
        SDL_UnmapGPUTransferBuffer(gpu_device, xfer);

        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu_device);
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);

        SDL_GPUTransferBufferLocation src = {};
        src.transfer_buffer = xfer;
        SDL_GPUBufferRegion dst = {};
        dst.buffer = buf;
        dst.size = size;
        SDL_UploadToGPUBuffer(copy, &src, &dst, false);

        SDL_EndGPUCopyPass(copy);
        SDL_SubmitGPUCommandBuffer(cmd);
        SDL_ReleaseGPUTransferBuffer(gpu_device, xfer);
        return buf;
    };

    font_curve_buffer = upload_buffer(
        font->curves,
        font->curve_count * sizeof(BezierCurve),
        SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ);

    font_glyph_buffer = upload_buffer(
        font->glyphs,
        256 * sizeof(GlyphInfo),
        SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ);

    font_grid_cell_buffer = upload_buffer(
        font->grid_cells,
        font->grid_cell_count * sizeof(GpuGridCell),
        SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ);

    font_grid_index_buffer = upload_buffer(
        font->grid_indices,
        font->grid_index_count * sizeof(uint16_t),
        SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ);

    printf("Font GPU buffers uploaded (curves=%uB, glyphs=%uB, grid=%uB, indices=%uB)\n",
           font->curve_count * (uint32_t)sizeof(BezierCurve),
           256 * (uint32_t)sizeof(GlyphInfo),
           font->grid_cell_count * (uint32_t)sizeof(GpuGridCell),
           font->grid_index_count * (uint32_t)sizeof(uint16_t));
    return 0;
}
```

**Step 3: Call after font_load in init_externals**

```cpp
if (font_upload_to_gpu(&editor_font) != 0) {
    fprintf(stderr, "Failed to upload font to GPU\n");
    return -1;
}
```

**Step 4: Release buffers in end_externals**

```cpp
if (font_curve_buffer) SDL_ReleaseGPUBuffer(gpu_device, font_curve_buffer);
if (font_glyph_buffer) SDL_ReleaseGPUBuffer(gpu_device, font_glyph_buffer);
if (font_grid_cell_buffer) SDL_ReleaseGPUBuffer(gpu_device, font_grid_cell_buffer);
if (font_grid_index_buffer) SDL_ReleaseGPUBuffer(gpu_device, font_grid_index_buffer);
```

**Step 5: Verify**

Run: `build.exe externals && build.exe exe && build\Debug\AnitraEngine.exe`
Expected: prints `Font GPU buffers uploaded (...)` with non-zero sizes.

**Step 6: Commit**

```bash
git add src/externals/externals.cpp
git commit -m "font: upload curve and grid data to GPU storage buffers"
```

---

### Task 5: Write font shaders (Bezier winding number)

**Files:**
- Create: `assets/shaders/font_vs.hlsl`
- Create: `assets/shaders/font_fs.hlsl`

**Context:** The vertex shader transforms glyph quads to screen space and passes glyph ID + local UV. The fragment shader reads the storage buffers to evaluate the Bezier winding number per pixel.

Refer to `C:\Users\andres\zettle\docs\FONTS.md` for the full algorithm and `C:\Users\andres\Development\anitra\assets\shaders\sprite_vs.hlsl` for the descriptor set convention.

**Step 1: Write font_vs.hlsl**

```hlsl
// Vertex shader for vector font rendering
// Descriptor set 1 = vertex uniform buffers

[[vk::binding(0, 1)]]
cbuffer Constants : register(b0) {
    float4x4 projection;
    float4x4 view;
};

struct VSInput {
    float2 pos : POSITION;       // screen position of glyph quad corner
    float2 uv : TEXCOORD0;       // normalized position within glyph bbox [0,1]
    float4 color : COLOR0;       // text color
    uint glyph_id : BLENDINDICES0;  // glyph index into glyph info buffer
};

struct VSOutput {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    nointerpolation uint glyph_id : BLENDINDICES0;
};

VSOutput VSMain(VSInput input) {
    VSOutput output;
    output.pos = mul(mul(projection, view), float4(input.pos, 0.0, 1.0));
    output.uv = input.uv;
    output.color = input.color;
    output.glyph_id = input.glyph_id;
    return output;
}
```

**Step 2: Write font_fs.hlsl**

```hlsl
// Fragment shader for GPU vector font rendering
// Evaluates Bezier winding number per pixel with grid acceleration
// Descriptor set 2 = fragment SRVs (storage buffers)
// Descriptor set 3 = fragment uniform buffers

struct BezierCurve {
    float2 p0;
    float2 p1;
    float2 p2;
};

struct GlyphInfo {
    uint curve_offset;
    uint curve_count;
    float2 bbox_min;
    float2 bbox_max;
    float advance_width;
    float left_bearing;
    uint grid_offset;
    uint grid_cols;  // packed: cols in low 16, rows in high 16
};

struct GpuGridCell {
    uint packed;  // index_offset in low 16, index_count in high 16
};

[[vk::binding(0, 2)]]
StructuredBuffer<BezierCurve> curves : register(t0);
[[vk::binding(1, 2)]]
StructuredBuffer<GlyphInfo> glyphs : register(t1);
[[vk::binding(2, 2)]]
StructuredBuffer<GpuGridCell> grid_cells : register(t2);
[[vk::binding(3, 2)]]
StructuredBuffer<uint> grid_indices : register(t3); // uint16 packed as uint

struct VSOutput {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    nointerpolation uint glyph_id : BLENDINDICES0;
};

float4 PSMain(VSOutput input) : SV_Target {
    GlyphInfo glyph = glyphs[input.glyph_id];

    if (glyph.curve_count == 0)
        discard;

    // Map UV to glyph-local coordinates
    float2 bbox_size = glyph.bbox_max - glyph.bbox_min;
    float2 local_pos = glyph.bbox_min + input.uv * bbox_size;

    // Grid cell lookup
    uint cols = glyph.grid_cols & 0xFFFF;
    uint rows = glyph.grid_cols >> 16;
    float2 cell_uv = saturate(input.uv);
    uint2 cell_coord = uint2(cell_uv * float2(cols, rows));
    cell_coord = min(cell_coord, uint2(cols - 1, rows - 1));

    uint cell_idx = cell_coord.y * cols + cell_coord.x;
    GpuGridCell cell = grid_cells[glyph.grid_offset + cell_idx];
    uint idx_offset = cell.packed & 0xFFFF;
    uint idx_count = cell.packed >> 16;

    // Evaluate winding number
    float coverage = 0.0;

    for (uint i = 0; i < idx_count; i++) {
        uint curve_local_idx = grid_indices[idx_offset + i];
        BezierCurve curve = curves[glyph.curve_offset + curve_local_idx];

        // Transform to pixel-local (subtract local_pos so pixel center is origin)
        float2 p0 = curve.p0 - local_pos;
        float2 p1 = curve.p1 - local_pos;
        float2 p2 = curve.p2 - local_pos;

        // Solve quadratic for y = 0 (horizontal ray through pixel)
        float a = p0.y - 2.0 * p1.y + p2.y;
        float b = -2.0 * p0.y + 2.0 * p1.y;
        float c = p0.y;

        // Handle near-linear case
        if (abs(a) < 1e-6) {
            if (abs(b) < 1e-6) continue;
            float t = -c / b;
            if (t >= 0.0 && t <= 1.0) {
                float x = (1.0 - t) * (1.0 - t) * p0.x + 2.0 * t * (1.0 - t) * p1.x + t * t * p2.x;
                if (x > 0.0) {
                    float dy = 2.0 * (a * t) + b;
                    coverage += dy > 0.0 ? 1.0 : -1.0;
                }
            }
            continue;
        }

        float disc = b * b - 4.0 * a * c;
        if (disc < 0.0) continue;

        float sqrt_disc = sqrt(disc);
        float inv_2a = 0.5 / a;

        float t0 = (-b - sqrt_disc) * inv_2a;
        float t1 = (-b + sqrt_disc) * inv_2a;

        // Process each crossing
        [unroll]
        for (int j = 0; j < 2; j++) {
            float t = (j == 0) ? t0 : t1;
            if (t < 0.0 || t > 1.0) continue;

            float omt = 1.0 - t;
            float x = omt * omt * p0.x + 2.0 * t * omt * p1.x + t * t * p2.x;
            if (x > 0.0) {
                float dy = 2.0 * a * t + b;
                coverage += dy > 0.0 ? 1.0 : -1.0;
            }
        }
    }

    float alpha = abs(coverage);
    alpha = clamp(alpha, 0.0, 1.0);

    if (alpha < 0.01)
        discard;

    return float4(input.color.rgb, input.color.a * alpha);
}
```

**Step 3: Commit**

```bash
git add assets/shaders/font_vs.hlsl assets/shaders/font_fs.hlsl
git commit -m "shaders: vector font vertex and fragment shaders"
```

---

### Task 6: Write UI rect shaders

**Files:**
- Create: `assets/shaders/ui_rect_vs.hlsl`
- Create: `assets/shaders/ui_rect_fs.hlsl`

**Step 1: Write ui_rect_vs.hlsl**

```hlsl
[[vk::binding(0, 1)]]
cbuffer Constants : register(b0) {
    float4x4 projection;
    float4x4 view;
};

struct VSInput {
    float2 pos : POSITION;
    float4 color : COLOR0;
};

struct VSOutput {
    float4 pos : SV_Position;
    float4 color : COLOR0;
};

VSOutput VSMain(VSInput input) {
    VSOutput output;
    output.pos = mul(mul(projection, view), float4(input.pos, 0.0, 1.0));
    output.color = input.color;
    return output;
}
```

**Step 2: Write ui_rect_fs.hlsl**

```hlsl
struct VSOutput {
    float4 pos : SV_Position;
    float4 color : COLOR0;
};

float4 PSMain(VSOutput input) : SV_Target {
    return input.color;
}
```

**Step 3: Commit**

```bash
git add assets/shaders/ui_rect_vs.hlsl assets/shaders/ui_rect_fs.hlsl
git commit -m "shaders: UI rect vertex and fragment shaders"
```

---

### Task 7: Create UI rect and font pipelines

**Files:**
- Modify: `src/externals/externals.cpp`

**Step 1: Add pipeline handles and vertex structs**

```cpp
static SDL_GPUGraphicsPipeline *ui_rect_pipeline = NULL;
static SDL_GPUGraphicsPipeline *font_pipeline = NULL;

struct ui_rect_vertex {
    float x, y;
    float r, g, b, a;
};

struct font_vertex {
    float x, y;       // screen position
    float u, v;        // UV within glyph bbox
    float r, g, b, a;  // text color
    uint32_t glyph_id; // glyph index
};
```

**Step 2: Create both pipelines in init_externals**

Follow the same pattern as sprite_pipeline creation (compile shader, create pipeline with vertex layout, blend state, etc.). The ui_rect_pipeline is nearly identical to the debug_lines_pipeline but with float4 color instead of float3. The font_pipeline needs the `num_storage_buffers` field set to 4 in the fragment shader info.

**Step 3: Verify**

Run: `build.exe externals && build.exe exe && build\Debug\AnitraEngine.exe`
Expected: no shader compile errors, no pipeline creation failures.

**Step 4: Commit**

```bash
git add src/externals/externals.cpp
git commit -m "pipelines: create UI rect and font GPU pipelines"
```

---

### Task 8: Initialize HarfBuzz with stb_truetype backend

**Files:**
- Modify: `src/externals/externals.cpp`

**Step 1: Add HarfBuzz includes and font handle**

```cpp
#include <hb.h>
#include <hb-ot.h>

static hb_font_t *hb_font = NULL;
static hb_blob_t *hb_blob = NULL;
static hb_face_t *hb_face = NULL;
```

**Step 2: Initialize HarfBuzz after font_load**

```cpp
static int harfbuzz_init(FontData *font) {
    hb_blob = hb_blob_create((const char*)font->ttf_buffer,
        /* length — need file size stored in FontData */,
        HB_MEMORY_MODE_READONLY, NULL, NULL);
    hb_face = hb_face_create(hb_blob, 0);
    hb_font = hb_font_create(hb_face);
    hb_ot_font_set_funcs(hb_font);
    hb_font_set_scale(hb_font, 64, 64); // 26.6 fixed point, 1pt = 64 units
    printf("HarfBuzz initialized\n");
    return 0;
}
```

Note: `hb_ot_font_set_funcs` tells HarfBuzz to use OpenType tables directly from the font blob — it reads the tables itself without needing stb_truetype callbacks. This is the simplest integration path.

**Step 3: Write text measurement function for Clay**

```cpp
static float font_measure_width(FontData *font, const char *text, uint32_t len, float font_size) {
    hb_buffer_t *buf = hb_buffer_create();
    hb_buffer_add_utf8(buf, text, len, 0, len);
    hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
    hb_buffer_set_script(buf, HB_SCRIPT_LATIN);
    hb_buffer_set_language(buf, hb_language_from_string("en", -1));

    hb_shape(hb_font, buf, NULL, 0);

    unsigned int glyph_count;
    hb_glyph_position_t *positions = hb_buffer_get_glyph_positions(buf, &glyph_count);

    float width = 0;
    for (unsigned int i = 0; i < glyph_count; i++) {
        width += positions[i].x_advance;
    }
    hb_buffer_destroy(buf);

    // Convert from 26.6 fixed point, scale to desired font size
    // HarfBuzz uses font units at scale set by hb_font_set_scale
    float scale = font_size / 64.0f; // since we set scale to 64
    return width * scale / 64.0f; // divide by 64 for 26.6 fixed point
}
```

**Step 4: Cleanup in end_externals**

```cpp
if (hb_font) hb_font_destroy(hb_font);
if (hb_face) hb_face_destroy(hb_face);
if (hb_blob) hb_blob_destroy(hb_blob);
```

**Step 5: Verify**

Run: `build.exe externals && build.exe exe && build\Debug\AnitraEngine.exe`
Expected: prints `HarfBuzz initialized`

**Step 6: Commit**

```bash
git add src/externals/externals.cpp
git commit -m "harfbuzz: initialize with OT font funcs for text shaping"
```

---

### Task 9: Integrate Clay — init, layout, and render commands

**Files:**
- Modify: `src/externals/externals.cpp`

**Step 1: Add Clay include (with implementation)**

```cpp
#define CLAY_IMPLEMENTATION
#include "clay.h"
```

**Step 2: Add Clay initialization in init_externals**

```cpp
static uint8_t *clay_memory = NULL;

// In init_externals:
uint64_t clay_mem_size = Clay_MinMemorySize();
clay_memory = (uint8_t*)malloc(clay_mem_size);
Clay_Arena clay_arena = Clay_CreateArenaWithCapacityAndMemory(clay_mem_size, clay_memory);

int window_w, window_h;
SDL_GetWindowSize(window, &window_w, &window_h);

Clay_Initialize(clay_arena, (Clay_Dimensions){(float)window_w, (float)window_h},
    (Clay_ErrorHandler){ /* .errorHandlerFunction = */ NULL });

// Wire text measurement
Clay_SetMeasureTextFunction(
    [](Clay_StringSlice text, Clay_TextElementConfig *config, void *userData) -> Clay_Dimensions {
        float width = font_measure_width(&editor_font, text.chars, text.length, config->fontSize);
        float height = config->fontSize * (editor_font.ascent - editor_font.descent + editor_font.line_gap);
        return (Clay_Dimensions){ width, height };
    }, NULL);
```

**Step 3: Add Clay layout + render in update_externals**

After the existing sprite/line render pass, add:

```cpp
// --- Clay UI pass ---
int win_w, win_h;
SDL_GetWindowSize(window, &win_w, &win_h);
Clay_SetLayoutDimensions((Clay_Dimensions){(float)win_w, (float)win_h});

// TODO: pipe mouse input from SDL events
// Clay_SetPointerState((Clay_Vector2){mouse_x, mouse_y}, mouse_down);

Clay_BeginLayout();

// Test: one rectangle with text
CLAY(CLAY_ID("TestPanel"),
     CLAY_LAYOUT({ .sizing = { CLAY_SIZING_FIXED(300), CLAY_SIZING_FIXED(100) },
                   .padding = CLAY_PADDING_ALL(16) }),
     CLAY_RECTANGLE({ .color = { 40, 40, 40, 220 } })) {
    CLAY_TEXT(CLAY_STRING("Hello from Clay!"),
              CLAY_TEXT_CONFIG({ .fontSize = 24, .textColor = { 255, 255, 255, 255 } }));
}

Clay_RenderCommandArray commands = Clay_EndLayout();

// Process commands -> emit vertices -> draw
// (implementation in next task)
```

**Step 4: Commit**

```bash
git add src/externals/externals.cpp
git commit -m "clay: initialize and declare test panel with text"
```

---

### Task 10: Implement ui_render — process Clay commands into draw calls

**Files:**
- Modify: `src/externals/externals.cpp`

**Step 1: Write ui_render function**

This iterates Clay's render command array, emits vertices for rects and text into dynamic buffers, uploads to GPU, and draws with the appropriate pipelines.

For RECTANGLE commands: emit 6 vertices (2 triangles) per quad with the rectangle's color.

For TEXT commands: use HarfBuzz to shape the text, then emit one quad (6 vertices) per glyph with the glyph's bounding box mapped to screen coordinates.

For SCISSOR_START/END: call `SDL_SetGPUScissor` / reset.

**Step 2: Wire it into the render pass**

After `Clay_EndLayout()`, call `ui_render(commands, render_pass, cmd_buf, &uniforms)`.

**Step 3: Verify end-to-end**

Run: `build.exe externals && build.exe exe && build\Debug\AnitraEngine.exe`
Expected: a dark rectangle with "Hello from Clay!" rendered in vector text at the top-left of the window.

**Step 4: Commit**

```bash
git add src/externals/externals.cpp
git commit -m "ui: render Clay commands — rectangles and vector text"
```

---

### Task 11: Add a real editor panel

**Files:**
- Modify: `src/externals/externals.cpp`

**Step 1: Replace test panel with a real editor panel**

Add an entity inspector panel that shows the player position and game state. Wire up Clay's pointer state from SDL mouse events.

**Step 2: Verify**

Run: `build.exe externals && build.exe exe && build\Debug\AnitraEngine.exe`
Expected: an inspector panel rendered on screen, text readable at various sizes.

**Step 3: Commit**

```bash
git add src/externals/externals.cpp
git commit -m "editor: add entity inspector panel via Clay"
```

---

### Task 12: End-to-end verification

**Step 1: Full build**

Run: `build.exe all`
Expected: clean build

**Step 2: Run and verify rendering**

- Rectangles render with correct colors
- Text is sharp at different sizes (try 12px, 18px, 24px, 48px)
- No GPU errors or validation messages
- Performance: check that UI rendering doesn't drop below 60fps

**Step 3: Test with forge hot-reload**

Run `build.exe watch` + `AnitraEngine.exe`, edit engine code, verify hot-reload still works alongside the new UI system.

**Step 4: Commit any fixes**

```bash
git add -A
git commit -m "editor UI: end-to-end verified"
```
