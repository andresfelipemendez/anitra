#include <externals.h>
#include <game.h>
#include <debug_render.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3_shadercross/SDL_shadercross.h>

#include <tracy/Tracy.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <hb.h>
#include <hb-ot.h>

#define CLAY_IMPLEMENTATION
#include "clay.h"

#include "gltf_types.h"

// Forward declarations from gltf_loader.cpp
extern GltfModel load_glb(const char *path, arena *a);
extern void load_animations_glb(const char *path, GltfModel *model, arena *a);

// ---------------------------------------------------------------------------
// Static globals (replace GL state)
// ---------------------------------------------------------------------------

static SDL_Window *window = NULL;
SDL_GPUDevice *gpu_device = NULL;  /* non-static: shared with gltf_loader.cpp */

// Sprite pipeline
static SDL_GPUGraphicsPipeline *sprite_pipeline = NULL;
static SDL_GPUSampler *sprite_sampler = NULL;

// Debug line pipeline
static SDL_GPUGraphicsPipeline *line_pipeline = NULL;

// UI rect pipeline (Clay rectangles)
static SDL_GPUGraphicsPipeline *ui_rect_pipeline = NULL;

// Font pipeline (vector text)
static SDL_GPUGraphicsPipeline *font_pipeline = NULL;

// Mesh (3D skinned) pipeline
static SDL_GPUGraphicsPipeline *mesh_pipeline = NULL;
static SDL_GPUSampler *mesh_sampler = NULL;
static SDL_GPUTexture *depth_texture = NULL;
static Uint32 depth_w = 0, depth_h = 0;
static SDL_GPUBuffer *bone_storage_buffer = NULL;
static SDL_GPUTexture *white_texture = NULL;

// glTF model — GPU primitives kept here (not in game struct)
static GltfModel loaded_model = {};

#define MAX_BONES 128

// Font GPU storage buffers
static SDL_GPUBuffer *font_curve_buffer = NULL;
static SDL_GPUBuffer *font_glyph_buffer = NULL;
static SDL_GPUBuffer *font_grid_cell_buffer = NULL;
static SDL_GPUBuffer *font_grid_index_buffer = NULL;

// HarfBuzz
static hb_font_t *hb_editor_font = NULL;
static hb_blob_t *hb_editor_blob = NULL;
static hb_face_t *hb_editor_face = NULL;

// Clay UI — game window
static arena         *clay_arena_game = NULL;    // sub-arena backing Clay game context
static Clay_Context  *clay_context = NULL;

// Profiler window
static SDL_Window    *profiler_window = NULL;
static arena         *clay_arena_prof = NULL;    // sub-arena backing Clay profiler context
static Clay_Context  *profiler_clay_context = NULL;
static bool           profiler_open = true;

// Textures (one per TextureID enum)
static SDL_GPUTexture *gpu_textures[TEXTURE_COUNT] = {NULL};

// Callbacks
static init g_init = NULL;
static destroy g_destroy = NULL;
static update g_update = NULL;

// ---------------------------------------------------------------------------
// Vertex structures
// ---------------------------------------------------------------------------

struct sprite_vertex {
    float x, y;       // world position
    float u, v;        // UV coordinates
    float r, g, b, a;  // tint color
};

struct line_vertex {
    float x, y;       // world position
    float r, g, b;    // color
};

struct ui_rect_vertex {
    float x, y;
    float r, g, b, a;
};

struct font_vertex {
    float x, y;       // screen position
    float u, v;        // UV within glyph bbox [0,1]
    float r, g, b, a;  // text color
    uint32_t glyph_id; // glyph index
};

// ---------------------------------------------------------------------------
// Uniform data (projection + view, matching HLSL cbuffer)
// ---------------------------------------------------------------------------

struct uniform_data {
    float projection[16];
    float view[16];
};

struct mesh_uniform_data {
    float projection[16];
    float view[16];
    float model[16];
};

// ---------------------------------------------------------------------------
// Font data structures
// ---------------------------------------------------------------------------

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
    uint32_t grid_cols;
    uint32_t grid_rows;
};

#define MAX_CURVES      32768
#define MAX_GRID_CELLS  (256 * 64)   // 256 glyphs * 8x8 grid
#define MAX_GRID_INDICES (256 * 64 * 8) // avg 8 curves per cell
#define GRID_COLS 8
#define GRID_ROWS 8

struct FontData {
    stbtt_fontinfo stb_info;
    unsigned char *ttf_buffer;
    size_t ttf_size;

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

// ---------------------------------------------------------------------------
// Helper: compile HLSL -> SPIRV -> SDL_GPUShader
// ---------------------------------------------------------------------------

static SDL_GPUShader* load_shader_from_spirv(
    const char* filename,
    const char* entrypoint,
    SDL_ShaderCross_ShaderStage stage)
{
    // Read pre-compiled SPIR-V from disk
    size_t spirv_size = 0;
    void* spirv_bytecode = SDL_LoadFile(filename, &spirv_size);
    if (!spirv_bytecode) {
        fprintf(stderr, "Failed to load SPIR-V file: %s (%s)\n", filename, SDL_GetError());
        return NULL;
    }

    // Reflect to get resource info
    SDL_ShaderCross_GraphicsShaderMetadata* metadata =
        SDL_ShaderCross_ReflectGraphicsSPIRV((const Uint8*)spirv_bytecode, spirv_size, 0);
    if (!metadata) {
        fprintf(stderr, "Failed to reflect SPIRV: %s (%s)\n", filename, SDL_GetError());
        SDL_free(spirv_bytecode);
        return NULL;
    }

    // Compile SPIRV -> GPU shader
    SDL_ShaderCross_SPIRV_Info spirv_info = {};
    spirv_info.bytecode = (const Uint8*)spirv_bytecode;
    spirv_info.bytecode_size = spirv_size;
    spirv_info.entrypoint = entrypoint;
    spirv_info.shader_stage = stage;
    spirv_info.props = 0;

    SDL_GPUShader* shader = SDL_ShaderCross_CompileGraphicsShaderFromSPIRV(
        gpu_device, &spirv_info, &metadata->resource_info, 0);

    SDL_free(metadata);
    SDL_free(spirv_bytecode);

    if (!shader) {
        fprintf(stderr, "Failed to compile GPU shader: %s (%s)\n", filename, SDL_GetError());
    }

    return shader;
}

// ---------------------------------------------------------------------------
// Helper: load texture via stb_image -> SDL_GPUTexture
// ---------------------------------------------------------------------------

static SDL_GPUTexture* load_gpu_texture(const char* filepath) {
    int w, h, channels;
    unsigned char* data = stbi_load(filepath, &w, &h, &channels, 4); // force RGBA
    if (!data) {
        fprintf(stderr, "Failed to load image: %s\n", filepath);
        return NULL;
    }

    Uint32 image_size = (Uint32)(w * h * 4);

    // Create GPU texture
    SDL_GPUTextureCreateInfo tex_info = {};
    tex_info.type = SDL_GPU_TEXTURETYPE_2D;
    tex_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    tex_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tex_info.width = (Uint32)w;
    tex_info.height = (Uint32)h;
    tex_info.layer_count_or_depth = 1;
    tex_info.num_levels = 1;
    tex_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    tex_info.props = 0;

    SDL_GPUTexture* texture = SDL_CreateGPUTexture(gpu_device, &tex_info);
    if (!texture) {
        fprintf(stderr, "Failed to create GPU texture: %s (%s)\n", filepath, SDL_GetError());
        stbi_image_free(data);
        return NULL;
    }

    // Create transfer buffer
    SDL_GPUTransferBufferCreateInfo tbuf_info = {};
    tbuf_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbuf_info.size = image_size;
    tbuf_info.props = 0;

    SDL_GPUTransferBuffer* transfer_buf = SDL_CreateGPUTransferBuffer(gpu_device, &tbuf_info);
    if (!transfer_buf) {
        fprintf(stderr, "Failed to create transfer buffer for texture: %s (%s)\n",
                filepath, SDL_GetError());
        SDL_ReleaseGPUTexture(gpu_device, texture);
        stbi_image_free(data);
        return NULL;
    }

    // Map, copy, unmap
    void* mapped = SDL_MapGPUTransferBuffer(gpu_device, transfer_buf, false);
    memcpy(mapped, data, image_size);
    SDL_UnmapGPUTransferBuffer(gpu_device, transfer_buf);
    stbi_image_free(data);

    // Upload via copy pass
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu_device);
    SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(cmd);

    SDL_GPUTextureTransferInfo src = {};
    src.transfer_buffer = transfer_buf;
    src.offset = 0;
    src.pixels_per_row = 0;
    src.rows_per_layer = 0;

    SDL_GPUTextureRegion dst = {};
    dst.texture = texture;
    dst.mip_level = 0;
    dst.layer = 0;
    dst.x = 0;
    dst.y = 0;
    dst.z = 0;
    dst.w = (Uint32)w;
    dst.h = (Uint32)h;
    dst.d = 1;

    SDL_UploadToGPUTexture(copy_pass, &src, &dst, false);
    SDL_EndGPUCopyPass(copy_pass);
    SDL_SubmitGPUCommandBuffer(cmd);

    SDL_ReleaseGPUTransferBuffer(gpu_device, transfer_buf);

    printf("Loaded texture: %s (%dx%d)\n", filepath, w, h);
    return texture;
}

// ---------------------------------------------------------------------------
// Font loading
// ---------------------------------------------------------------------------

static int bezier_intersects_rect(BezierCurve *c,
    float min_x, float min_y, float max_x, float max_y)
{
    float cx_min = fminf(fminf(c->p0x, c->p1x), c->p2x);
    float cx_max = fmaxf(fmaxf(c->p0x, c->p1x), c->p2x);
    float cy_min = fminf(fminf(c->p0y, c->p1y), c->p2y);
    float cy_max = fmaxf(fmaxf(c->p0y, c->p1y), c->p2y);
    return !(cx_max < min_x || cx_min > max_x ||
             cy_max < min_y || cy_min > max_y);
}

