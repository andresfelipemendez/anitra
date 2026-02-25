#include <externals.h>
#include <game.h>
#include <debug_render.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3_shadercross/SDL_shadercross.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <hb.h>
#include <hb-ot.h>

#define CLAY_IMPLEMENTATION
#include "clay.h"

// ---------------------------------------------------------------------------
// Static globals (replace GL state)
// ---------------------------------------------------------------------------

static SDL_Window *window = NULL;
static SDL_GPUDevice *gpu_device = NULL;

// Sprite pipeline
static SDL_GPUGraphicsPipeline *sprite_pipeline = NULL;
static SDL_GPUSampler *sprite_sampler = NULL;

// Debug line pipeline
static SDL_GPUGraphicsPipeline *line_pipeline = NULL;

// UI rect pipeline (Clay rectangles)
static SDL_GPUGraphicsPipeline *ui_rect_pipeline = NULL;

// Font pipeline (vector text)
static SDL_GPUGraphicsPipeline *font_pipeline = NULL;

// Font GPU storage buffers
static SDL_GPUBuffer *font_curve_buffer = NULL;
static SDL_GPUBuffer *font_glyph_buffer = NULL;
static SDL_GPUBuffer *font_grid_cell_buffer = NULL;
static SDL_GPUBuffer *font_grid_index_buffer = NULL;

// HarfBuzz
static hb_font_t *hb_editor_font = NULL;
static hb_blob_t *hb_editor_blob = NULL;
static hb_face_t *hb_editor_face = NULL;

// Clay UI
static uint8_t *clay_memory = NULL;
static Clay_Context *clay_context = NULL;

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
    float height = (float)config->fontSize * (editor_font.ascent - editor_font.descent + editor_font.line_gap);
    return Clay_Dimensions{width, height};
}

// ---------------------------------------------------------------------------
// UI rendering: process Clay render commands -> GPU draw calls
// ---------------------------------------------------------------------------

#define MAX_UI_RECT_VERTICES  (4096 * 6)
#define MAX_FONT_VERTICES     (4096 * 6)

// UI render state: populated by ui_prepare, consumed by ui_draw
static ui_rect_vertex ui_rect_verts[MAX_UI_RECT_VERTICES];
static font_vertex    ui_font_verts[MAX_FONT_VERTICES];
static int            ui_rect_vert_count = 0;
static int            ui_font_vert_count = 0;
static SDL_GPUBuffer *ui_rect_gpu_buf = NULL;
static SDL_GPUBuffer *ui_font_gpu_buf = NULL;