static int font_load(FontData *font, const char *path) {
    size_t size = 0;
    font->ttf_buffer = (unsigned char*)SDL_LoadFile(path, &size);
    if (!font->ttf_buffer) {
        fprintf(stderr, "Failed to load font: %s\n", path);
        return -1;
    }
    font->ttf_size = size;

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

        float cx = 0, cy = 0;
        for (int v = 0; v < num_verts; v++) {
            stbtt_vertex *vert = &vertices[v];
            float vx = vert->x * scale;
            float vy = vert->y * scale;

            if (vert->type == STBTT_vmove) {
                cx = vx; cy = vy;
            } else if (vert->type == STBTT_vline) {
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
            } else if (vert->type == STBTT_vcubic) {
                // Cubic P0(cx,cy), P1(c1), P2(c2), P3(vx,vy)
                // Standard N=2 cubic-to-quadratic conversion:
                //   Q0 = P0
                //   Q1 = (P0 + 3*P1) / 4
                //   Q2 = (P1 + P2) / 2        (split point)
                //   Q3 = (3*P2 + P3) / 4
                //   Q4 = P3
                float c1x = vert->cx * scale, c1y = vert->cy * scale;
                float c2x = vert->cx1 * scale, c2y = vert->cy1 * scale;

                float splitx = (c1x + c2x) * 0.5f;
                float splity = (c1y + c2y) * 0.5f;

                BezierCurve *c_a = &font->curves[font->curve_count++];
                c_a->p0x = cx;   c_a->p0y = cy;
                c_a->p1x = (cx + 3.0f * c1x) * 0.25f;
                c_a->p1y = (cy + 3.0f * c1y) * 0.25f;
                c_a->p2x = splitx; c_a->p2y = splity;
                gi->curve_count++;

                BezierCurve *c_b = &font->curves[font->curve_count++];
                c_b->p0x = splitx; c_b->p0y = splity;
                c_b->p1x = (3.0f * c2x + vx) * 0.25f;
                c_b->p1y = (3.0f * c2y + vy) * 0.25f;
                c_b->p2x = vx;    c_b->p2y = vy;
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

// ---------------------------------------------------------------------------
// Font GPU upload
// ---------------------------------------------------------------------------

static SDL_GPUBuffer* upload_storage_buffer(const void *data, uint32_t size) {
    SDL_GPUBufferCreateInfo buf_info = {};
    buf_info.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
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
}

static int font_upload_to_gpu(FontData *font) {
    font_curve_buffer = upload_storage_buffer(
        font->curves,
        font->curve_count * sizeof(BezierCurve));

    font_glyph_buffer = upload_storage_buffer(
        font->glyphs,
        256 * sizeof(GlyphInfo));

    font_grid_cell_buffer = upload_storage_buffer(
        font->grid_cells,
        font->grid_cell_count * sizeof(GpuGridCell));

    font_grid_index_buffer = upload_storage_buffer(
        font->grid_indices,
        font->grid_index_count * sizeof(uint16_t));

    printf("Font GPU buffers uploaded (curves=%uB, glyphs=%uB, grid=%uB, indices=%uB)\n",
           font->curve_count * (uint32_t)sizeof(BezierCurve),
           256 * (uint32_t)sizeof(GlyphInfo),
           font->grid_cell_count * (uint32_t)sizeof(GpuGridCell),
           font->grid_index_count * (uint32_t)sizeof(uint16_t));
    return 0;
}

// ---------------------------------------------------------------------------
// HarfBuzz init and text measurement
// ---------------------------------------------------------------------------

static unsigned int hb_scale = 0;  // units per em used for HarfBuzz

static int harfbuzz_init(FontData *font) {
    hb_editor_blob = hb_blob_create((const char*)font->ttf_buffer,
        (unsigned int)font->ttf_size, HB_MEMORY_MODE_READONLY, NULL, NULL);
    hb_editor_face = hb_face_create(hb_editor_blob, 0);
    hb_editor_font = hb_font_create(hb_editor_face);
    hb_ot_font_set_funcs(hb_editor_font);
    hb_scale = hb_face_get_upem(hb_editor_face);
    hb_font_set_scale(hb_editor_font, (int)hb_scale, (int)hb_scale);
    printf("HarfBuzz initialized (upem=%u)\n", hb_scale);
    return 0;
}

static float font_measure_width(FontData *font, const char *text, uint32_t len, float font_size) {
    hb_buffer_t *buf = hb_buffer_create();
    hb_buffer_add_utf8(buf, text, (int)len, 0, (int)len);
    hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
    hb_buffer_set_script(buf, HB_SCRIPT_LATIN);
    hb_buffer_set_language(buf, hb_language_from_string("en", -1));

    hb_shape(hb_editor_font, buf, NULL, 0);

    unsigned int glyph_count;
    hb_glyph_position_t *positions = hb_buffer_get_glyph_positions(buf, &glyph_count);

    float width = 0;
    for (unsigned int i = 0; i < glyph_count; i++) {
        width += positions[i].x_advance;
    }
    hb_buffer_destroy(buf);

    float scale = font_size / (float)hb_scale;
    return width * scale;
}

// ---------------------------------------------------------------------------
// Clay text measurement callback
// ---------------------------------------------------------------------------

static Clay_Dimensions clay_measure_text(Clay_StringSlice text, Clay_TextElementConfig *config, void *userData) {
    (void)userData;
    float width = font_measure_width(&editor_font, text.chars, text.length, (float)config->fontSize);
    // Ceil to account for pixel snapping in the renderer (prevents glyph clipping)
    float height = ceilf((float)config->fontSize * (editor_font.ascent - editor_font.descent + editor_font.line_gap));
    return Clay_Dimensions{ceilf(width + 1.0f), height};
}

// ---------------------------------------------------------------------------
// UI rendering: process Clay render commands -> GPU draw calls
// ---------------------------------------------------------------------------

#define MAX_UI_RECT_VERTICES  (4096 * 6)
#define MAX_FONT_VERTICES     (4096 * 6)

// Per-window UI render state
struct UIRenderState {
    ui_rect_vertex rect_verts[MAX_UI_RECT_VERTICES];
    font_vertex    font_verts[MAX_FONT_VERTICES];
    int            rect_vert_count;
    int            font_vert_count;
    SDL_GPUBuffer *rect_gpu_buf;
    SDL_GPUBuffer *font_gpu_buf;
};

static UIRenderState ui_game = {};
static UIRenderState ui_profiler = {};

// ---------------------------------------------------------------------------
// Memory profiler colors (one per allocation slot)
// ---------------------------------------------------------------------------
static const Clay_Color profiler_colors[] = {
    {100, 160, 220, 255},  // blue
    {220, 140,  80, 255},  // orange
    {100, 200, 120, 255},  // green
    {200, 100, 180, 255},  // pink
    {180, 180, 100, 255},  // yellow
    {130, 120, 220, 255},  // purple
    {100, 200, 200, 255},  // teal
    {220, 110, 110, 255},  // red
};
static const int profiler_color_count = sizeof(profiler_colors) / sizeof(profiler_colors[0]);

// Format bytes as human-readable string (rotating static buffers)
static const char *format_bytes(uint32_t bytes) {
    static char bufs[4][32];
    static int idx = 0;
    char *buf = bufs[idx++ & 3];
    if (bytes >= 1024 * 1024)
        snprintf(buf, 32, "%.2f MB", bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024)
        snprintf(buf, 32, "%.1f KB", bytes / 1024.0);
    else
        snprintf(buf, 32, "%u B", bytes);
    return buf;
}

// ---------------------------------------------------------------------------
// Build Clay render commands → vertex arrays (window-agnostic)
// ---------------------------------------------------------------------------
static void ui_build_vertices(UIRenderState *ui, Clay_RenderCommandArray commands) {
    ui->rect_vert_count = 0;
    ui->font_vert_count = 0;

    for (int32_t i = 0; i < commands.length; i++) {
        Clay_RenderCommand *cmd = Clay_RenderCommandArray_Get(&commands, i);
        Clay_BoundingBox box = cmd->boundingBox;

        switch (cmd->commandType) {
        case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
            Clay_RectangleRenderData *rect = &cmd->renderData.rectangle;
            float r = rect->backgroundColor.r / 255.0f;
            float g = rect->backgroundColor.g / 255.0f;
            float b = rect->backgroundColor.b / 255.0f;
            float a = rect->backgroundColor.a / 255.0f;

            float x0 = box.x, y0 = box.y;
            float x1 = box.x + box.width, y1 = box.y + box.height;

            if (ui->rect_vert_count + 6 <= MAX_UI_RECT_VERTICES) {
                ui_rect_vertex *v = &ui->rect_verts[ui->rect_vert_count];
                v[0] = {x0, y0, r, g, b, a};
                v[1] = {x1, y0, r, g, b, a};
                v[2] = {x1, y1, r, g, b, a};
                v[3] = {x0, y0, r, g, b, a};
                v[4] = {x1, y1, r, g, b, a};
                v[5] = {x0, y1, r, g, b, a};
                ui->rect_vert_count += 6;
            }
            break;
        }
        case CLAY_RENDER_COMMAND_TYPE_TEXT: {
            Clay_TextRenderData *text = &cmd->renderData.text;
            float r = text->textColor.r / 255.0f;
            float g = text->textColor.g / 255.0f;
            float b = text->textColor.b / 255.0f;
            float a = text->textColor.a / 255.0f;
            float font_size = (float)text->fontSize;

            hb_buffer_t *hb_buf = hb_buffer_create();
            hb_buffer_add_utf8(hb_buf, text->stringContents.chars,
                (int)text->stringContents.length, 0, (int)text->stringContents.length);
            hb_buffer_set_direction(hb_buf, HB_DIRECTION_LTR);
            hb_buffer_set_script(hb_buf, HB_SCRIPT_LATIN);
            hb_buffer_set_language(hb_buf, hb_language_from_string("en", -1));
            hb_shape(hb_editor_font, hb_buf, NULL, 0);

            unsigned int glyph_count;
            hb_glyph_info_t *glyph_infos = hb_buffer_get_glyph_infos(hb_buf, &glyph_count);
            hb_glyph_position_t *glyph_positions = hb_buffer_get_glyph_positions(hb_buf, &glyph_count);

            float cursor_x = floorf(box.x);
            float baseline_y = floorf(box.y + font_size * editor_font.ascent);
            float scale = font_size / (float)hb_scale;

            for (unsigned int gi = 0; gi < glyph_count; gi++) {
                uint32_t cluster = glyph_infos[gi].cluster;
                uint32_t cp = 0;
                if (cluster < text->stringContents.length)
                    cp = (uint32_t)(unsigned char)text->stringContents.chars[cluster];

                if (cp < 32 || cp >= 127) {
                    cursor_x += glyph_positions[gi].x_advance * scale;
                    continue;
                }

                GlyphInfo *info = &editor_font.glyphs[cp];

                if (info->curve_count > 0) {
                    float glyph_w = (info->bbox_max_x - info->bbox_min_x) * font_size;
                    float glyph_h = (info->bbox_max_y - info->bbox_min_y) * font_size;

                    // Snap X to pixel grid for consistent spacing; leave Y exact
                    // so all glyphs share the same snapped baseline.
                    float gx = floorf(cursor_x + info->bbox_min_x * font_size + glyph_positions[gi].x_offset * scale);
                    float gy = baseline_y - info->bbox_max_y * font_size + glyph_positions[gi].y_offset * scale;
                    float gw = glyph_w;
                    float gh = glyph_h;

                    if (ui->font_vert_count + 6 <= MAX_FONT_VERTICES) {
                        font_vertex *v = &ui->font_verts[ui->font_vert_count];
                        v[0] = {gx,      gy,      0, 0, r, g, b, a, cp};
                        v[1] = {gx + gw, gy,      1, 0, r, g, b, a, cp};
                        v[2] = {gx + gw, gy + gh, 1, 1, r, g, b, a, cp};
                        v[3] = {gx,      gy,      0, 0, r, g, b, a, cp};
                        v[4] = {gx + gw, gy + gh, 1, 1, r, g, b, a, cp};
                        v[5] = {gx,      gy + gh, 0, 1, r, g, b, a, cp};
                        ui->font_vert_count += 6;
                    }
                }

                cursor_x += glyph_positions[gi].x_advance * scale;
            }
            hb_buffer_destroy(hb_buf);
            break;
        }
        default:
            break;
        }
    }
}

// Upload a UIRenderState's vertex arrays to GPU via copy pass
static void ui_upload(SDL_GPUCommandBuffer *cmd_buf, UIRenderState *ui) {
    if (ui->rect_vert_count > 0) {
        Uint32 buf_size = (Uint32)(ui->rect_vert_count * sizeof(ui_rect_vertex));

        SDL_GPUTransferBufferCreateInfo tbuf_info = {};
        tbuf_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbuf_info.size = buf_size;
        SDL_GPUTransferBuffer *xfer = SDL_CreateGPUTransferBuffer(gpu_device, &tbuf_info);
        void *mapped = SDL_MapGPUTransferBuffer(gpu_device, xfer, false);
        memcpy(mapped, ui->rect_verts, buf_size);
        SDL_UnmapGPUTransferBuffer(gpu_device, xfer);

        SDL_GPUBufferCreateInfo gbuf_info = {};
        gbuf_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        gbuf_info.size = buf_size;
        ui->rect_gpu_buf = SDL_CreateGPUBuffer(gpu_device, &gbuf_info);

        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd_buf);
        SDL_GPUTransferBufferLocation src = {};
        src.transfer_buffer = xfer;
        SDL_GPUBufferRegion dst = {};
        dst.buffer = ui->rect_gpu_buf;
        dst.size = buf_size;
        SDL_UploadToGPUBuffer(copy, &src, &dst, false);
        SDL_EndGPUCopyPass(copy);
        SDL_ReleaseGPUTransferBuffer(gpu_device, xfer);
    }

    if (ui->font_vert_count > 0) {
        Uint32 buf_size = (Uint32)(ui->font_vert_count * sizeof(font_vertex));

        SDL_GPUTransferBufferCreateInfo tbuf_info = {};
        tbuf_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbuf_info.size = buf_size;
        SDL_GPUTransferBuffer *xfer = SDL_CreateGPUTransferBuffer(gpu_device, &tbuf_info);
        void *mapped = SDL_MapGPUTransferBuffer(gpu_device, xfer, false);
        memcpy(mapped, ui->font_verts, buf_size);
        SDL_UnmapGPUTransferBuffer(gpu_device, xfer);

        SDL_GPUBufferCreateInfo gbuf_info = {};
        gbuf_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        gbuf_info.size = buf_size;
        ui->font_gpu_buf = SDL_CreateGPUBuffer(gpu_device, &gbuf_info);

        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd_buf);
        SDL_GPUTransferBufferLocation src = {};
        src.transfer_buffer = xfer;
        SDL_GPUBufferRegion dst = {};
        dst.buffer = ui->font_gpu_buf;
        dst.size = buf_size;
        SDL_UploadToGPUBuffer(copy, &src, &dst, false);
        SDL_EndGPUCopyPass(copy);
        SDL_ReleaseGPUTransferBuffer(gpu_device, xfer);
    }
}

// Game window: run Clay layout for game UI overlay, build + upload vertices
static void ui_prepare_game(SDL_GPUCommandBuffer *cmd_buf, game *g) {
    Clay_SetCurrentContext(clay_context);

    int win_w, win_h;
    SDL_GetWindowSizeInPixels(window, &win_w, &win_h);
    Clay_SetLayoutDimensions(Clay_Dimensions{(float)win_w, (float)win_h});

    Clay_BeginLayout();
    /* (game HUD elements will go here later) */
    Clay_RenderCommandArray commands = Clay_EndLayout();

    ui_build_vertices(&ui_game, commands);
    ui_upload(cmd_buf, &ui_game);
}

// ---------------------------------------------------------------------------
// Profiler UI layout helpers
// ---------------------------------------------------------------------------

// Recursive: emit tree rows for an arena and its children
static void profiler_tree_arena(arena *a, int depth, int *row_id) {
    for (uint32_t i = 0; i < a->record_count; i++) {
        arena_record *r = &a->records[i];
        Clay_Color color = profiler_colors[(*row_id) % profiler_color_count];

        // Build size string
        static char size_bufs[256][32];
        int idx = (*row_id) % 256;
        if (r->child) {
            float child_pct = (r->child->capacity > 0)
                ? (100.0f * r->child->used / r->child->capacity) : 0.0f;
            snprintf(size_bufs[idx], sizeof(size_bufs[idx]), "%s (%.0f%%)",
                format_bytes(r->child->capacity), (double)child_pct);
        } else {
            snprintf(size_bufs[idx], sizeof(size_bufs[idx]), "%s", format_bytes(r->size));
        }

        CLAY(CLAY_IDI("TreeRow", (int32_t)*row_id), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW({}), CLAY_SIZING_FIT({}) },
                .padding = { .left = (uint16_t)(4 + depth * 12), .right = 4, .top = 2, .bottom = 2 },
                .childGap = 6,
                .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER},
                .layoutDirection = CLAY_LEFT_TO_RIGHT
            }
        }) {
            // Color swatch
            CLAY(CLAY_IDI("TSwatch", (int32_t)*row_id), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(8), CLAY_SIZING_FIXED(8) } },
                .backgroundColor = color,
                .cornerRadius = CLAY_CORNER_RADIUS(2)
            }) {}

            // Tag name (grows to fill)
            {
                Clay_String tag_s = {false, (int32_t)strlen(r->tag), r->tag};
                CLAY_TEXT(tag_s, CLAY_TEXT_CONFIG({.textColor = {200, 200, 200, 255}, .fontSize = 16}));
            }

            // Size (right side, dimmer)
            {
                Clay_String sz_s = {false, (int32_t)strlen(size_bufs[idx]), size_bufs[idx]};
                CLAY_TEXT(sz_s, CLAY_TEXT_CONFIG({.textColor = {170, 170, 180, 255}, .fontSize = 16}));
            }
        }

        (*row_id)++;

        if (r->child) {
            profiler_tree_arena(r->child, depth + 1, row_id);
        }
    }
}

// Collect all leaf records (flattened) for block + grid views
struct FlatRecord {
    const char *tag;
    uint32_t    offset;  /* absolute offset within root arena */
    uint32_t    size;
    int         color_idx;
};
#define MAX_FLAT_RECORDS 256

static int profiler_flatten_arena(arena *a, uint32_t base_offset,
                                  FlatRecord *out, int count, int *color_id) {
    for (uint32_t i = 0; i < a->record_count && count < MAX_FLAT_RECORDS; i++) {
        arena_record *r = &a->records[i];
        if (r->child && r->child->record_count > 0) {
            // Sub-arena with its own records — recurse into children
            count = profiler_flatten_arena(r->child, base_offset + r->offset + (uint32_t)sizeof(arena),
                                           out, count, color_id);
        } else if (r->child) {
            // Sub-arena with no internal records (e.g. Clay) — show as leaf using its used size
            out[count].tag = r->tag;
            out[count].offset = base_offset + r->offset;
            out[count].size = r->child->used > 0 ? r->child->used : r->size;
            out[count].color_idx = (*color_id)++;
            count++;
        } else {
            out[count].tag = r->tag;
            out[count].offset = base_offset + r->offset;
            out[count].size = r->size;
            out[count].color_idx = (*color_id)++;
            count++;
        }
    }
    return count;
}

// Profiler window: run Clay layout for profiler panels, build + upload vertices
static void profiler_prepare(SDL_GPUCommandBuffer *cmd_buf, game *g) {
    // Sync Clay's peak frame usage into our sub-arenas for profiler display.
    // nextAllocation = current bump offset (peak after EndLayout).
    if (clay_arena_game && clay_context)
        clay_arena_game->used = (uint32_t)clay_context->internalArena.nextAllocation;
    if (clay_arena_prof && profiler_clay_context)
        clay_arena_prof->used = (uint32_t)profiler_clay_context->internalArena.nextAllocation;

    Clay_SetCurrentContext(profiler_clay_context);

    int win_w, win_h;
    SDL_GetWindowSizeInPixels(profiler_window, &win_w, &win_h);
    Clay_SetLayoutDimensions(Clay_Dimensions{(float)win_w, (float)win_h});

    Clay_BeginLayout();

    arena *a = &g->arena;
    float used_pct = (a->capacity > 0) ? (100.0f * a->used / a->capacity) : 0.0f;

    static char title_buf[128];
    snprintf(title_buf, sizeof(title_buf), "Memory Profiler    %s / %s  (%.1f%%)",
        format_bytes(a->used), format_bytes(a->capacity), (double)used_pct);

    // Flatten records for block+grid
    static FlatRecord flat[MAX_FLAT_RECORDS];
    int flat_color = 0;
    int flat_count = profiler_flatten_arena(a, 0, flat, 0, &flat_color);

    // Root container — fills entire window
    CLAY(CLAY_ID("PRoot"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW({}), CLAY_SIZING_GROW({}) },
            .padding = CLAY_PADDING_ALL(12),
            .childGap = 0,
            .layoutDirection = CLAY_TOP_TO_BOTTOM
        },
        .backgroundColor = {25, 25, 30, 255}
    }) {
        // Title bar
        CLAY(CLAY_ID("PTitleBar"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW({}), CLAY_SIZING_FIT({}) },
                .padding = { .left = 8, .right = 8, .top = 6, .bottom = 10 }
            }
        }) {
            Clay_String ts = {false, (int32_t)strlen(title_buf), title_buf};
            CLAY_TEXT(ts, CLAY_TEXT_CONFIG({.textColor = {220, 220, 220, 255}, .fontSize = 16}));
        }

        // Three-panel row
        CLAY(CLAY_ID("PPanels"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW({}), CLAY_SIZING_GROW({}) },
                .childGap = 8,
                .layoutDirection = CLAY_LEFT_TO_RIGHT
            }
        }) {
            // ===== TREE PANEL (left, 35%) =====
            CLAY(CLAY_ID("PTree"), {
                .layout = {
                    .sizing = { CLAY_SIZING_PERCENT(0.35f), CLAY_SIZING_GROW({}) },
                    .padding = CLAY_PADDING_ALL(8),
                    .childGap = 2,
                    .layoutDirection = CLAY_TOP_TO_BOTTOM
                },
                .backgroundColor = {35, 35, 40, 255},
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                {
                    Clay_String hdr = CLAY_STRING("Tree View");
                    CLAY_TEXT(hdr, CLAY_TEXT_CONFIG({.textColor = {180, 180, 190, 255}, .fontSize = 16}));
                }

                // Arena root row
                {
                    static char root_buf[64];
                    snprintf(root_buf, sizeof(root_buf), "Arena  %s", format_bytes(a->capacity));
                    Clay_String rs = {false, (int32_t)strlen(root_buf), root_buf};

                    CLAY(CLAY_ID("TRootRow"), {
                        .layout = {
                            .sizing = { CLAY_SIZING_GROW({}), CLAY_SIZING_FIT({}) },
                            .padding = { .left = 8, .right = 8, .top = 4, .bottom = 4 }
                        }
                    }) {
                        CLAY_TEXT(rs, CLAY_TEXT_CONFIG({.textColor = {220, 220, 220, 255}, .fontSize = 16}));
                    }
                }

                int row_id = 0;
                profiler_tree_arena(a, 1, &row_id);
            }

            // ===== BLOCK PANEL (center, 20%) =====
            CLAY(CLAY_ID("PBlock"), {
                .layout = {
                    .sizing = { CLAY_SIZING_PERCENT(0.20f), CLAY_SIZING_GROW({}) },
                    .padding = CLAY_PADDING_ALL(8),
                    .childGap = 2,
                    .layoutDirection = CLAY_TOP_TO_BOTTOM
                },
                .backgroundColor = {35, 35, 40, 255},
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                {
                    Clay_String hdr = CLAY_STRING("Block View");
                    CLAY_TEXT(hdr, CLAY_TEXT_CONFIG({.textColor = {180, 180, 190, 255}, .fontSize = 16}));
                }

                // Column of proportional blocks
                float block_total_h = (float)(win_h - 80);  // approximate available height
                if (block_total_h < 100) block_total_h = 100;

                for (int i = 0; i < flat_count; i++) {
                    float frac = (float)flat[i].size / (float)a->capacity;
                    float h = frac * block_total_h;
                    if (h < 2.0f) h = 2.0f;
                    Clay_Color color = profiler_colors[flat[i].color_idx % profiler_color_count];

                    static char block_bufs[MAX_FLAT_RECORDS][48];
                    snprintf(block_bufs[i], sizeof(block_bufs[i]), "%s", flat[i].tag);

                    CLAY(CLAY_IDI("BlkEntry", (int32_t)i), {
                        .layout = {
                            .sizing = { CLAY_SIZING_GROW({}), CLAY_SIZING_FIXED(h) },
                            .padding = { .left = 4, .right = 4, .top = 1, .bottom = 1 },
                            .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}
                        },
                        .backgroundColor = color,
                        .cornerRadius = CLAY_CORNER_RADIUS(2)
                    }) {
                        if (h > 14) {
                            Clay_String bs = {false, (int32_t)strlen(block_bufs[i]), block_bufs[i]};
                            CLAY_TEXT(bs, CLAY_TEXT_CONFIG({.textColor = {0, 0, 0, 200}, .fontSize = 16}));
                        }
                    }
                }

                // Free space block
                {
                    uint32_t free_bytes = a->capacity - a->used;
                    float frac = (float)free_bytes / (float)a->capacity;
                    float h = frac * block_total_h;
                    if (h < 2.0f) h = 2.0f;

                    static char free_buf[32];
                    snprintf(free_buf, sizeof(free_buf), "FREE %s", format_bytes(free_bytes));

                    CLAY(CLAY_ID("BlkFree"), {
                        .layout = {
                            .sizing = { CLAY_SIZING_GROW({}), CLAY_SIZING_FIXED(h) },
                            .padding = { .left = 4, .right = 4, .top = 1, .bottom = 1 },
                            .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}
                        },
                        .backgroundColor = {50, 50, 55, 255},
                        .cornerRadius = CLAY_CORNER_RADIUS(2)
                    }) {
                        if (h > 14) {
                            Clay_String fs = {false, (int32_t)strlen(free_buf), free_buf};
                            CLAY_TEXT(fs, CLAY_TEXT_CONFIG({.textColor = {120, 120, 120, 255}, .fontSize = 16}));
                        }
                    }
                }
            }

            // ===== GRID PANEL (right) =====
            CLAY(CLAY_ID("PGrid"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW({}), CLAY_SIZING_GROW({}) },
                    .padding = CLAY_PADDING_ALL(8),
                    .childGap = 2,
                    .layoutDirection = CLAY_TOP_TO_BOTTOM
                },
                .backgroundColor = {35, 35, 40, 255},
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                {
                    Clay_String hdr = CLAY_STRING("Grid View");
                    CLAY_TEXT(hdr, CLAY_TEXT_CONFIG({.textColor = {180, 180, 190, 255}, .fontSize = 16}));
                }

                // Grid: each cell = 64KB of arena space
                uint32_t cell_size = 64 * 1024;  // 64 KB per cell
                uint32_t total_cells = (a->capacity + cell_size - 1) / cell_size;
                int grid_cols = 16;
                int grid_rows = ((int)total_cells + grid_cols - 1) / grid_cols;

                for (int row = 0; row < grid_rows; row++) {
                    CLAY(CLAY_IDI("GRow", (int32_t)row), {
                        .layout = {
                            .sizing = { CLAY_SIZING_GROW({}), CLAY_SIZING_FIT({}) },
                            .childGap = 2,
                            .layoutDirection = CLAY_LEFT_TO_RIGHT
                        }
                    }) {
                        for (int col = 0; col < grid_cols; col++) {
                            uint32_t cell_idx = (uint32_t)(row * grid_cols + col);
                            if (cell_idx >= total_cells) break;

                            uint32_t cell_start = cell_idx * cell_size;
                            uint32_t cell_end = cell_start + cell_size;
                            if (cell_end > a->capacity) cell_end = a->capacity;

                            // Find which record owns this cell
                            Clay_Color cell_color = {50, 50, 55, 255};  // free/dark
                            for (int fi = 0; fi < flat_count; fi++) {
                                uint32_t rec_start = flat[fi].offset;
                                uint32_t rec_end = flat[fi].offset + flat[fi].size;
                                if (rec_start < cell_end && rec_end > cell_start) {
                                    cell_color = profiler_colors[flat[fi].color_idx % profiler_color_count];
                                    break;
                                }
                            }

                            int32_t cell_id = (int32_t)(row * grid_cols + col);
                            CLAY(CLAY_IDI("GCell", cell_id), {
                                .layout = {
                                    .sizing = { CLAY_SIZING_FIXED(18), CLAY_SIZING_FIXED(18) }
                                },
                                .backgroundColor = cell_color,
                                .cornerRadius = CLAY_CORNER_RADIUS(2)
                            }) {}
                        }
                    }
                }

                // Legend
                CLAY(CLAY_ID("GLegend"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW({}), CLAY_SIZING_FIT({}) },
                        .padding = { .left = 0, .right = 0, .top = 8, .bottom = 0 },
                        .childGap = 12,
                        .layoutDirection = CLAY_LEFT_TO_RIGHT
                    }
                }) {
                    CLAY(CLAY_ID("GLAllocBox"), {
                        .layout = { .sizing = { CLAY_SIZING_FIXED(10), CLAY_SIZING_FIXED(10) } },
                        .backgroundColor = profiler_colors[0],
                        .cornerRadius = CLAY_CORNER_RADIUS(1)
                    }) {}
                    {
                        Clay_String ls = CLAY_STRING("= allocated");
                        CLAY_TEXT(ls, CLAY_TEXT_CONFIG({.textColor = {170, 170, 170, 255}, .fontSize = 16}));
                    }
                    CLAY(CLAY_ID("GLFreeBox"), {
                        .layout = { .sizing = { CLAY_SIZING_FIXED(10), CLAY_SIZING_FIXED(10) } },
                        .backgroundColor = {50, 50, 55, 255},
                        .cornerRadius = CLAY_CORNER_RADIUS(1)
                    }) {}
                    {
                        Clay_String ls = CLAY_STRING("= free");
                        CLAY_TEXT(ls, CLAY_TEXT_CONFIG({.textColor = {170, 170, 170, 255}, .fontSize = 16}));
                    }
                }
            }
        }
    }

    Clay_RenderCommandArray commands = Clay_EndLayout();
    ui_build_vertices(&ui_profiler, commands);
    ui_upload(cmd_buf, &ui_profiler);
}