// Phase 1: Run Clay layout, build vertices, upload via copy pass (before render pass)
static void ui_prepare(SDL_GPUCommandBuffer *cmd_buf) {
    int win_w, win_h;
    SDL_GetWindowSizeInPixels(window, &win_w, &win_h);
    Clay_SetLayoutDimensions(Clay_Dimensions{(float)win_w, (float)win_h});

    Clay_BeginLayout();

    // Test panel
    CLAY(CLAY_ID("TestPanel"), {
        .layout = {
            .sizing = { CLAY_SIZING_FIXED(600), CLAY_SIZING_FIXED(200) },
            .padding = CLAY_PADDING_ALL(16),
            .childGap = 8,
            .layoutDirection = CLAY_TOP_TO_BOTTOM
        },
        .backgroundColor = {40, 40, 40, 220},
        .cornerRadius = CLAY_CORNER_RADIUS(4)
    }) {
        CLAY_TEXT(CLAY_STRING("Hello from Clay!"),
            CLAY_TEXT_CONFIG({.textColor = {255, 255, 255, 255}, .fontSize = 48}));
        CLAY_TEXT(CLAY_STRING("GPU vector fonts"),
            CLAY_TEXT_CONFIG({.textColor = {180, 180, 255, 255}, .fontSize = 36}));
    }

    Clay_RenderCommandArray commands = Clay_EndLayout();

    // Build vertex arrays from Clay commands
    ui_rect_vert_count = 0;
    ui_font_vert_count = 0;

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

            if (ui_rect_vert_count + 6 <= MAX_UI_RECT_VERTICES) {
                ui_rect_vertex *v = &ui_rect_verts[ui_rect_vert_count];
                v[0] = {x0, y0, r, g, b, a};
                v[1] = {x1, y0, r, g, b, a};
                v[2] = {x1, y1, r, g, b, a};
                v[3] = {x0, y0, r, g, b, a};
                v[4] = {x1, y1, r, g, b, a};
                v[5] = {x0, y1, r, g, b, a};
                ui_rect_vert_count += 6;
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


            float cursor_x = box.x;
            float baseline_y = box.y + font_size * editor_font.ascent;
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

                    float gx = cursor_x + info->bbox_min_x * font_size + glyph_positions[gi].x_offset * scale;
                    float gy = baseline_y - info->bbox_max_y * font_size + glyph_positions[gi].y_offset * scale;

                    if (ui_font_vert_count + 6 <= MAX_FONT_VERTICES) {
                        font_vertex *v = &ui_font_verts[ui_font_vert_count];
                        v[0] = {gx,           gy,           0, 0, r, g, b, a, cp};
                        v[1] = {gx + glyph_w, gy,           1, 0, r, g, b, a, cp};
                        v[2] = {gx + glyph_w, gy + glyph_h, 1, 1, r, g, b, a, cp};
                        v[3] = {gx,           gy,           0, 0, r, g, b, a, cp};
                        v[4] = {gx + glyph_w, gy + glyph_h, 1, 1, r, g, b, a, cp};
                        v[5] = {gx,           gy + glyph_h, 0, 1, r, g, b, a, cp};
                        ui_font_vert_count += 6;
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

    // Upload rect vertices
    if (ui_rect_vert_count > 0) {
        Uint32 buf_size = (Uint32)(ui_rect_vert_count * sizeof(ui_rect_vertex));

        SDL_GPUTransferBufferCreateInfo tbuf_info = {};
        tbuf_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbuf_info.size = buf_size;
        SDL_GPUTransferBuffer *xfer = SDL_CreateGPUTransferBuffer(gpu_device, &tbuf_info);
        void *mapped = SDL_MapGPUTransferBuffer(gpu_device, xfer, false);
        memcpy(mapped, ui_rect_verts, buf_size);
        SDL_UnmapGPUTransferBuffer(gpu_device, xfer);

        SDL_GPUBufferCreateInfo gbuf_info = {};
        gbuf_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        gbuf_info.size = buf_size;
        ui_rect_gpu_buf = SDL_CreateGPUBuffer(gpu_device, &gbuf_info);

        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd_buf);
        SDL_GPUTransferBufferLocation src = {};
        src.transfer_buffer = xfer;
        SDL_GPUBufferRegion dst = {};
        dst.buffer = ui_rect_gpu_buf;
        dst.size = buf_size;
        SDL_UploadToGPUBuffer(copy, &src, &dst, false);
        SDL_EndGPUCopyPass(copy);
        SDL_ReleaseGPUTransferBuffer(gpu_device, xfer);
    }

    // Upload font vertices
    if (ui_font_vert_count > 0) {
        Uint32 buf_size = (Uint32)(ui_font_vert_count * sizeof(font_vertex));

        SDL_GPUTransferBufferCreateInfo tbuf_info = {};
        tbuf_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbuf_info.size = buf_size;
        SDL_GPUTransferBuffer *xfer = SDL_CreateGPUTransferBuffer(gpu_device, &tbuf_info);
        void *mapped = SDL_MapGPUTransferBuffer(gpu_device, xfer, false);
        memcpy(mapped, ui_font_verts, buf_size);
        SDL_UnmapGPUTransferBuffer(gpu_device, xfer);

        SDL_GPUBufferCreateInfo gbuf_info = {};
        gbuf_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        gbuf_info.size = buf_size;
        ui_font_gpu_buf = SDL_CreateGPUBuffer(gpu_device, &gbuf_info);

        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd_buf);
        SDL_GPUTransferBufferLocation src = {};
        src.transfer_buffer = xfer;
        SDL_GPUBufferRegion dst = {};
        dst.buffer = ui_font_gpu_buf;
        dst.size = buf_size;
        SDL_UploadToGPUBuffer(copy, &src, &dst, false);
        SDL_EndGPUCopyPass(copy);
        SDL_ReleaseGPUTransferBuffer(gpu_device, xfer);
    }
}