// Draw UI during render pass (works for any UIRenderState)
static void ui_draw(SDL_GPURenderPass *render_pass, SDL_GPUCommandBuffer *cmd_buf,
                    uniform_data *uniforms, UIRenderState *ui) {
    if (ui->rect_vert_count > 0 && ui->rect_gpu_buf) {
        SDL_BindGPUGraphicsPipeline(render_pass, ui_rect_pipeline);
        SDL_PushGPUVertexUniformData(cmd_buf, 0, uniforms, sizeof(*uniforms));

        SDL_GPUBufferBinding vbuf_binding = {};
        vbuf_binding.buffer = ui->rect_gpu_buf;
        SDL_BindGPUVertexBuffers(render_pass, 0, &vbuf_binding, 1);
        SDL_DrawGPUPrimitives(render_pass, (Uint32)ui->rect_vert_count, 1, 0, 0);
    }

    if (ui->font_vert_count > 0 && ui->font_gpu_buf) {
        SDL_BindGPUGraphicsPipeline(render_pass, font_pipeline);
        SDL_PushGPUVertexUniformData(cmd_buf, 0, uniforms, sizeof(*uniforms));

        SDL_GPUBuffer *storage_bufs[4] = {
            font_curve_buffer, font_glyph_buffer,
            font_grid_cell_buffer, font_grid_index_buffer
        };
        SDL_BindGPUFragmentStorageBuffers(render_pass, 0, storage_bufs, 4);

        SDL_GPUBufferBinding vbuf_binding = {};
        vbuf_binding.buffer = ui->font_gpu_buf;
        SDL_BindGPUVertexBuffers(render_pass, 0, &vbuf_binding, 1);
        SDL_DrawGPUPrimitives(render_pass, (Uint32)ui->font_vert_count, 1, 0, 0);
    }
}

// Cleanup per-frame UI GPU buffers for one window
static void ui_release_buffers(UIRenderState *ui) {
    if (ui->rect_gpu_buf) { SDL_ReleaseGPUBuffer(gpu_device, ui->rect_gpu_buf); ui->rect_gpu_buf = NULL; }
    if (ui->font_gpu_buf) { SDL_ReleaseGPUBuffer(gpu_device, ui->font_gpu_buf); ui->font_gpu_buf = NULL; }
}

// ---------------------------------------------------------------------------
// init_externals
// ---------------------------------------------------------------------------

EXPORT int init_externals(game *g) {
    ZoneScopedN("init_externals");

    // 1. Init SDL
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    // 2. Init shadercross BEFORE creating GPU device (needs DXC/SPIRV-Cross loaded
    //    so GetSPIRVShaderFormats returns correct format flags)
    if (!SDL_ShaderCross_Init()) {
        fprintf(stderr, "SDL_ShaderCross_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    // 3. Create window
    window = SDL_CreateWindow("Anitra", 800, 600, SDL_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return -1;
    }

    // 4. Create GPU device
    gpu_device = SDL_CreateGPUDevice(
        SDL_ShaderCross_GetSPIRVShaderFormats(),
        true,  // debug_mode
        NULL);
    if (!gpu_device) {
        fprintf(stderr, "SDL_CreateGPUDevice failed: %s\n", SDL_GetError());
        return -1;
    }

    // 5. Claim window
    if (!SDL_ClaimWindowForGPUDevice(gpu_device, window)) {
        fprintf(stderr, "SDL_ClaimWindowForGPUDevice failed: %s\n", SDL_GetError());
        return -1;
    }

    // 6. Compile sprite shaders (split files to avoid DXC including unused resources)
    SDL_GPUShader* sprite_vs = load_shader_from_spirv(
        "assets/shaders/compiled/sprite_vs.spv", "main", SDL_SHADERCROSS_SHADERSTAGE_VERTEX);
    SDL_GPUShader* sprite_fs = load_shader_from_spirv(
        "assets/shaders/compiled/sprite_fs.spv", "main", SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT);
    if (!sprite_vs || !sprite_fs) {
        fprintf(stderr, "Failed to compile sprite shaders\n");
        return -1;
    }

    // 7. Create sprite pipeline
    {
        SDL_GPUVertexBufferDescription vbuf_desc = {};
        vbuf_desc.slot = 0;
        vbuf_desc.pitch = sizeof(sprite_vertex);
        vbuf_desc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        vbuf_desc.instance_step_rate = 0;

        SDL_GPUVertexAttribute attrs[3] = {};
        // location 0: position (float2)
        attrs[0].location = 0;
        attrs[0].buffer_slot = 0;
        attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        attrs[0].offset = 0;
        // location 1: texcoord (float2)
        attrs[1].location = 1;
        attrs[1].buffer_slot = 0;
        attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        attrs[1].offset = sizeof(float) * 2;
        // location 2: tint color (float4)
        attrs[2].location = 2;
        attrs[2].buffer_slot = 0;
        attrs[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        attrs[2].offset = sizeof(float) * 4;

        SDL_GPUColorTargetBlendState blend = {};
        blend.enable_blend = true;
        blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        blend.color_blend_op = SDL_GPU_BLENDOP_ADD;
        blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        blend.enable_color_write_mask = false;

        SDL_GPUTextureFormat swapchain_format =
            SDL_GetGPUSwapchainTextureFormat(gpu_device, window);

        SDL_GPUColorTargetDescription color_target = {};
        color_target.format = swapchain_format;
        color_target.blend_state = blend;

        SDL_GPUGraphicsPipelineCreateInfo pipe_info = {};
        pipe_info.vertex_shader = sprite_vs;
        pipe_info.fragment_shader = sprite_fs;

        pipe_info.vertex_input_state.vertex_buffer_descriptions = &vbuf_desc;
        pipe_info.vertex_input_state.num_vertex_buffers = 1;
        pipe_info.vertex_input_state.vertex_attributes = attrs;
        pipe_info.vertex_input_state.num_vertex_attributes = 3;

        pipe_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

        pipe_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        pipe_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        pipe_info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;

        pipe_info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        pipe_info.multisample_state.sample_mask = 0;
        pipe_info.multisample_state.enable_mask = false;

        pipe_info.target_info.color_target_descriptions = &color_target;
        pipe_info.target_info.num_color_targets = 1;
        pipe_info.target_info.has_depth_stencil_target = true;
        pipe_info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

        pipe_info.props = 0;

        sprite_pipeline = SDL_CreateGPUGraphicsPipeline(gpu_device, &pipe_info);
        if (!sprite_pipeline) {
            fprintf(stderr, "Failed to create sprite pipeline: %s\n", SDL_GetError());
            return -1;
        }
    }

    // Release sprite shaders (pipeline keeps internal reference)
    SDL_ReleaseGPUShader(gpu_device, sprite_vs);
    SDL_ReleaseGPUShader(gpu_device, sprite_fs);

    // 8. Compile debug line shaders (split files)
    SDL_GPUShader* line_vs = load_shader_from_spirv(
        "assets/shaders/compiled/debug_lines_vs.spv", "main", SDL_SHADERCROSS_SHADERSTAGE_VERTEX);
    SDL_GPUShader* line_fs = load_shader_from_spirv(
        "assets/shaders/compiled/debug_lines_fs.spv", "main", SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT);
    if (!line_vs || !line_fs) {
        fprintf(stderr, "Failed to compile debug line shaders\n");
        return -1;
    }

    // 9. Create line pipeline
    {
        SDL_GPUVertexBufferDescription vbuf_desc = {};
        vbuf_desc.slot = 0;
        vbuf_desc.pitch = sizeof(line_vertex);
        vbuf_desc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        vbuf_desc.instance_step_rate = 0;

        SDL_GPUVertexAttribute attrs[2] = {};
        // location 0: position (float2)
        attrs[0].location = 0;
        attrs[0].buffer_slot = 0;
        attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        attrs[0].offset = 0;
        // location 1: color (float3)
        attrs[1].location = 1;
        attrs[1].buffer_slot = 0;
        attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
        attrs[1].offset = sizeof(float) * 2;

        SDL_GPUTextureFormat swapchain_format =
            SDL_GetGPUSwapchainTextureFormat(gpu_device, window);

        SDL_GPUColorTargetDescription color_target = {};
        color_target.format = swapchain_format;
        // No blending needed for debug lines
        color_target.blend_state = {};

        SDL_GPUGraphicsPipelineCreateInfo pipe_info = {};
        pipe_info.vertex_shader = line_vs;
        pipe_info.fragment_shader = line_fs;

        pipe_info.vertex_input_state.vertex_buffer_descriptions = &vbuf_desc;
        pipe_info.vertex_input_state.num_vertex_buffers = 1;
        pipe_info.vertex_input_state.vertex_attributes = attrs;
        pipe_info.vertex_input_state.num_vertex_attributes = 2;

        pipe_info.primitive_type = SDL_GPU_PRIMITIVETYPE_LINELIST;

        pipe_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        pipe_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        pipe_info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;

        pipe_info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        pipe_info.multisample_state.sample_mask = 0;
        pipe_info.multisample_state.enable_mask = false;

        pipe_info.target_info.color_target_descriptions = &color_target;
        pipe_info.target_info.num_color_targets = 1;
        pipe_info.target_info.has_depth_stencil_target = true;
        pipe_info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

        pipe_info.props = 0;

        line_pipeline = SDL_CreateGPUGraphicsPipeline(gpu_device, &pipe_info);
        if (!line_pipeline) {
            fprintf(stderr, "Failed to create line pipeline: %s\n", SDL_GetError());
            return -1;
        }
    }

    SDL_ReleaseGPUShader(gpu_device, line_vs);
    SDL_ReleaseGPUShader(gpu_device, line_fs);

    // 10. Compile and create UI rect pipeline
    {
        SDL_GPUShader *ui_vs = load_shader_from_spirv(
            "assets/shaders/compiled/ui_rect_vs.spv", "main", SDL_SHADERCROSS_SHADERSTAGE_VERTEX);
        SDL_GPUShader *ui_fs = load_shader_from_spirv(
            "assets/shaders/compiled/ui_rect_fs.spv", "main", SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT);
        if (!ui_vs || !ui_fs) {
            fprintf(stderr, "Failed to compile UI rect shaders\n");
            return -1;
        }

        SDL_GPUVertexBufferDescription vbuf_desc = {};
        vbuf_desc.slot = 0;
        vbuf_desc.pitch = sizeof(ui_rect_vertex);
        vbuf_desc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

        SDL_GPUVertexAttribute attrs[2] = {};
        attrs[0].location = 0;
        attrs[0].buffer_slot = 0;
        attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        attrs[0].offset = 0;
        attrs[1].location = 1;
        attrs[1].buffer_slot = 0;
        attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        attrs[1].offset = sizeof(float) * 2;

        SDL_GPUColorTargetBlendState blend = {};
        blend.enable_blend = true;
        blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        blend.color_blend_op = SDL_GPU_BLENDOP_ADD;
        blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

        SDL_GPUTextureFormat swapchain_format =
            SDL_GetGPUSwapchainTextureFormat(gpu_device, window);

        SDL_GPUColorTargetDescription color_target = {};
        color_target.format = swapchain_format;
        color_target.blend_state = blend;

        SDL_GPUGraphicsPipelineCreateInfo pipe_info = {};
        pipe_info.vertex_shader = ui_vs;
        pipe_info.fragment_shader = ui_fs;
        pipe_info.vertex_input_state.vertex_buffer_descriptions = &vbuf_desc;
        pipe_info.vertex_input_state.num_vertex_buffers = 1;
        pipe_info.vertex_input_state.vertex_attributes = attrs;
        pipe_info.vertex_input_state.num_vertex_attributes = 2;
        pipe_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipe_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        pipe_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        pipe_info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipe_info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        pipe_info.target_info.color_target_descriptions = &color_target;
        pipe_info.target_info.num_color_targets = 1;
        pipe_info.target_info.has_depth_stencil_target = true;
        pipe_info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

        ui_rect_pipeline = SDL_CreateGPUGraphicsPipeline(gpu_device, &pipe_info);
        if (!ui_rect_pipeline) {
            fprintf(stderr, "Failed to create UI rect pipeline: %s\n", SDL_GetError());
            return -1;
        }

        SDL_ReleaseGPUShader(gpu_device, ui_vs);
        SDL_ReleaseGPUShader(gpu_device, ui_fs);
    }

    // 11. Compile and create font pipeline
    {
        SDL_GPUShader *font_vs = load_shader_from_spirv(
            "assets/shaders/compiled/font_vs.spv", "main", SDL_SHADERCROSS_SHADERSTAGE_VERTEX);
        SDL_GPUShader *font_fs = load_shader_from_spirv(
            "assets/shaders/compiled/font_fs.spv", "main", SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT);
        if (!font_vs || !font_fs) {
            fprintf(stderr, "Failed to compile font shaders\n");
            return -1;
        }

        SDL_GPUVertexBufferDescription vbuf_desc = {};
        vbuf_desc.slot = 0;
        vbuf_desc.pitch = sizeof(font_vertex);
        vbuf_desc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

        SDL_GPUVertexAttribute attrs[4] = {};
        // position (float2)
        attrs[0].location = 0;
        attrs[0].buffer_slot = 0;
        attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        attrs[0].offset = 0;
        // uv (float2)
        attrs[1].location = 1;
        attrs[1].buffer_slot = 0;
        attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        attrs[1].offset = sizeof(float) * 2;
        // color (float4)
        attrs[2].location = 2;
        attrs[2].buffer_slot = 0;
        attrs[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        attrs[2].offset = sizeof(float) * 4;
        // glyph_id (uint)
        attrs[3].location = 3;
        attrs[3].buffer_slot = 0;
        attrs[3].format = SDL_GPU_VERTEXELEMENTFORMAT_UINT;
        attrs[3].offset = sizeof(float) * 8;

        SDL_GPUColorTargetBlendState blend = {};
        blend.enable_blend = true;
        blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        blend.color_blend_op = SDL_GPU_BLENDOP_ADD;
        blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

        SDL_GPUTextureFormat swapchain_format =
            SDL_GetGPUSwapchainTextureFormat(gpu_device, window);

        SDL_GPUColorTargetDescription color_target = {};
        color_target.format = swapchain_format;
        color_target.blend_state = blend;

        SDL_GPUGraphicsPipelineCreateInfo pipe_info = {};
        pipe_info.vertex_shader = font_vs;
        pipe_info.fragment_shader = font_fs;
        pipe_info.vertex_input_state.vertex_buffer_descriptions = &vbuf_desc;
        pipe_info.vertex_input_state.num_vertex_buffers = 1;
        pipe_info.vertex_input_state.vertex_attributes = attrs;
        pipe_info.vertex_input_state.num_vertex_attributes = 4;
        pipe_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipe_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        pipe_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        pipe_info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipe_info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        pipe_info.target_info.color_target_descriptions = &color_target;
        pipe_info.target_info.num_color_targets = 1;
        pipe_info.target_info.has_depth_stencil_target = true;
        pipe_info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

        font_pipeline = SDL_CreateGPUGraphicsPipeline(gpu_device, &pipe_info);
        if (!font_pipeline) {
            fprintf(stderr, "Failed to create font pipeline: %s\n", SDL_GetError());
            return -1;
        }

        SDL_ReleaseGPUShader(gpu_device, font_vs);
        SDL_ReleaseGPUShader(gpu_device, font_fs);
    }

    // 12. Compile and create mesh (3D skinned) pipeline
    {
        SDL_GPUShader *mesh_vs = load_shader_from_spirv(
            "assets/shaders/compiled/mesh_vs.spv", "main", SDL_SHADERCROSS_SHADERSTAGE_VERTEX);
        SDL_GPUShader *mesh_fs = load_shader_from_spirv(
            "assets/shaders/compiled/mesh_fs.spv", "main", SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT);
        if (!mesh_vs || !mesh_fs) {
            fprintf(stderr, "Failed to compile mesh shaders\n");
            return -1;
        }

        SDL_GPUVertexBufferDescription vbuf_desc = {};
        vbuf_desc.slot = 0;
        vbuf_desc.pitch = sizeof(SkinnedVertex);
        vbuf_desc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

        SDL_GPUVertexAttribute attrs[5] = {};
        // location 0: position (float3)
        attrs[0].location = 0;
        attrs[0].buffer_slot = 0;
        attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
        attrs[0].offset = offsetof(SkinnedVertex, position);
        // location 1: normal (float3)
        attrs[1].location = 1;
        attrs[1].buffer_slot = 0;
        attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
        attrs[1].offset = offsetof(SkinnedVertex, normal);
        // location 2: uv (float2)
        attrs[2].location = 2;
        attrs[2].buffer_slot = 0;
        attrs[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        attrs[2].offset = offsetof(SkinnedVertex, uv);
        // location 3: bone_ids (ubyte4)
        attrs[3].location = 3;
        attrs[3].buffer_slot = 0;
        attrs[3].format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4;
        attrs[3].offset = offsetof(SkinnedVertex, bone_ids);
        // location 4: bone_weights (float4)
        attrs[4].location = 4;
        attrs[4].buffer_slot = 0;
        attrs[4].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        attrs[4].offset = offsetof(SkinnedVertex, bone_weights);

        SDL_GPUColorTargetBlendState blend = {};
        blend.enable_blend = false;

        SDL_GPUTextureFormat swapchain_format =
            SDL_GetGPUSwapchainTextureFormat(gpu_device, window);

        SDL_GPUColorTargetDescription color_target = {};
        color_target.format = swapchain_format;
        color_target.blend_state = blend;

        SDL_GPUGraphicsPipelineCreateInfo pipe_info = {};
        pipe_info.vertex_shader = mesh_vs;
        pipe_info.fragment_shader = mesh_fs;
        pipe_info.vertex_input_state.vertex_buffer_descriptions = &vbuf_desc;
        pipe_info.vertex_input_state.num_vertex_buffers = 1;
        pipe_info.vertex_input_state.vertex_attributes = attrs;
        pipe_info.vertex_input_state.num_vertex_attributes = 5;
        pipe_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipe_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        pipe_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
        pipe_info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipe_info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;

        pipe_info.depth_stencil_state.enable_depth_test = true;
        pipe_info.depth_stencil_state.enable_depth_write = true;
        pipe_info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;

        pipe_info.target_info.color_target_descriptions = &color_target;
        pipe_info.target_info.num_color_targets = 1;
        pipe_info.target_info.has_depth_stencil_target = true;
        pipe_info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

        mesh_pipeline = SDL_CreateGPUGraphicsPipeline(gpu_device, &pipe_info);
        if (!mesh_pipeline) {
            fprintf(stderr, "Failed to create mesh pipeline: %s\n", SDL_GetError());
            return -1;
        }

        SDL_ReleaseGPUShader(gpu_device, mesh_vs);
        SDL_ReleaseGPUShader(gpu_device, mesh_fs);
    }

    // 13. Create mesh sampler (linear filtering for 3D textures)
    {
        SDL_GPUSamplerCreateInfo samp_info = {};
        samp_info.min_filter = SDL_GPU_FILTER_LINEAR;
        samp_info.mag_filter = SDL_GPU_FILTER_LINEAR;
        samp_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        samp_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        samp_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        samp_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;

        mesh_sampler = SDL_CreateGPUSampler(gpu_device, &samp_info);
        if (!mesh_sampler) {
            fprintf(stderr, "Failed to create mesh sampler: %s\n", SDL_GetError());
            return -1;
        }
    }

    // 14. Create white 1x1 fallback texture (for untextured meshes)
    {
        SDL_GPUTextureCreateInfo tex_info = {};
        tex_info.type = SDL_GPU_TEXTURETYPE_2D;
        tex_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        tex_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        tex_info.width = 1;
        tex_info.height = 1;
        tex_info.layer_count_or_depth = 1;
        tex_info.num_levels = 1;
        tex_info.sample_count = SDL_GPU_SAMPLECOUNT_1;

        white_texture = SDL_CreateGPUTexture(gpu_device, &tex_info);

        uint32_t white_pixel = 0xFFFFFFFF;
        SDL_GPUTransferBufferCreateInfo tbuf_info = {};
        tbuf_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbuf_info.size = 4;
        SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(gpu_device, &tbuf_info);
        void *map = SDL_MapGPUTransferBuffer(gpu_device, transfer, false);
        memcpy(map, &white_pixel, 4);
        SDL_UnmapGPUTransferBuffer(gpu_device, transfer);

        SDL_GPUCommandBuffer *upload_cmd = SDL_AcquireGPUCommandBuffer(gpu_device);
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(upload_cmd);
        SDL_GPUTextureTransferInfo src = {};
        src.transfer_buffer = transfer;
        SDL_GPUTextureRegion dst = {};
        dst.texture = white_texture;
        dst.w = 1;
        dst.h = 1;
        dst.d = 1;
        SDL_UploadToGPUTexture(copy, &src, &dst, false);
        SDL_EndGPUCopyPass(copy);
        SDL_SubmitGPUCommandBuffer(upload_cmd);
        SDL_ReleaseGPUTransferBuffer(gpu_device, transfer);
    }

    // 15. Create bone storage buffer
    {
        SDL_GPUBufferCreateInfo buf_info = {};
        buf_info.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
        buf_info.size = MAX_BONES * sizeof(Mat4);
        bone_storage_buffer = SDL_CreateGPUBuffer(gpu_device, &buf_info);
        if (!bone_storage_buffer) {
            fprintf(stderr, "Failed to create bone storage buffer: %s\n", SDL_GetError());
            return -1;
        }
    }

    // 16. Create sampler (nearest-neighbor for pixel art)
    {
        SDL_GPUSamplerCreateInfo samp_info = {};
        samp_info.min_filter = SDL_GPU_FILTER_NEAREST;
        samp_info.mag_filter = SDL_GPU_FILTER_NEAREST;
        samp_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        samp_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        samp_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        samp_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        samp_info.props = 0;

        sprite_sampler = SDL_CreateGPUSampler(gpu_device, &samp_info);
        if (!sprite_sampler) {
            fprintf(stderr, "Failed to create sampler: %s\n", SDL_GetError());
            return -1;
        }
    }

    // 11. Load textures
    gpu_textures[TEXTURE_PLAYER]      = load_gpu_texture("assets/char_spritesheet.png");
    gpu_textures[TEXTURE_TILES]       = load_gpu_texture("assets/Dungeon_Tileset.png");
    gpu_textures[TEXTURE_SLIME]       = load_gpu_texture("assets/pinkslime_spritesheet.png");
    gpu_textures[TEXTURE_HEALTH_BAR]  = load_gpu_texture("assets/health_bar_hud.png");
    gpu_textures[TEXTURE_HEALTH_FILL] = load_gpu_texture("assets/health_hud.png");

    // Load editor font and upload to GPU
    if (font_load(&editor_font, "assets/fonts/SourceCodePro-Regular.ttf") != 0) {
        fprintf(stderr, "Failed to load editor font\n");
        return -1;
    }
    if (font_upload_to_gpu(&editor_font) != 0) {
        fprintf(stderr, "Failed to upload font to GPU\n");
        return -1;
    }
    if (harfbuzz_init(&editor_font) != 0) {
        fprintf(stderr, "Failed to init HarfBuzz\n");
        return -1;
    }

    // Initialize arena (single allocation for all engine memory)
    {
        uint32_t arena_size = 16 * 1024 * 1024; // 16 MB
        void *arena_mem = malloc(arena_size);
        arena_init(&g->arena, arena_mem, arena_size);
        printf("Arena initialized (%u bytes)\n", arena_size);
    }

    // Initialize Clay UI — game window (sub-arena so profiler can track usage)
    {
        uint64_t clay_mem_size = Clay_MinMemorySize();
        clay_arena_game = arena_alloc_subarena(&g->arena, (uint32_t)clay_mem_size, 16, "clay_ui");

        Clay_Arena clay_arena = Clay_CreateArenaWithCapacityAndMemory(clay_mem_size, clay_arena_game->base);

        int window_w, window_h;
        SDL_GetWindowSize(window, &window_w, &window_h);

        Clay_ErrorHandler err_handler = {};
        clay_context = Clay_Initialize(clay_arena, Clay_Dimensions{(float)window_w, (float)window_h}, err_handler);
        Clay_SetMeasureTextFunction(clay_measure_text, NULL);
        printf("Clay game context initialized (%llu bytes from arena)\n", (unsigned long long)clay_mem_size);
    }

    // Initialize Clay UI — profiler window (sub-arena so profiler can track usage)
    {
        uint64_t clay_mem_size = Clay_MinMemorySize();
        clay_arena_prof = arena_alloc_subarena(&g->arena, (uint32_t)clay_mem_size, 16, "clay_profiler");

        Clay_Arena clay_arena = Clay_CreateArenaWithCapacityAndMemory(clay_mem_size, clay_arena_prof->base);
        Clay_ErrorHandler err_handler = {};
        profiler_clay_context = Clay_Initialize(clay_arena, Clay_Dimensions{800, 600}, err_handler);
        Clay_SetCurrentContext(profiler_clay_context);
        Clay_SetMeasureTextFunction(clay_measure_text, NULL);
        Clay_SetCurrentContext(clay_context);  // restore game context
        printf("Clay profiler context initialized (%llu bytes from arena)\n", (unsigned long long)clay_mem_size);
    }

    // Create profiler window
    profiler_window = SDL_CreateWindow("Memory Profiler", 900, 600, SDL_WINDOW_RESIZABLE);
    if (!profiler_window) {
        fprintf(stderr, "Failed to create profiler window: %s\n", SDL_GetError());
        return -1;
    }
    if (!SDL_ClaimWindowForGPUDevice(gpu_device, profiler_window)) {
        fprintf(stderr, "Failed to claim profiler window: %s\n", SDL_GetError());
        return -1;
    }
    profiler_open = true;

    // Allocate rendering sub-arena (draw_commands, debug_lines, debug_vertices)
    {
        arena *rendering = arena_alloc_subarena(&g->arena, 256 * 1024, 16, "rendering");

        g->draw_list.sprite_capacity = MAX_DRAW_COMMANDS;
        g->draw_list.sprites = (draw_command*)arena_alloc(rendering,
            (uint32_t)(MAX_DRAW_COMMANDS * sizeof(draw_command)), 16, "draw_commands");
        g->draw_list.line_capacity = MAX_DEBUG_LINES;
        g->draw_list.lines = (debug_line_command*)arena_alloc(rendering,
            (uint32_t)(MAX_DEBUG_LINES * sizeof(debug_line_command)), 16, "debug_lines");

        g->debug_renderer.max_lines = 1000;
        g->debug_renderer.vertex_buffer = (float*)arena_alloc(rendering,
            (uint32_t)(g->debug_renderer.max_lines * 10 * sizeof(float)), 16, "debug_vertices");
        g->debug_renderer.current_line_count = 0;
    }

    // Allocate gameplay sub-arena (entities, etc.)
    g->gameplay = arena_alloc_subarena(&g->arena, 256 * 1024, 16, "gameplay");

    // Create initial depth texture (matching window size)
    {
        int win_w, win_h;
        SDL_GetWindowSizeInPixels(window, &win_w, &win_h);
        depth_w = (Uint32)win_w;
        depth_h = (Uint32)win_h;

        SDL_GPUTextureCreateInfo depth_info = {};
        depth_info.type = SDL_GPU_TEXTURETYPE_2D;
        depth_info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        depth_info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        depth_info.width = depth_w;
        depth_info.height = depth_h;
        depth_info.layer_count_or_depth = 1;
        depth_info.num_levels = 1;
        depth_info.sample_count = SDL_GPU_SAMPLECOUNT_1;

        depth_texture = SDL_CreateGPUTexture(gpu_device, &depth_info);
        if (!depth_texture) {
            fprintf(stderr, "Failed to create depth texture: %s\n", SDL_GetError());
            return -1;
        }
        printf("Depth texture created (%u x %u)\n", depth_w, depth_h);
    }

    // Load glTF model (Knight) — populate g->mesh3d for engine to animate
    {
        arena *model_arena = arena_alloc_subarena(&g->arena, 2 * 1024 * 1024, 16, "gltf_models");
        if (model_arena) {
            loaded_model = load_glb(
                "C:/Users/andres/Downloads/KayKit_Adventurers_2.0_FREE/Characters/gltf/Knight.glb",
                model_arena);

            if (loaded_model.mesh.primitive_count > 0) {
                // Try loading animations from separate file
                if (loaded_model.clip_count == 0) {
                    load_animations_glb(
                        "C:/Users/andres/Downloads/KayKit_Adventurers_2.0_FREE/Animations/gltf/Rig_Medium/Rig_Medium_General.glb",
                        &loaded_model, model_arena);
                }

                // Populate g->mesh3d so engine.c can drive animation
                uint32_t jc = loaded_model.skeleton.joint_count;
                g->mesh3d.skeleton        = loaded_model.skeleton;
                g->mesh3d.clips           = loaded_model.clips;
                g->mesh3d.clip_count      = loaded_model.clip_count;
                g->mesh3d.primitive_count  = loaded_model.mesh.primitive_count;

                g->mesh3d.pose_trans  = (Vec3*)arena_alloc(model_arena, jc * sizeof(Vec3), 16, "pose_trans");
                g->mesh3d.pose_rot    = (Quat*)arena_alloc(model_arena, jc * sizeof(Quat), 16, "pose_rot");
                g->mesh3d.pose_scale  = (Vec3*)arena_alloc(model_arena, jc * sizeof(Vec3), 16, "pose_scale");
                g->mesh3d.world_mats  = (Mat4*)arena_alloc(model_arena, jc * sizeof(Mat4), 16, "world_mats");
                g->mesh3d.skin_mats   = (Mat4*)arena_alloc(model_arena, jc * sizeof(Mat4), 16, "skin_mats");

                // Engine will set these in init_engine / update_engine
                g->mesh3d.visible = 1;
                g->mesh3d.active_clip = loaded_model.clip_count > 6 ? 6 : 0;
                g->mesh3d.anim_time = 0.0f;
                g->mesh3d.camera_eye    = VEC3(0.0f, 1.0f, 3.0f);
                g->mesh3d.camera_target = VEC3(0.0f, 0.5f, 0.0f);
                g->mesh3d.camera_up     = VEC3(0.0f, 1.0f, 0.0f);
                g->mesh3d.model_transform = loaded_model.armature_transform;
            } else {
                fprintf(stderr, "Warning: Knight.glb loaded but has no primitives\n");
            }
        } else {
            fprintf(stderr, "Warning: Failed to allocate gltf_models sub-arena\n");
        }
    }

    // Init game timing
    g->_t_prev = (double)SDL_GetTicks() / 1000.0;
    g->dt = 0.0f;
    g->play = true;

    printf("Externals initialized (SDL3 GPU, 2 windows)\n");
    return 1;
}

// ---------------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------------

static void update_input(game *g) {
    g->input.horizontal = 0.0f;
    g->input.vertical = 0.0f;
    g->input.input_mask = 0;

    // Keyboard input
    float kb_horizontal = 0.0f;
    float kb_vertical = 0.0f;

    const bool *keys = SDL_GetKeyboardState(NULL);

    if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT])  kb_horizontal -= 1.0f;
    if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) kb_horizontal += 1.0f;
    if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP])    kb_vertical += 1.0f;
    if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN])  kb_vertical -= 1.0f;

    float kb_magnitude = sqrtf(kb_horizontal * kb_horizontal + kb_vertical * kb_vertical);
    if (kb_magnitude > 1.0f) {
        kb_horizontal /= kb_magnitude;
        kb_vertical /= kb_magnitude;
    }

    // Keyboard buttons
    if (keys[SDL_SCANCODE_SPACE] || keys[SDL_SCANCODE_J])
        g->input.input_mask |= INPUT_A;
    if (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT] || keys[SDL_SCANCODE_K])
        g->input.input_mask |= INPUT_B;
    if (keys[SDL_SCANCODE_E] || keys[SDL_SCANCODE_L])
        g->input.input_mask |= INPUT_X;
    if (keys[SDL_SCANCODE_Q] || keys[SDL_SCANCODE_I] || keys[SDL_SCANCODE_TAB])
        g->input.input_mask |= INPUT_Y;

    // Gamepad input
    float gp_horizontal = 0.0f;
    float gp_vertical = 0.0f;

    int gamepad_count = 0;
    SDL_JoystickID *gamepads = SDL_GetGamepads(&gamepad_count);
    if (gamepads && gamepad_count > 0) {
        SDL_Gamepad *pad = SDL_OpenGamepad(gamepads[0]);
        if (pad) {
            gp_horizontal = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
            gp_vertical = -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;

            const float deadzone = 0.2f;
            float magnitude = sqrtf(gp_horizontal * gp_horizontal + gp_vertical * gp_vertical);
            if (magnitude < deadzone) {
                gp_horizontal = 0.0f;
                gp_vertical = 0.0f;
            } else {
                float normalized_magnitude = (magnitude - deadzone) / (1.0f - deadzone);
                if (normalized_magnitude > 1.0f) normalized_magnitude = 1.0f;
                gp_horizontal = (gp_horizontal / magnitude) * normalized_magnitude;
                gp_vertical = (gp_vertical / magnitude) * normalized_magnitude;
            }

            if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH))
                g->input.input_mask |= INPUT_A;
            if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST))
                g->input.input_mask |= INPUT_B;
            if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_WEST))
                g->input.input_mask |= INPUT_X;
            if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_NORTH))
                g->input.input_mask |= INPUT_Y;

            SDL_CloseGamepad(pad);
        }
    }
    SDL_free(gamepads);

    // Combine keyboard and gamepad (take stronger input)
    g->input.horizontal = (fabsf(kb_horizontal) > fabsf(gp_horizontal)) ? kb_horizontal : gp_horizontal;
    g->input.vertical   = (fabsf(kb_vertical)   > fabsf(gp_vertical))   ? kb_vertical   : gp_vertical;
}