// Phase 2: Draw UI during render pass (after sprites/lines)
static void ui_draw(SDL_GPURenderPass *render_pass, SDL_GPUCommandBuffer *cmd_buf, uniform_data *uniforms) {
    if (ui_rect_vert_count > 0 && ui_rect_gpu_buf) {
        SDL_BindGPUGraphicsPipeline(render_pass, ui_rect_pipeline);
        SDL_PushGPUVertexUniformData(cmd_buf, 0, uniforms, sizeof(*uniforms));

        SDL_GPUBufferBinding vbuf_binding = {};
        vbuf_binding.buffer = ui_rect_gpu_buf;
        SDL_BindGPUVertexBuffers(render_pass, 0, &vbuf_binding, 1);
        SDL_DrawGPUPrimitives(render_pass, (Uint32)ui_rect_vert_count, 1, 0, 0);
    }

    if (ui_font_vert_count > 0 && ui_font_gpu_buf) {
        SDL_BindGPUGraphicsPipeline(render_pass, font_pipeline);
        SDL_PushGPUVertexUniformData(cmd_buf, 0, uniforms, sizeof(*uniforms));

        SDL_GPUBuffer *storage_bufs[4] = {
            font_curve_buffer, font_glyph_buffer,
            font_grid_cell_buffer, font_grid_index_buffer
        };
        SDL_BindGPUFragmentStorageBuffers(render_pass, 0, storage_bufs, 4);

        SDL_GPUBufferBinding vbuf_binding = {};
        vbuf_binding.buffer = ui_font_gpu_buf;
        SDL_BindGPUVertexBuffers(render_pass, 0, &vbuf_binding, 1);
        SDL_DrawGPUPrimitives(render_pass, (Uint32)ui_font_vert_count, 1, 0, 0);
    }
}

// Cleanup per-frame UI GPU buffers
static void ui_release_buffers() {
    if (ui_rect_gpu_buf) { SDL_ReleaseGPUBuffer(gpu_device, ui_rect_gpu_buf); ui_rect_gpu_buf = NULL; }
    if (ui_font_gpu_buf) { SDL_ReleaseGPUBuffer(gpu_device, ui_font_gpu_buf); ui_font_gpu_buf = NULL; }
}

// ---------------------------------------------------------------------------
// init_externals
// ---------------------------------------------------------------------------