// ---------------------------------------------------------------------------
// update_externals
// ---------------------------------------------------------------------------

EXPORT void update_externals(game *g) {
    ZoneScopedN("update_externals");
    // --- Timing ---
    double now = (double)SDL_GetTicks() / 1000.0;
    double dtd = now - g->_t_prev;
    g->_t_prev = now;
    if (dtd < 0.0) dtd = 0.0;
    if (dtd > 0.1) dtd = 0.1;
    g->dt = (float)dtd;

    // --- Events ---
    {
        ZoneScopedN("SDL_PollEvents");
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                g->play = false;
                return;
            }
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                SDL_Window *evwin = SDL_GetWindowFromEvent(&event);
                if (evwin == profiler_window) {
                    SDL_ReleaseWindowFromGPUDevice(gpu_device, profiler_window);
                    SDL_DestroyWindow(profiler_window);
                    profiler_window = NULL;
                    profiler_open = false;
                } else if (evwin == window) {
                    g->play = false;
                    return;
                }
            }
        }
    }

    // --- Input ---
    update_input(g);

    // --- Window size ---
    int display_w, display_h;
    SDL_GetWindowSizeInPixels(window, &display_w, &display_h);
    g->width = display_w;
    g->height = display_h;

    // --- Ortho projection ---
    float w = (float)display_w;
    float h = (float)display_h;
    float ortho[16] = {
        2.0f/w, 0,      0,     0,
        0,      2.0f/h, 0,     0,
        0,      0,     -1.0f,  0,
        0,      0,      0,     1.0f
    };
    memcpy(g->draw_list.ortho_projection, ortho, sizeof(ortho));

    // --- Reset draw list ---
    g->draw_list.sprite_count = 0;
    g->draw_list.line_count = 0;

    // --- Call engine update (fills draw_list + updates mesh3d animation) ---
    g_update(g);

    // --- Render ---
    SDL_GPUCommandBuffer *cmd_buf = SDL_AcquireGPUCommandBuffer(gpu_device);
    if (!cmd_buf) {
        fprintf(stderr, "Failed to acquire command buffer: %s\n", SDL_GetError());
        return;
    }

    // ================================================================
    // ALL COPY PASSES FIRST (both windows) — no render passes yet
    // ================================================================

    // --- Build sprite vertex data ---
    int sprite_count = g->draw_list.sprite_count;
    int sprite_vertex_count = sprite_count * 6;
    Uint32 sprite_buf_size = (Uint32)(sprite_vertex_count * sizeof(sprite_vertex));

    SDL_GPUBuffer *sprite_gpu_buf = NULL;
    if (sprite_count > 0 && sprite_buf_size > 0) {
        SDL_GPUTransferBufferCreateInfo tbuf_info = {};
        tbuf_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbuf_info.size = sprite_buf_size;
        tbuf_info.props = 0;

        SDL_GPUTransferBuffer *sprite_transfer = SDL_CreateGPUTransferBuffer(gpu_device, &tbuf_info);
        sprite_vertex *verts = (sprite_vertex*)SDL_MapGPUTransferBuffer(gpu_device, sprite_transfer, false);

        for (int i = 0; i < sprite_count; i++) {
            const draw_command &cmd = g->draw_list.sprites[i];
            float half_w = cmd.width * 0.5f;
            float half_h = cmd.height * 0.5f;

            float x0 = cmd.x - half_w;
            float y0 = cmd.y - half_h;
            float x1 = cmd.x + half_w;
            float y1 = cmd.y + half_h;

            float u0 = cmd.uv_x;
            float v0 = cmd.uv_y;
            float u1 = cmd.uv_x + cmd.uv_w;
            float v1 = cmd.uv_y + cmd.uv_h;

            float r = cmd.tint_r, gr = cmd.tint_g, b = cmd.tint_b, a = cmd.tint_a;

            sprite_vertex *v = &verts[i * 6];
            v[0] = {x0, y1, u0, v0, r, gr, b, a};
            v[1] = {x1, y1, u1, v0, r, gr, b, a};
            v[2] = {x1, y0, u1, v1, r, gr, b, a};
            v[3] = {x0, y1, u0, v0, r, gr, b, a};
            v[4] = {x1, y0, u1, v1, r, gr, b, a};
            v[5] = {x0, y0, u0, v1, r, gr, b, a};
        }

        SDL_UnmapGPUTransferBuffer(gpu_device, sprite_transfer);

        SDL_GPUBufferCreateInfo buf_info = {};
        buf_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        buf_info.size = sprite_buf_size;
        buf_info.props = 0;
        sprite_gpu_buf = SDL_CreateGPUBuffer(gpu_device, &buf_info);

        SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(cmd_buf);
        SDL_GPUTransferBufferLocation src_loc = {};
        src_loc.transfer_buffer = sprite_transfer;
        SDL_GPUBufferRegion dst_region = {};
        dst_region.buffer = sprite_gpu_buf;
        dst_region.size = sprite_buf_size;
        SDL_UploadToGPUBuffer(copy_pass, &src_loc, &dst_region, false);
        SDL_EndGPUCopyPass(copy_pass);
        SDL_ReleaseGPUTransferBuffer(gpu_device, sprite_transfer);
    }

    // --- Build line vertex data ---
    int line_count = g->draw_list.line_count;
    int line_vertex_count = line_count * 2;
    Uint32 line_buf_size = (Uint32)(line_vertex_count * sizeof(line_vertex));

    SDL_GPUBuffer *line_gpu_buf = NULL;
    if (line_count > 0 && line_buf_size > 0) {
        SDL_GPUTransferBufferCreateInfo tbuf_info = {};
        tbuf_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbuf_info.size = line_buf_size;
        tbuf_info.props = 0;

        SDL_GPUTransferBuffer *line_transfer = SDL_CreateGPUTransferBuffer(gpu_device, &tbuf_info);
        line_vertex *verts = (line_vertex*)SDL_MapGPUTransferBuffer(gpu_device, line_transfer, false);

        for (int i = 0; i < line_count; i++) {
            const debug_line_command &ln = g->draw_list.lines[i];
            verts[i * 2 + 0] = {ln.x1, ln.y1, ln.r, ln.g, ln.b};
            verts[i * 2 + 1] = {ln.x2, ln.y2, ln.r, ln.g, ln.b};
        }

        SDL_UnmapGPUTransferBuffer(gpu_device, line_transfer);

        SDL_GPUBufferCreateInfo buf_info = {};
        buf_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        buf_info.size = line_buf_size;
        buf_info.props = 0;
        line_gpu_buf = SDL_CreateGPUBuffer(gpu_device, &buf_info);

        SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(cmd_buf);
        SDL_GPUTransferBufferLocation src_loc = {};
        src_loc.transfer_buffer = line_transfer;
        SDL_GPUBufferRegion dst_region = {};
        dst_region.buffer = line_gpu_buf;
        dst_region.size = line_buf_size;
        SDL_UploadToGPUBuffer(copy_pass, &src_loc, &dst_region, false);
        SDL_EndGPUCopyPass(copy_pass);
        SDL_ReleaseGPUTransferBuffer(gpu_device, line_transfer);
    }

    // --- Upload bone matrices (computed by engine) ---
    if (g->mesh3d.visible && g->mesh3d.skeleton.joint_count > 0 && g->mesh3d.skin_mats) {
        Uint32 bone_size = g->mesh3d.skeleton.joint_count * sizeof(Mat4);
        SDL_GPUTransferBufferCreateInfo tbuf_info = {};
        tbuf_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbuf_info.size = bone_size;
        SDL_GPUTransferBuffer *bone_transfer = SDL_CreateGPUTransferBuffer(gpu_device, &tbuf_info);
        void *map = SDL_MapGPUTransferBuffer(gpu_device, bone_transfer, false);
        memcpy(map, g->mesh3d.skin_mats, bone_size);
        SDL_UnmapGPUTransferBuffer(gpu_device, bone_transfer);

        SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(cmd_buf);
        SDL_GPUTransferBufferLocation src_loc = {};
        src_loc.transfer_buffer = bone_transfer;
        SDL_GPUBufferRegion dst_region = {};
        dst_region.buffer = bone_storage_buffer;
        dst_region.size = bone_size;
        SDL_UploadToGPUBuffer(copy_pass, &src_loc, &dst_region, false);
        SDL_EndGPUCopyPass(copy_pass);
        SDL_ReleaseGPUTransferBuffer(gpu_device, bone_transfer);
    }

    // --- Prepare Clay UI: game window (layout + upload) ---
    ui_prepare_game(cmd_buf, g);

    // --- Prepare Clay UI: profiler window (layout + upload) ---
    if (profiler_open && profiler_window) {
        profiler_prepare(cmd_buf, g);
    }

    // ================================================================
    // RENDER PASSES (all copy passes complete)
    // ================================================================

    // --- GAME WINDOW RENDER PASS ---
    SDL_GPUTexture *swapchain_texture = NULL;
    Uint32 sc_w, sc_h;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd_buf, window, &swapchain_texture, &sc_w, &sc_h)) {
        fprintf(stderr, "Failed to acquire swapchain texture: %s\n", SDL_GetError());
        SDL_SubmitGPUCommandBuffer(cmd_buf);
        return;
    }

    if (swapchain_texture) {
        // Recreate depth texture if window was resized
        if (sc_w != depth_w || sc_h != depth_h) {
            if (depth_texture) SDL_ReleaseGPUTexture(gpu_device, depth_texture);
            depth_w = sc_w;
            depth_h = sc_h;
            SDL_GPUTextureCreateInfo d_info = {};
            d_info.type = SDL_GPU_TEXTURETYPE_2D;
            d_info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
            d_info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
            d_info.width = depth_w;
            d_info.height = depth_h;
            d_info.layer_count_or_depth = 1;
            d_info.num_levels = 1;
            d_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
            depth_texture = SDL_CreateGPUTexture(gpu_device, &d_info);
        }

        SDL_GPUColorTargetInfo color_target = {};
        color_target.texture = swapchain_texture;
        color_target.clear_color = {0.45f, 0.55f, 0.60f, 1.0f};
        color_target.load_op = SDL_GPU_LOADOP_CLEAR;
        color_target.store_op = SDL_GPU_STOREOP_STORE;

        SDL_GPUDepthStencilTargetInfo depth_target = {};
        depth_target.texture = depth_texture;
        depth_target.clear_depth = 1.0f;
        depth_target.load_op = SDL_GPU_LOADOP_CLEAR;
        depth_target.store_op = SDL_GPU_STOREOP_DONT_CARE;
        depth_target.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        depth_target.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;

        SDL_GPURenderPass *render_pass = SDL_BeginGPURenderPass(cmd_buf, &color_target, 1, &depth_target);

        // --- 3D Mesh (camera + animation driven by engine via g->mesh3d) ---
        if (g->mesh3d.visible && loaded_model.mesh.primitive_count > 0) {
            SDL_BindGPUGraphicsPipeline(render_pass, mesh_pipeline);

            // Build perspective projection from engine's camera
            float aspect = (float)sc_w / (float)sc_h;
            Mat4 proj = mat4_perspective(60.0f * 3.14159265f / 180.0f, aspect, 0.1f, 100.0f);
            Mat4 view = mat4_look_at(g->mesh3d.camera_eye, g->mesh3d.camera_target, g->mesh3d.camera_up);

            mesh_uniform_data mesh_uniforms;
            memcpy(mesh_uniforms.projection, proj.m, sizeof(float) * 16);
            memcpy(mesh_uniforms.view, view.m, sizeof(float) * 16);
            memcpy(mesh_uniforms.model, g->mesh3d.model_transform.m, sizeof(float) * 16);
            SDL_PushGPUVertexUniformData(cmd_buf, 0, &mesh_uniforms, sizeof(mesh_uniforms));

            // Bind bone storage buffer (vertex stage, slot 0)
            SDL_BindGPUVertexStorageBuffers(render_pass, 0, &bone_storage_buffer, 1);

            for (uint32_t p = 0; p < loaded_model.mesh.primitive_count; p++) {
                GltfPrimitive *prim = &loaded_model.mesh.primitives[p];

                SDL_GPUBufferBinding vbuf_binding = {};
                vbuf_binding.buffer = (SDL_GPUBuffer *)prim->vertex_buffer;
                SDL_BindGPUVertexBuffers(render_pass, 0, &vbuf_binding, 1);

                SDL_GPUBufferBinding ibuf_binding = {};
                ibuf_binding.buffer = (SDL_GPUBuffer *)prim->index_buffer;
                SDL_BindGPUIndexBuffer(render_pass, &ibuf_binding, SDL_GPU_INDEXELEMENTSIZE_16BIT);

                SDL_GPUTextureSamplerBinding tex_bind = {};
                tex_bind.texture = prim->texture ? (SDL_GPUTexture *)prim->texture : white_texture;
                tex_bind.sampler = mesh_sampler;
                SDL_BindGPUFragmentSamplers(render_pass, 0, &tex_bind, 1);

                SDL_DrawGPUIndexedPrimitives(render_pass, prim->index_count, 1, 0, 0, 0);
            }
        }

        // --- 2D rendering ---
        uniform_data uniforms;
        memcpy(uniforms.projection, g->draw_list.ortho_projection, sizeof(float) * 16);
        memcpy(uniforms.view, g->draw_list.view_matrix, sizeof(float) * 16);

        // Sprites
        if (sprite_count > 0 && sprite_gpu_buf) {
            SDL_BindGPUGraphicsPipeline(render_pass, sprite_pipeline);
            SDL_PushGPUVertexUniformData(cmd_buf, 0, &uniforms, sizeof(uniforms));

            SDL_GPUBufferBinding vbuf_binding = {};
            vbuf_binding.buffer = sprite_gpu_buf;
            SDL_BindGPUVertexBuffers(render_pass, 0, &vbuf_binding, 1);

            for (int tex_id = 0; tex_id < TEXTURE_COUNT; tex_id++) {
                if (!gpu_textures[tex_id]) continue;
                bool bound = false;
                for (int i = 0; i < sprite_count; i++) {
                    if (g->draw_list.sprites[i].texture_id == tex_id) {
                        if (!bound) {
                            SDL_GPUTextureSamplerBinding tex_binding = {};
                            tex_binding.texture = gpu_textures[tex_id];
                            tex_binding.sampler = sprite_sampler;
                            SDL_BindGPUFragmentSamplers(render_pass, 0, &tex_binding, 1);
                            bound = true;
                        }
                        SDL_DrawGPUPrimitives(render_pass, 6, 1, (Uint32)(i * 6), 0);
                    }
                }
            }
        }

        // Lines
        if (line_count > 0 && line_gpu_buf) {
            SDL_BindGPUGraphicsPipeline(render_pass, line_pipeline);
            SDL_PushGPUVertexUniformData(cmd_buf, 0, &uniforms, sizeof(uniforms));

            SDL_GPUBufferBinding vbuf_binding = {};
            vbuf_binding.buffer = line_gpu_buf;
            SDL_BindGPUVertexBuffers(render_pass, 0, &vbuf_binding, 1);
            SDL_DrawGPUPrimitives(render_pass, (Uint32)line_vertex_count, 1, 0, 0);
        }

        // Game Clay UI overlay
        {
            uniform_data ui_uniforms;
            float ui_w = (float)display_w;
            float ui_h = (float)display_h;
            float ui_ortho[16] = {
                2.0f/ui_w,  0,          0,     0,
                0,         -2.0f/ui_h,  0,     0,
                0,          0,         -1.0f,  0,
               -1.0f,       1.0f,       0,     1.0f
            };
            memcpy(ui_uniforms.projection, ui_ortho, sizeof(ui_ortho));
            float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            memcpy(ui_uniforms.view, identity, sizeof(identity));

            ui_draw(render_pass, cmd_buf, &ui_uniforms, &ui_game);
        }

        SDL_EndGPURenderPass(render_pass);
    }

    // --- PROFILER WINDOW RENDER PASS ---
    if (profiler_open && profiler_window) {
        SDL_GPUTexture *prof_swapchain = NULL;
        Uint32 prof_w, prof_h;
        if (SDL_WaitAndAcquireGPUSwapchainTexture(cmd_buf, profiler_window, &prof_swapchain, &prof_w, &prof_h)
            && prof_swapchain) {

            SDL_GPUColorTargetInfo prof_target = {};
            prof_target.texture = prof_swapchain;
            prof_target.clear_color = {0.10f, 0.10f, 0.12f, 1.0f};
            prof_target.load_op = SDL_GPU_LOADOP_CLEAR;
            prof_target.store_op = SDL_GPU_STOREOP_STORE;

            SDL_GPURenderPass *prof_pass = SDL_BeginGPURenderPass(cmd_buf, &prof_target, 1, NULL);

            // Profiler uses screen-space ortho
            uniform_data prof_uniforms;
            float pw = (float)prof_w;
            float ph = (float)prof_h;
            float prof_ortho[16] = {
                2.0f/pw,  0,         0,     0,
                0,       -2.0f/ph,   0,     0,
                0,        0,        -1.0f,  0,
               -1.0f,     1.0f,      0,     1.0f
            };
            memcpy(prof_uniforms.projection, prof_ortho, sizeof(prof_ortho));
            float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            memcpy(prof_uniforms.view, identity, sizeof(identity));

            ui_draw(prof_pass, cmd_buf, &prof_uniforms, &ui_profiler);

            SDL_EndGPURenderPass(prof_pass);
        }
    }

    SDL_SubmitGPUCommandBuffer(cmd_buf);

    // Release per-frame GPU buffers
    if (sprite_gpu_buf) SDL_ReleaseGPUBuffer(gpu_device, sprite_gpu_buf);
    if (line_gpu_buf)   SDL_ReleaseGPUBuffer(gpu_device, line_gpu_buf);
    ui_release_buffers(&ui_game);
    ui_release_buffers(&ui_profiler);

    FrameMark;
}

// ---------------------------------------------------------------------------
// end_externals
// ---------------------------------------------------------------------------

EXPORT void end_externals(game *g) {
    // Release textures
    for (int i = 0; i < TEXTURE_COUNT; i++) {
        if (gpu_textures[i]) {
            SDL_ReleaseGPUTexture(gpu_device, gpu_textures[i]);
            gpu_textures[i] = NULL;
        }
    }

    // Release sampler
    if (sprite_sampler) {
        SDL_ReleaseGPUSampler(gpu_device, sprite_sampler);
        sprite_sampler = NULL;
    }

    // Clay memory is inside sub-arenas — no separate free needed
    clay_arena_game = NULL;
    clay_arena_prof = NULL;

    // Profiler window cleanup
    if (profiler_window) {
        SDL_ReleaseWindowFromGPUDevice(gpu_device, profiler_window);
        SDL_DestroyWindow(profiler_window);
        profiler_window = NULL;
    }

    // Release HarfBuzz
    if (hb_editor_font) { hb_font_destroy(hb_editor_font); hb_editor_font = NULL; }
    if (hb_editor_face) { hb_face_destroy(hb_editor_face); hb_editor_face = NULL; }
    if (hb_editor_blob) { hb_blob_destroy(hb_editor_blob); hb_editor_blob = NULL; }

    // Release font GPU buffers
    if (font_curve_buffer) { SDL_ReleaseGPUBuffer(gpu_device, font_curve_buffer); font_curve_buffer = NULL; }
    if (font_glyph_buffer) { SDL_ReleaseGPUBuffer(gpu_device, font_glyph_buffer); font_glyph_buffer = NULL; }
    if (font_grid_cell_buffer) { SDL_ReleaseGPUBuffer(gpu_device, font_grid_cell_buffer); font_grid_cell_buffer = NULL; }
    if (font_grid_index_buffer) { SDL_ReleaseGPUBuffer(gpu_device, font_grid_index_buffer); font_grid_index_buffer = NULL; }

    // Release pipelines
    if (sprite_pipeline) {
        SDL_ReleaseGPUGraphicsPipeline(gpu_device, sprite_pipeline);
        sprite_pipeline = NULL;
    }
    if (line_pipeline) {
        SDL_ReleaseGPUGraphicsPipeline(gpu_device, line_pipeline);
        line_pipeline = NULL;
    }
    if (ui_rect_pipeline) {
        SDL_ReleaseGPUGraphicsPipeline(gpu_device, ui_rect_pipeline);
        ui_rect_pipeline = NULL;
    }
    if (font_pipeline) {
        SDL_ReleaseGPUGraphicsPipeline(gpu_device, font_pipeline);
        font_pipeline = NULL;
    }
    if (mesh_pipeline) {
        SDL_ReleaseGPUGraphicsPipeline(gpu_device, mesh_pipeline);
        mesh_pipeline = NULL;
    }

    // Release mesh-related GPU resources
    if (mesh_sampler) { SDL_ReleaseGPUSampler(gpu_device, mesh_sampler); mesh_sampler = NULL; }
    if (depth_texture) { SDL_ReleaseGPUTexture(gpu_device, depth_texture); depth_texture = NULL; }
    if (white_texture) { SDL_ReleaseGPUTexture(gpu_device, white_texture); white_texture = NULL; }
    if (bone_storage_buffer) { SDL_ReleaseGPUBuffer(gpu_device, bone_storage_buffer); bone_storage_buffer = NULL; }

    // Release glTF model GPU resources
    for (uint32_t p = 0; p < loaded_model.mesh.primitive_count; p++) {
        GltfPrimitive *prim = &loaded_model.mesh.primitives[p];
        if (prim->vertex_buffer) SDL_ReleaseGPUBuffer(gpu_device, (SDL_GPUBuffer *)prim->vertex_buffer);
        if (prim->index_buffer) SDL_ReleaseGPUBuffer(gpu_device, (SDL_GPUBuffer *)prim->index_buffer);
        if (prim->texture) SDL_ReleaseGPUTexture(gpu_device, (SDL_GPUTexture *)prim->texture);
    }

    // Shadercross
    SDL_ShaderCross_Quit();

    // Device & window
    if (gpu_device) {
        SDL_DestroyGPUDevice(gpu_device);
        gpu_device = NULL;
    }
    if (window) {
        SDL_DestroyWindow(window);
        window = NULL;
    }

    // Free arena (single free for all engine memory)
    if (g->arena.base) {
        free(g->arena.base);
        g->arena.base = NULL;
    }

    SDL_Quit();
}

// ---------------------------------------------------------------------------
// Engine callbacks
// ---------------------------------------------------------------------------

EXPORT void init_engine(game *g) {
    g_init(g);
}

EXPORT void destroy_engine(game *g) {
    g_destroy(g);
}

EXPORT void assign_init(init func) {
    g_init = func;
}

EXPORT void assign_destroy(destroy func) {
    g_destroy = func;
}

EXPORT void assign_update(update func) {
    g_update = func;
}