EXPORT int init_externals(game *g) {
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
        pipe_info.target_info.has_depth_stencil_target = false;

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
        pipe_info.target_info.has_depth_stencil_target = false;

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

        font_pipeline = SDL_CreateGPUGraphicsPipeline(gpu_device, &pipe_info);
        if (!font_pipeline) {
            fprintf(stderr, "Failed to create font pipeline: %s\n", SDL_GetError());
            return -1;
        }

        SDL_ReleaseGPUShader(gpu_device, font_vs);
        SDL_ReleaseGPUShader(gpu_device, font_fs);
    }

    // 12. Create sampler (nearest-neighbor for pixel art)
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

    // Initialize Clay UI
    {
        uint64_t clay_mem_size = Clay_MinMemorySize();
        clay_memory = (uint8_t*)malloc(clay_mem_size);
        Clay_Arena clay_arena = Clay_CreateArenaWithCapacityAndMemory(clay_mem_size, clay_memory);

        int window_w, window_h;
        SDL_GetWindowSize(window, &window_w, &window_h);

        Clay_ErrorHandler err_handler = {};
        clay_context = Clay_Initialize(clay_arena, Clay_Dimensions{(float)window_w, (float)window_h}, err_handler);
        Clay_SetMeasureTextFunction(clay_measure_text, NULL);
        printf("Clay initialized (%llu bytes arena)\n", (unsigned long long)clay_mem_size);
    }

    // 12. Init game timing and debug renderer
    g->_t_prev = (double)SDL_GetTicks() / 1000.0;
    g->dt = 0.0f;
    g->play = true;

    // Init debug renderer vertex buffer
    g->debug_renderer.max_lines = 1000;
    g->debug_renderer.vertex_buffer = (float*)malloc(g->debug_renderer.max_lines * 10 * sizeof(float));
    g->debug_renderer.current_line_count = 0;

    printf("Externals initialized (SDL3 GPU)\n");
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
    // --- Timing ---
    double now = (double)SDL_GetTicks() / 1000.0;
    double dtd = now - g->_t_prev;
    g->_t_prev = now;
    if (dtd < 0.0) dtd = 0.0;
    if (dtd > 0.1) dtd = 0.1;
    g->dt = (float)dtd;

    // --- Events ---
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            g->play = false;
            return;
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

    // --- Call engine update (fills draw_list) ---
    g_update(g);

    // --- Render ---
    SDL_GPUCommandBuffer *cmd_buf = SDL_AcquireGPUCommandBuffer(gpu_device);
    if (!cmd_buf) {
        fprintf(stderr, "Failed to acquire command buffer: %s\n", SDL_GetError());
        return;
    }

    SDL_GPUTexture *swapchain_texture = NULL;
    Uint32 sc_w, sc_h;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd_buf, window, &swapchain_texture, &sc_w, &sc_h)) {
        fprintf(stderr, "Failed to acquire swapchain texture: %s\n", SDL_GetError());
        SDL_SubmitGPUCommandBuffer(cmd_buf);
        return;
    }

    if (!swapchain_texture) {
        SDL_SubmitGPUCommandBuffer(cmd_buf);
        return;
    }

    // --- Build sprite vertex data ---
    int sprite_count = g->draw_list.sprite_count;
    int sprite_vertex_count = sprite_count * 6; // 6 vertices per sprite (2 triangles)
    Uint32 sprite_buf_size = (Uint32)(sprite_vertex_count * sizeof(sprite_vertex));

    SDL_GPUBuffer *sprite_gpu_buf = NULL;
    if (sprite_count > 0 && sprite_buf_size > 0) {
        // Create transfer buffer for sprite vertices
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

            // Triangle 1: top-left, top-right, bottom-right
            v[0] = {x0, y1, u0, v0, r, gr, b, a}; // top-left
            v[1] = {x1, y1, u1, v0, r, gr, b, a}; // top-right
            v[2] = {x1, y0, u1, v1, r, gr, b, a}; // bottom-right

            // Triangle 2: top-left, bottom-right, bottom-left
            v[3] = {x0, y1, u0, v0, r, gr, b, a}; // top-left
            v[4] = {x1, y0, u1, v1, r, gr, b, a}; // bottom-right
            v[5] = {x0, y0, u0, v1, r, gr, b, a}; // bottom-left
        }

        SDL_UnmapGPUTransferBuffer(gpu_device, sprite_transfer);

        // Create GPU vertex buffer
        SDL_GPUBufferCreateInfo buf_info = {};
        buf_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        buf_info.size = sprite_buf_size;
        buf_info.props = 0;

        sprite_gpu_buf = SDL_CreateGPUBuffer(gpu_device, &buf_info);

        // Copy pass: transfer -> GPU buffer
        SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(cmd_buf);

        SDL_GPUTransferBufferLocation src_loc = {};
        src_loc.transfer_buffer = sprite_transfer;
        src_loc.offset = 0;

        SDL_GPUBufferRegion dst_region = {};
        dst_region.buffer = sprite_gpu_buf;
        dst_region.offset = 0;
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
        src_loc.offset = 0;

        SDL_GPUBufferRegion dst_region = {};
        dst_region.buffer = line_gpu_buf;
        dst_region.offset = 0;
        dst_region.size = line_buf_size;

        SDL_UploadToGPUBuffer(copy_pass, &src_loc, &dst_region, false);
        SDL_EndGPUCopyPass(copy_pass);

        SDL_ReleaseGPUTransferBuffer(gpu_device, line_transfer);
    }

    // --- Prepare Clay UI (layout + upload before render pass) ---
    ui_prepare(cmd_buf);

    // --- Begin render pass ---
    SDL_GPUColorTargetInfo color_target = {};
    color_target.texture = swapchain_texture;
    color_target.clear_color = {0.45f, 0.55f, 0.60f, 1.0f};
    color_target.load_op = SDL_GPU_LOADOP_CLEAR;
    color_target.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass *render_pass = SDL_BeginGPURenderPass(cmd_buf, &color_target, 1, NULL);

    // --- Uniforms ---
    uniform_data uniforms;
    memcpy(uniforms.projection, g->draw_list.ortho_projection, sizeof(float) * 16);
    memcpy(uniforms.view, g->draw_list.view_matrix, sizeof(float) * 16);

    // --- Sprite pass ---
    if (sprite_count > 0 && sprite_gpu_buf) {
        SDL_BindGPUGraphicsPipeline(render_pass, sprite_pipeline);

        SDL_PushGPUVertexUniformData(cmd_buf, 0, &uniforms, sizeof(uniforms));

        SDL_GPUBufferBinding vbuf_binding = {};
        vbuf_binding.buffer = sprite_gpu_buf;
        vbuf_binding.offset = 0;
        SDL_BindGPUVertexBuffers(render_pass, 0, &vbuf_binding, 1);

        // Sort/batch by texture_id for fewer bind calls
        // Simple approach: iterate textures, draw matching sprites
        for (int tex_id = 0; tex_id < TEXTURE_COUNT; tex_id++) {
            if (!gpu_textures[tex_id]) continue;

            // Count sprites with this texture
            int first_vertex = -1;
            int count = 0;
            // Since sprites might be interleaved, we draw per-sprite ranges
            // But for simplicity, just draw each sprite batch individually
            for (int i = 0; i < sprite_count; i++) {
                if (g->draw_list.sprites[i].texture_id == tex_id) {
                    if (first_vertex < 0) {
                        // Bind texture for this batch
                        SDL_GPUTextureSamplerBinding tex_binding = {};
                        tex_binding.texture = gpu_textures[tex_id];
                        tex_binding.sampler = sprite_sampler;
                        SDL_BindGPUFragmentSamplers(render_pass, 0, &tex_binding, 1);
                        first_vertex = i * 6;
                    }
                    count++;
                }
            }

            if (count > 0) {
                // Draw all sprites with this texture
                // Since sprites are interleaved, draw each individually
                for (int i = 0; i < sprite_count; i++) {
                    if (g->draw_list.sprites[i].texture_id == tex_id) {
                        SDL_DrawGPUPrimitives(render_pass, 6, 1, (Uint32)(i * 6), 0);
                    }
                }
            }
        }
    }

    // --- Line pass ---
    if (line_count > 0 && line_gpu_buf) {
        SDL_BindGPUGraphicsPipeline(render_pass, line_pipeline);

        SDL_PushGPUVertexUniformData(cmd_buf, 0, &uniforms, sizeof(uniforms));

        SDL_GPUBufferBinding vbuf_binding = {};
        vbuf_binding.buffer = line_gpu_buf;
        vbuf_binding.offset = 0;
        SDL_BindGPUVertexBuffers(render_pass, 0, &vbuf_binding, 1);

        SDL_DrawGPUPrimitives(render_pass, (Uint32)(line_vertex_count), 1, 0, 0);
    }

    // --- Clay UI draw ---
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
        float identity[16] = {
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
            0, 0, 0, 1
        };
        memcpy(ui_uniforms.view, identity, sizeof(identity));

        ui_draw(render_pass, cmd_buf, &ui_uniforms);
    }

    SDL_EndGPURenderPass(render_pass);
    SDL_SubmitGPUCommandBuffer(cmd_buf);

    // Release per-frame GPU buffers
    if (sprite_gpu_buf) SDL_ReleaseGPUBuffer(gpu_device, sprite_gpu_buf);
    if (line_gpu_buf)   SDL_ReleaseGPUBuffer(gpu_device, line_gpu_buf);
    ui_release_buffers();
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

    // Release Clay
    if (clay_memory) { free(clay_memory); clay_memory = NULL; }

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

    // Free debug renderer
    if (g->debug_renderer.vertex_buffer) {
        free(g->debug_renderer.vertex_buffer);
        g->debug_renderer.vertex_buffer = NULL;
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
