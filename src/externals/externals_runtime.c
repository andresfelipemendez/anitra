#include <externals.h>
#include <game.h>
#include "draw_processor.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3_shadercross/SDL_shadercross.h>

#include <tracy/TracyC.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "msdf_gen.h"

#include <hb.h>
#include <hb-ot.h>

#define CACHE_PROF_IMPL
#include "cache_profiler.h"

#define CPU_PROF_IMPL
#include "cpu_profiler.h"

#define CLAY_IMPLEMENTATION
#include "clay.h"

#include "gltf_types.h"

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

// Mesh (3D skinned) pipeline
static SDL_GPUGraphicsPipeline *mesh_pipeline = NULL;
static SDL_GPUSampler *mesh_sampler = NULL;
/* Per-panel depth textures live in panel_depth[] below */
static SDL_GPUBuffer *bone_storage_buffer = NULL;
static SDL_GPUBuffer *bone_identity_buffer = NULL;
static SDL_GPUTexture *white_texture = NULL;

#define MAX_BONES (ANIM_SM_MAX_ENTITIES * ANIM_SM_MAX_JOINTS)
#define FLOOR_INSTANCE_MAX 64

// Font MSDF atlas
static SDL_GPUTexture *font_atlas_texture = NULL;
static SDL_GPUSampler *font_atlas_sampler = NULL;
static SDL_GPUTexture *icon_atlas_texture = NULL;
static SDL_GPUSampler *icon_atlas_sampler = NULL;

// HarfBuzz
static hb_font_t *hb_editor_font = NULL;
static hb_blob_t *hb_editor_blob = NULL;
static hb_face_t *hb_editor_face = NULL;

// Clay UI — game window
static arena         *clay_arena_game = NULL;    // sub-arena backing Clay game context
static Clay_Context  *clay_context = NULL;

// Clay profiler context is now owned by editor.dll — render commands are read from editor_state

// Editor line pipeline (editor renders to panel texture)
static SDL_GPUGraphicsPipeline *editor_line_pipeline = NULL;

// Dock panel offscreen textures (render-to-texture per panel, composited into window)
static SDL_GPUTexture *panel_color[PANEL_COUNT] = {0};
static SDL_GPUTexture *panel_depth[PANEL_COUNT] = {0};
static int panel_tex_w[PANEL_COUNT] = {0};
static int panel_tex_h[PANEL_COUNT] = {0};
static float display_density = 1.0f;  /* pixel density (2.0 on Retina) */

// Composite pipeline (draws panel textures into window swapchain)
static SDL_GPUGraphicsPipeline *composite_pipeline = NULL;
static SDL_GPUSampler *composite_sampler = NULL;
static SDL_GPUTextureFormat offscreen_format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

// Grid texture pipeline (draws profiler grid texture into profiler render pass)
static SDL_GPUGraphicsPipeline *grid_tex_pipeline = NULL;
static SDL_GPUSampler *grid_sampler = NULL;
static SDL_GPUTexture *grid_texture = NULL;
static int grid_tex_w = 0, grid_tex_h = 0;
static SDL_GPUBuffer *grid_quad_buf = NULL;  /* 6 composite_vertex for the textured quad */

// Pre-rendered tab header textures (CPU-rasterized text, created once at init)
static SDL_GPUTexture *header_textures[PANEL_COUNT] = {0};
static int header_tex_w[PANEL_COUNT] = {0};
/* panel_names[] defined in editor/editor.h */

// Textures (one per TextureID enum)
static SDL_GPUTexture *gpu_textures[TEXTURE_COUNT] = {NULL};

// Engine callbacks (loaded by core.cpp)
static engine_init_fn g_init = NULL;
static engine_destroy_fn g_destroy = NULL;
static engine_update_fn g_update = NULL;

// Editor callbacks (loaded by core.cpp)
static editor_init_fn g_editor_init = NULL;
static editor_destroy_fn g_editor_destroy = NULL;
static editor_update_fn g_editor_update = NULL;
static editor_handle_event_fn g_editor_handle_event = NULL;
static int cpu_prof_capture_paused = 0;
static int cpu_prof_capture_was_paused = 0;

#define FRAME_CPU_ZONE_BEGIN(name) do { if (!cpu_prof_capture_paused) cpu_zone_begin(name); } while (0)
#define FRAME_CPU_ZONE_END()       do { if (!cpu_prof_capture_paused) cpu_zone_end(); } while (0)
#define FRAME_CACHE_ZONE_BEGIN(name) do { cache_zone_begin(name); } while (0)
#define FRAME_CACHE_ZONE_END()       do { cache_zone_end(); } while (0)

// ---------------------------------------------------------------------------
// Vertex structures
// ---------------------------------------------------------------------------

typedef struct ui_rect_vertex {
    float x, y;
    float r, g, b, a;
} ui_rect_vertex;

typedef struct font_vertex {
    float x, y;       // screen position
    float u, v;        // UV into MSDF atlas
    float r, g, b, a;  // text color
} font_vertex;

typedef struct composite_vertex {
    float x, y;       // NDC position
    float u, v;        // UV
    float r, g, b, a;  // tint color (multiplied with texture)
} composite_vertex;

/* Build composite quad vertices for a panel at a given rect in a window.
   Converts pixel rect to NDC and writes 6 vertices (2 triangles).
   r,g,b,a is the tint color multiplied with the texture sample. */
static void build_composite_quad(composite_vertex *out, float px, float py,
                                  float pw, float ph, float win_w, float win_h,
                                  float u0_in, float v0_in, float u1_in, float v1_in,
                                  float r, float g, float b, float a)
{
    /* Pixel to NDC: ndc_x = 2*px/win_w - 1, ndc_y = 1 - 2*py/win_h */
    float x0 = 2.0f * px / win_w - 1.0f;
    float y0 = 1.0f - 2.0f * py / win_h;
    float x1 = 2.0f * (px + pw) / win_w - 1.0f;
    float y1 = 1.0f - 2.0f * (py + ph) / win_h;

    /* Triangle 1 */
    out[0] = (composite_vertex){x0, y0, u0_in, v0_in, r, g, b, a};
    out[1] = (composite_vertex){x1, y0, u1_in, v0_in, r, g, b, a};
    out[2] = (composite_vertex){x1, y1, u1_in, v1_in, r, g, b, a};
    /* Triangle 2 */
    out[3] = (composite_vertex){x0, y0, u0_in, v0_in, r, g, b, a};
    out[4] = (composite_vertex){x1, y1, u1_in, v1_in, r, g, b, a};
    out[5] = (composite_vertex){x0, y1, u0_in, v1_in, r, g, b, a};
}

/* ── Per-window composite data (used in multi-window composite pass) ──── */
#define MAX_COMP_QUADS 64  /* max quads per window (tabs + panels) */
typedef enum { CQUAD_HEADER, CQUAD_PANEL, CQUAD_DROP_ZONE } CompQuadType;
typedef struct {
    CompQuadType type;
    PanelId panel;  /* which panel's texture to bind */
} CompQuadInfo;
typedef struct {
    SDL_GPUBuffer *gpu_buf;
    int quad_count;
    CompQuadInfo quads[MAX_COMP_QUADS]; /* per-quad: what texture to bind */
} CompWindowData;

/* Count total composite quads needed for a subtree.
   Each visible leaf produces: panel_count header quads + 1 panel content quad. */
static int count_tree_quads(dock_state *d, int node_idx)
{
    DockNode *n;
    if (node_idx < 0 || node_idx >= MAX_DOCK_NODES) return 0;
    n = &d->nodes[node_idx];
    if (!n->in_use) return 0;
    if (n->type == DOCK_TABS) {
        PanelId pid;
        if (n->panel_count == 0) return 0;
        pid = n->panels[n->active_tab];
        if (panel_color[pid] && n->w > 0 && (n->h - DOCK_HEADER_HEIGHT) > 0)
            return n->panel_count + 1; /* N tab headers + 1 panel content */
        return 0;
    }
    return count_tree_quads(d, n->children[0]) + count_tree_quads(d, n->children[1]);
}

/* Build composite quads for all visible leaves in a subtree.
   Per leaf: N tab header quads (side-by-side) + 1 panel content quad.
   Returns updated qi (quad index).
   Also records per-quad info (type + panel) into quads_out. */
static int build_tree_quads(dock_state *d, int node_idx,
                             composite_vertex *verts, int qi,
                             float win_w, float win_h,
                             CompQuadInfo *quads_out, int *qcount, int max_quads)
{
    DockNode *n;
    if (node_idx < 0 || node_idx >= MAX_DOCK_NODES) return qi;
    n = &d->nodes[node_idx];
    if (!n->in_use) return qi;

    if (n->type == DOCK_TABS) {
        PanelId pid;
        float px, pw, content_h;
        int ti;
        float tab_w;
        if (n->panel_count == 0) return qi;
        pid = n->panels[n->active_tab];
        if (!panel_color[pid]) return qi;
        px = n->x; pw = n->w;
        content_h = n->h - DOCK_HEADER_HEIGHT;
        if (pw <= 0 || content_h <= 0) return qi;

        /* Tab header quads — one per panel, side by side */
        tab_w = pw / (float)n->panel_count;
        for (ti = 0; ti < n->panel_count; ti++) {
            float tx = px + tab_w * ti;
            float u_right = tab_w / 1600.0f;
            /* Active tab: bright, inactive: dimmed */
            float tint = (ti == n->active_tab) ? 1.0f : 0.5f;
            if (u_right > 1.0f) u_right = 1.0f;
            build_composite_quad(&verts[qi * 6], tx, n->y, tab_w, (float)DOCK_HEADER_HEIGHT,
                                 win_w, win_h, 0.0f, 0.0f, u_right, 1.0f,
                                 tint, tint, tint, 1.0f);
            if (*qcount < max_quads) {
                quads_out[*qcount].type = CQUAD_HEADER;
                quads_out[*qcount].panel = n->panels[ti];
                (*qcount)++;
            }
            qi++;
        }

        /* Panel content quad (active tab's texture) */
        build_composite_quad(&verts[qi * 6], px, n->y + DOCK_HEADER_HEIGHT,
                             pw, content_h, win_w, win_h,
                             0.0f, 0.0f, 1.0f, 1.0f,
                             1.0f, 1.0f, 1.0f, 1.0f);
        if (*qcount < max_quads) {
            quads_out[*qcount].type = CQUAD_PANEL;
            quads_out[*qcount].panel = pid;
            (*qcount)++;
        }
        qi++;

        return qi;
    }

    qi = build_tree_quads(d, n->children[0], verts, qi, win_w, win_h, quads_out, qcount, max_quads);
    qi = build_tree_quads(d, n->children[1], verts, qi, win_w, win_h, quads_out, qcount, max_quads);
    return qi;
}

/* Build a drop zone overlay quad for visual drag feedback.
   Returns updated qi, or original qi if no drag is active on this window. */
static int build_drop_zone_quad(dock_state *d, int win_idx,
                                 composite_vertex *verts, int qi,
                                 float win_w, float win_h,
                                 CompQuadInfo *quads_out, int *qcount, int max_quads)
{
    DragState *drag = &d->drag;
    DockNode *hn;
    float zx, zy, zw, zh;

    if (drag->phase != DRAG_ACTIVE) return qi;
    if (drag->hover_window != win_idx) return qi;
    if (drag->hover_node < 0 || drag->hover_zone == DROP_NONE) return qi;
    hn = &d->nodes[drag->hover_node];
    if (!hn->in_use) return qi;

    /* Compute the highlighted zone rect */
    zx = hn->x; zy = hn->y; zw = hn->w; zh = hn->h;
    switch (drag->hover_zone) {
        case DROP_LEFT:   zw = hn->w * 0.3f; break;
        case DROP_RIGHT:  zx = hn->x + hn->w * 0.7f; zw = hn->w * 0.3f; break;
        case DROP_TOP:    zh = hn->h * 0.3f; break;
        case DROP_BOTTOM: zy = hn->y + hn->h * 0.7f; zh = hn->h * 0.3f; break;
        case DROP_CENTER: break; /* full node */
        default: return qi;
    }

    build_composite_quad(&verts[qi * 6], zx, zy, zw, zh,
                         win_w, win_h, 0.0f, 0.0f, 1.0f, 1.0f,
                         0.3f, 0.5f, 1.0f, 0.5f); /* semi-transparent blue overlay */
    if (*qcount < max_quads) {
        quads_out[*qcount].type = CQUAD_DROP_ZONE;
        quads_out[*qcount].panel = PANEL_GAME; /* unused, just needs a value */
        (*qcount)++;
    }
    qi++;
    return qi;
}

// ---------------------------------------------------------------------------
// Uniform data (projection + view, matching HLSL cbuffer)
// ---------------------------------------------------------------------------

typedef struct uniform_data {
    float projection[16];
    float view[16];
} uniform_data;

typedef struct mesh_uniform_data {
    float projection[16];
    float view[16];
    float model[16];
    uint32_t bone_offset;
    uint32_t _pad[3]; /* align to 16 bytes for std140 */
} mesh_uniform_data;

static scene_model_asset *scene_asset_for_index(game_state *gs, int asset_index) {
    if (!gs || asset_index < 0 || asset_index >= gs->scene_model_asset_count) return NULL;
    return &gs->scene_model_assets[asset_index];
}

static void draw_scene_meshes(memory *m,
                              SDL_GPURenderPass *render_pass,
                              SDL_GPUCommandBuffer *cmd_buf,
                              Mat4 projection,
                              Mat4 view) {
    int i;
    game_state *gs = &m->game;
    if (!mesh_pipeline || !render_pass || !cmd_buf) return;
    if (!gs->dl.meshes || gs->dl.mesh_count <= 0) return;
    if (!bone_identity_buffer) return;

    SDL_BindGPUGraphicsPipeline(render_pass, mesh_pipeline);

    for (i = 0; i < gs->dl.mesh_count; i++) {
        mesh_draw_command *mc = &gs->dl.meshes[i];
        scene_model_asset *asset;
        Mat4 model;
        mesh_uniform_data mesh_uniforms;
        SDL_GPUBuffer *bones_to_bind;
        uint32_t p;

        asset = scene_asset_for_index(gs, mc->model_asset_index);
        if (!asset || !asset->loaded || asset->model.mesh.primitive_count == 0) continue;

        memcpy(model.m, mc->model, sizeof(float) * 16);

        memcpy(mesh_uniforms.projection, projection.m, sizeof(float) * 16);
        memcpy(mesh_uniforms.view, view.m, sizeof(float) * 16);
        memcpy(mesh_uniforms.model, model.m, sizeof(float) * 16);
        mesh_uniforms.bone_offset = 0;

        bones_to_bind = bone_identity_buffer;
        if (asset->has_skeleton &&
            mc->use_skinned_bones &&
            bone_storage_buffer) {
            bones_to_bind = bone_storage_buffer;
            mesh_uniforms.bone_offset = (uint32_t)mc->bone_buffer_offset;
        }
        SDL_PushGPUVertexUniformData(cmd_buf, 0, &mesh_uniforms, sizeof(mesh_uniforms));
        SDL_BindGPUVertexStorageBuffers(render_pass, 0, &bones_to_bind, 1);

        for (p = 0; p < asset->model.mesh.primitive_count; p++) {
            GltfPrimitive *prim = &asset->model.mesh.primitives[p];
            SDL_GPUBufferBinding vbuf_binding = {0};
            SDL_GPUBufferBinding ibuf_binding = {0};
            SDL_GPUTextureSamplerBinding tex_bind = {0};

            vbuf_binding.buffer = (SDL_GPUBuffer *)prim->vertex_buffer;
            SDL_BindGPUVertexBuffers(render_pass, 0, &vbuf_binding, 1);

            ibuf_binding.buffer = (SDL_GPUBuffer *)prim->index_buffer;
            SDL_BindGPUIndexBuffer(render_pass, &ibuf_binding, SDL_GPU_INDEXELEMENTSIZE_16BIT);

            tex_bind.texture = prim->texture ? (SDL_GPUTexture *)prim->texture : white_texture;
            tex_bind.sampler = mesh_sampler;
            SDL_BindGPUFragmentSamplers(render_pass, 0, &tex_bind, 1);

            SDL_DrawGPUIndexedPrimitives(render_pass, prim->index_count, 1, 0, 0, 0);
        }
    }
}

// ---------------------------------------------------------------------------
// Font data (MSDF atlas)
// ---------------------------------------------------------------------------

#define BOOTSTRAP_ICON_FONT_PATH "assets/fonts/bootstrap-icons.ttf"
#define BI_ICON_PLAY_FILL        0xF4F4u
#define BI_ICON_STOP_FILL        0xF592u
#define BI_ICON_CAMERA           0xF220u
#define BI_ICON_BOUNDING_BOX     0xF1B6u
#define BI_ICON_ARROW_LEFT       0xF12Fu
#define BI_ICON_ARROW_DOWN       0xF128u
#define BI_ICON_ARROW_RIGHT      0xF138u
#define BI_ICON_ARROW_UP         0xF148u
#define ICON_GLYPH_MAX           16

typedef struct icon_glyph_entry {
    uint32_t cp;
    msdf_glyph glyph;
} icon_glyph_entry;

static stbtt_fontinfo font_stb_info;
static unsigned char *font_ttf_buffer = NULL;
static size_t font_ttf_size = 0;
static msdf_glyph font_glyphs[128];
static float font_ascent, font_descent, font_line_gap;

static stbtt_fontinfo icon_stb_info;
static unsigned char *icon_ttf_buffer = NULL;
static size_t icon_ttf_size = 0;
static icon_glyph_entry icon_glyphs[ICON_GLYPH_MAX];
static int icon_glyph_count = 0;
static float icon_font_ascent, icon_font_descent, icon_font_line_gap;

// ---------------------------------------------------------------------------
// Helper: compile HLSL -> SPIRV -> SDL_GPUShader
// ---------------------------------------------------------------------------

static void print_shader_stage_name(SDL_ShaderCross_ShaderStage stage, char* buf, size_t bufsize) {
    switch(stage) {
        case SDL_SHADERCROSS_SHADERSTAGE_VERTEX:   snprintf(buf, bufsize, "Vertex"); break;
        case SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT: snprintf(buf, bufsize, "Fragment"); break;
        case SDL_SHADERCROSS_SHADERSTAGE_COMPUTE:  snprintf(buf, bufsize, "Compute"); break;
        default: snprintf(buf, bufsize, "Unknown(%d)", (int)stage);
    }
}

static SDL_GPUShader* load_shader_from_spirv(
    const char* filename,
    const char* entrypoint,
    SDL_ShaderCross_ShaderStage stage)
{
    // Read pre-compiled SPIR-V from disk
    size_t spirv_size = 0;
    void* spirv_bytecode = SDL_LoadFile(filename, &spirv_size);
    if (!spirv_bytecode) {
        fprintf(stderr, "ERROR: Failed to load SPIR-V file: %s\n", filename);
        fprintf(stderr, "       SDL Error: %s\n", SDL_GetError());
        return NULL;
    }
    
    printf("INFO: Loaded SPIR-V file: %s (%zu bytes)\n", filename, spirv_size);

    // Reflect to get resource info
    SDL_ShaderCross_GraphicsShaderMetadata* metadata =
        SDL_ShaderCross_ReflectGraphicsSPIRV((const Uint8*)spirv_bytecode, spirv_size, 0);
    if (!metadata) {
        fprintf(stderr, "ERROR: Failed to reflect SPIR-V shader: %s\n", filename);
        fprintf(stderr, "       SDL Error: %s\n", SDL_GetError());
        SDL_free(spirv_bytecode);
        return NULL;
    }
    
    printf("INFO: Reflected shader resources for: %s\n", filename);

    // Compile SPIRV -> GPU shader
    SDL_ShaderCross_SPIRV_Info spirv_info = {0};
    spirv_info.bytecode = (const Uint8*)spirv_bytecode;
    spirv_info.bytecode_size = spirv_size;
    spirv_info.entrypoint = entrypoint;
    spirv_info.shader_stage = stage;
    spirv_info.props = 0;

    SDL_GPUShader* shader = SDL_ShaderCross_CompileGraphicsShaderFromSPIRV(
        gpu_device, &spirv_info, &metadata->resource_info, 0);

    // Cleanup before checking result (in case shader fails to compile)
    SDL_free(metadata);
    SDL_free(spirv_bytecode);
    
    if (!shader) {
        char stage_name[32];
        print_shader_stage_name(stage, stage_name, sizeof(stage_name));
        
        fprintf(stderr, "ERROR: Failed to compile GPU shader from SPIR-V: %s\n", filename);
        fprintf(stderr, "       Stage: %s\n", stage_name);
        fprintf(stderr, "       Entry point: %s\n", entrypoint);
        
        // Try to get more specific error info
        const char* sdl_error = SDL_GetError();
        if (sdl_error && strlen(sdl_error) > 0) {
            fprintf(stderr, "       Error details: %s\n", sdl_error);
        }
        
        return NULL;
    }

    printf("INFO: Successfully compiled shader: %s\n", filename);
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
    SDL_GPUTextureCreateInfo tex_info = {0};
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
    SDL_GPUTransferBufferCreateInfo tbuf_info = {0};
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
// Upload via copy pass
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu_device);
    if (!cmd) {
      fprintf(stderr, "Failed to acquire GPU command buffer for texture upload\n");
      SDL_ReleaseGPUTransferBuffer(gpu_device, transfer_buf);
      SDL_ReleaseGPUTexture(gpu_device, texture);
      stbi_image_free(data);
      return NULL;
    }
    
    SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src = {0};
    src.transfer_buffer = transfer_buf;
    src.offset = 0;
    src.pixels_per_row = 0;
    src.rows_per_layer = 0;

    SDL_GPUTextureRegion dst = {0};
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
    
    // Always release transfer buffer even if command submission fails
    SDL_ReleaseGPUTransferBuffer(gpu_device, transfer_buf);
    
    SDL_EndGPUCopyPass(copy_pass);
    if (!SDL_SubmitGPUCommandBuffer(cmd)) {
        fprintf(stderr, "Failed to submit GPU command buffer for texture upload: %s\n", filepath);
        SDL_ReleaseGPUTexture(gpu_device, texture);
        return NULL;
    }

    printf("Loaded texture: %s (%dx%d)\n", filepath, w, h);
    return texture;
}

// ---------------------------------------------------------------------------
// Font loading (MSDF atlas)
// ---------------------------------------------------------------------------

static int font_load_msdf(const char *path) {
    size_t size = 0;
    font_ttf_buffer = (unsigned char*)SDL_LoadFile(path, &size);
    if (!font_ttf_buffer) {
        fprintf(stderr, "Failed to load font: %s\n", path);
        return -1;
    }
    font_ttf_size = size;

    if (!stbtt_InitFont(&font_stb_info, font_ttf_buffer, 0)) {
        fprintf(stderr, "stbtt_InitFont failed for %s\n", path);
        return -1;
    }

    msdf_atlas atlas;
    if (msdf_build_atlas(&font_stb_info, font_glyphs, 32, 127, &atlas,
                          &font_ascent, &font_descent, &font_line_gap) != 0) {
        fprintf(stderr, "Failed to build MSDF atlas\n");
        return -1;
    }

    /* Upload atlas to GPU texture */
    {
        SDL_GPUTextureCreateInfo tex_info = {0};
        tex_info.type = SDL_GPU_TEXTURETYPE_2D;
        tex_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        tex_info.width = (Uint32)atlas.width;
        tex_info.height = (Uint32)atlas.height;
        tex_info.layer_count_or_depth = 1;
        tex_info.num_levels = 1;
        tex_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        font_atlas_texture = SDL_CreateGPUTexture(gpu_device, &tex_info);
        if (!font_atlas_texture) {
            fprintf(stderr, "Failed to create font atlas texture: %s\n", SDL_GetError());
            msdf_atlas_free(&atlas);
            return -1;
        }

        Uint32 pixel_size = (Uint32)(atlas.width * atlas.height * 4);
        SDL_GPUTransferBufferCreateInfo tbi = {0};
        tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbi.size = pixel_size;
        SDL_GPUTransferBuffer *xfer = SDL_CreateGPUTransferBuffer(gpu_device, &tbi);
        void *mapped = SDL_MapGPUTransferBuffer(gpu_device, xfer, false);
        memcpy(mapped, atlas.pixels, pixel_size);
        SDL_UnmapGPUTransferBuffer(gpu_device, xfer);

        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu_device);
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);

        SDL_GPUTextureTransferInfo src = {0};
        src.transfer_buffer = xfer;
        src.pixels_per_row = (Uint32)atlas.width;
        src.rows_per_layer = (Uint32)atlas.height;

        SDL_GPUTextureRegion dst = {0};
        dst.texture = font_atlas_texture;
        dst.w = (Uint32)atlas.width;
        dst.h = (Uint32)atlas.height;
        dst.d = 1;

        SDL_UploadToGPUTexture(copy, &src, &dst, false);
        SDL_EndGPUCopyPass(copy);
        SDL_SubmitGPUCommandBuffer(cmd);
        SDL_ReleaseGPUTransferBuffer(gpu_device, xfer);
    }

    /* Create sampler for MSDF atlas (linear filtering is essential for SDF) */
    {
        SDL_GPUSamplerCreateInfo samp_info = {0};
        samp_info.min_filter = SDL_GPU_FILTER_LINEAR;
        samp_info.mag_filter = SDL_GPU_FILTER_LINEAR;
        samp_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        samp_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        samp_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        samp_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        font_atlas_sampler = SDL_CreateGPUSampler(gpu_device, &samp_info);
        if (!font_atlas_sampler) {
            fprintf(stderr, "Failed to create font atlas sampler: %s\n", SDL_GetError());
            msdf_atlas_free(&atlas);
            return -1;
        }
    }

    msdf_atlas_free(&atlas);
    return 0;
}

static const msdf_glyph *icon_glyph_lookup(uint32_t cp) {
    int i;
    for (i = 0; i < icon_glyph_count; i++) {
        if (icon_glyphs[i].cp == cp) {
            return &icon_glyphs[i].glyph;
        }
    }
    return NULL;
}

static float msdf_tile_median_sample(const unsigned char *tile, int tw, int th, int x, int y);
static int msdf_tile_looks_inverted(const unsigned char *tile, int tw, int th);
static void msdf_tile_invert(unsigned char *tile, int tw, int th);

static int msdf_build_atlas_for_list(stbtt_fontinfo *info,
                                     const uint32_t *codepoints,
                                     int codepoint_count,
                                     icon_glyph_entry *out_glyphs,
                                     int out_cap,
                                     int *out_count,
                                     msdf_atlas *atlas,
                                     float *out_ascent,
                                     float *out_descent,
                                     float *out_line_gap) {
    float scale;
    float inv_source;
    int i;

    if (!info || !codepoints || codepoint_count <= 0 || !out_glyphs || out_cap <= 0 ||
        !out_count || !atlas || !out_ascent || !out_descent || !out_line_gap) {
        return -1;
    }

    scale = stbtt_ScaleForPixelHeight(info, (float)MSDF_SOURCE_SIZE);
    inv_source = 1.0f / (float)MSDF_SOURCE_SIZE;

    {
        int asc, desc, lg;
        float unit_scale;
        stbtt_GetFontVMetrics(info, &asc, &desc, &lg);
        unit_scale = stbtt_ScaleForPixelHeight(info, 1.0f);
        *out_ascent = (float)asc * unit_scale;
        *out_descent = (float)desc * unit_scale;
        *out_line_gap = (float)lg * unit_scale;
    }

    memset(out_glyphs, 0, (size_t)out_cap * sizeof(*out_glyphs));
    *out_count = 0;
    msdf_atlas_init(atlas);

    for (i = 0; i < codepoint_count; i++) {
        uint32_t cp = codepoints[i];
        int glyph_idx = stbtt_FindGlyphIndex(info, (int)cp);
        msdf_glyph g;
        int ix0, iy0, ix1, iy1;
        int bw, bh;
        int tw, th;
        int ax, ay;

        memset(&g, 0, sizeof(g));

        {
            int adv, lsb;
            float unit_scale = stbtt_ScaleForPixelHeight(info, 1.0f);
            stbtt_GetGlyphHMetrics(info, glyph_idx, &adv, &lsb);
            g.advance = (float)adv * unit_scale;
        }

        stbtt_GetGlyphBitmapBox(info, glyph_idx, scale, scale, &ix0, &iy0, &ix1, &iy1);
        bw = ix1 - ix0;
        bh = iy1 - iy0;

        if (bw <= 0 || bh <= 0) {
            if (*out_count < out_cap) {
                out_glyphs[*out_count].cp = cp;
                out_glyphs[*out_count].glyph = g;
                (*out_count)++;
            }
            continue;
        }

        tw = bw + 2 * MSDF_RECT_MARGIN;
        th = bh + 2 * MSDF_RECT_MARGIN;
        g.xoff = (float)(ix0 - MSDF_RECT_MARGIN) * inv_source;
        g.yoff = (float)(iy0 - MSDF_RECT_MARGIN) * inv_source;
        g.width = tw;
        g.height = th;

        {
            msdf_shape shape;
            unsigned char *tile;
            int py, px;

            msdf_extract_shape(&shape, info, glyph_idx, scale);
            msdf_color_edges(&shape);

            tile = (unsigned char *)calloc((size_t)(tw * th * 4), 1);
            if (!tile) {
                msdf_shape_free(&shape);
                msdf_atlas_free(atlas);
                return -1;
            }

            for (py = 0; py < th; py++) {
                for (px = 0; px < tw; px++) {
                    float outline_x = (float)(ix0 + px - MSDF_RECT_MARGIN);
                    float outline_y = -(float)(iy0 + py - MSDF_RECT_MARGIN);
                    msdf_signed_distance sd_r = {-DBL_MAX, 0};
                    msdf_signed_distance sd_g = {-DBL_MAX, 0};
                    msdf_signed_distance sd_b = {-DBL_MAX, 0};
                    int cii, eii;
                    unsigned char *px_out;
                    float r;
                    float gg;
                    float b;
                    float a;

                    for (cii = 0; cii < shape.contour_count; cii++) {
                        const msdf_contour *c = &shape.contours[cii];
                        for (eii = 0; eii < c->edge_count; eii++) {
                            const msdf_edge *e = &c->edges[eii];
                            msdf_signed_distance sd = msdf_edge_sd(e, (double)outline_x, (double)outline_y);

                            if ((e->color & MSDF_RED)   && msdf_sd_lt(sd, sd_r)) sd_r = sd;
                            if ((e->color & MSDF_GREEN) && msdf_sd_lt(sd, sd_g)) sd_g = sd;
                            if ((e->color & MSDF_BLUE)  && msdf_sd_lt(sd, sd_b)) sd_b = sd;
                        }
                    }

                    r = 0.5f + (float)(sd_r.distance / MSDF_PIXEL_RANGE);
                    gg = 0.5f + (float)(sd_g.distance / MSDF_PIXEL_RANGE);
                    b = 0.5f + (float)(sd_b.distance / MSDF_PIXEL_RANGE);
                    a = msdf_medianf(r, gg, b);

                    px_out = tile + (py * tw + px) * 4;
                    px_out[0] = (unsigned char)(msdf_clampf(r, 0, 1) * 255.0f + 0.5f);
                    px_out[1] = (unsigned char)(msdf_clampf(gg, 0, 1) * 255.0f + 0.5f);
                    px_out[2] = (unsigned char)(msdf_clampf(b, 0, 1) * 255.0f + 0.5f);
                    px_out[3] = (unsigned char)(msdf_clampf(a, 0, 1) * 255.0f + 0.5f);
                }
            }

            msdf_error_correct_tile(tile, tw, th, &shape, ix0, iy0);
            if (msdf_tile_looks_inverted(tile, tw, th)) {
                msdf_tile_invert(tile, tw, th);
            }

            if (msdf_atlas_pack(atlas, tile, tw, th, &ax, &ay) != 0) {
                free(tile);
                msdf_shape_free(&shape);
                msdf_atlas_free(atlas);
                return -1;
            }

            g.atlas_x = ax;
            g.atlas_y = ay;
            g.u0 = (float)ax / (float)atlas->width;
            g.v0 = (float)ay / (float)atlas->height;
            g.u1 = (float)(ax + tw) / (float)atlas->width;
            g.v1 = (float)(ay + th) / (float)atlas->height;

            free(tile);
            msdf_shape_free(&shape);
        }

        if (*out_count < out_cap) {
            out_glyphs[*out_count].cp = cp;
            out_glyphs[*out_count].glyph = g;
            (*out_count)++;
        }
    }

    return 0;
}

static float msdf_tile_median_sample(const unsigned char *tile, int tw, int th, int x, int y) {
    const unsigned char *p;
    float r, g, b;
    if (!tile || tw <= 0 || th <= 0) return 0.0f;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= tw) x = tw - 1;
    if (y >= th) y = th - 1;
    p = tile + ((y * tw + x) * 4);
    r = (float)p[0] / 255.0f;
    g = (float)p[1] / 255.0f;
    b = (float)p[2] / 255.0f;
    return msdf_medianf(r, g, b);
}

static int msdf_tile_looks_inverted(const unsigned char *tile, int tw, int th) {
    int x0 = 0;
    int y0 = 0;
    int x1 = tw - 1;
    int y1 = th - 1;
    int xm = tw / 2;
    int ym = th / 2;
    float avg;
    if (!tile || tw < 2 || th < 2) return 0;

    avg =
        msdf_tile_median_sample(tile, tw, th, x0, y0) +
        msdf_tile_median_sample(tile, tw, th, x1, y0) +
        msdf_tile_median_sample(tile, tw, th, x0, y1) +
        msdf_tile_median_sample(tile, tw, th, x1, y1) +
        msdf_tile_median_sample(tile, tw, th, xm, y0) +
        msdf_tile_median_sample(tile, tw, th, xm, y1) +
        msdf_tile_median_sample(tile, tw, th, x0, ym) +
        msdf_tile_median_sample(tile, tw, th, x1, ym);
    avg /= 8.0f;

    /* Glyph margins are outside the shape and should be < 0.5 for normal SDF.
       If background samples trend above 0.5, inside/outside is inverted. */
    return avg > 0.5f;
}

static void msdf_tile_invert(unsigned char *tile, int tw, int th) {
    int i;
    int total = tw * th;
    if (!tile || total <= 0) return;
    for (i = 0; i < total; i++) {
        unsigned char *p = tile + i * 4;
        p[0] = (unsigned char)(255 - p[0]);
        p[1] = (unsigned char)(255 - p[1]);
        p[2] = (unsigned char)(255 - p[2]);
        p[3] = (unsigned char)(255 - p[3]);
    }
}

static int icon_font_load_msdf(const char *path) {
    static const uint32_t icon_codepoints[] = {
        BI_ICON_PLAY_FILL,
        BI_ICON_STOP_FILL,
        BI_ICON_CAMERA,
        BI_ICON_BOUNDING_BOX,
        BI_ICON_ARROW_LEFT,
        BI_ICON_ARROW_DOWN,
        BI_ICON_ARROW_RIGHT,
        BI_ICON_ARROW_UP
    };
    size_t size = 0;
    msdf_atlas atlas;

    icon_ttf_buffer = (unsigned char*)SDL_LoadFile(path, &size);
    if (!icon_ttf_buffer) {
        fprintf(stderr, "Failed to load icon font: %s\n", path);
        return -1;
    }
    icon_ttf_size = size;

    if (!stbtt_InitFont(&icon_stb_info, icon_ttf_buffer, 0)) {
        fprintf(stderr, "stbtt_InitFont failed for icon font: %s\n", path);
        return -1;
    }

    if (msdf_build_atlas_for_list(&icon_stb_info,
                                  icon_codepoints,
                                  (int)(sizeof(icon_codepoints) / sizeof(icon_codepoints[0])),
                                  icon_glyphs,
                                  ICON_GLYPH_MAX,
                                  &icon_glyph_count,
                                  &atlas,
                                  &icon_font_ascent,
                                  &icon_font_descent,
                                  &icon_font_line_gap) != 0) {
        fprintf(stderr, "Failed to build icon MSDF atlas\n");
        return -1;
    }

    {
        SDL_GPUTextureCreateInfo tex_info = {0};
        Uint32 pixel_size;
        SDL_GPUTransferBufferCreateInfo tbi;
        SDL_GPUTransferBuffer *xfer;
        void *mapped;
        SDL_GPUCommandBuffer *cmd;
        SDL_GPUCopyPass *copy;
        SDL_GPUTextureTransferInfo src;
        SDL_GPUTextureRegion dst;
        SDL_GPUSamplerCreateInfo samp_info;

        tex_info.type = SDL_GPU_TEXTURETYPE_2D;
        tex_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        tex_info.width = (Uint32)atlas.width;
        tex_info.height = (Uint32)atlas.height;
        tex_info.layer_count_or_depth = 1;
        tex_info.num_levels = 1;
        tex_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        icon_atlas_texture = SDL_CreateGPUTexture(gpu_device, &tex_info);
        if (!icon_atlas_texture) {
            fprintf(stderr, "Failed to create icon atlas texture: %s\n", SDL_GetError());
            msdf_atlas_free(&atlas);
            return -1;
        }

        pixel_size = (Uint32)(atlas.width * atlas.height * 4);
        memset(&tbi, 0, sizeof(tbi));
        tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbi.size = pixel_size;
        xfer = SDL_CreateGPUTransferBuffer(gpu_device, &tbi);
        mapped = SDL_MapGPUTransferBuffer(gpu_device, xfer, false);
        memcpy(mapped, atlas.pixels, pixel_size);
        SDL_UnmapGPUTransferBuffer(gpu_device, xfer);

        cmd = SDL_AcquireGPUCommandBuffer(gpu_device);
        copy = SDL_BeginGPUCopyPass(cmd);

        memset(&src, 0, sizeof(src));
        src.transfer_buffer = xfer;
        src.pixels_per_row = (Uint32)atlas.width;
        src.rows_per_layer = (Uint32)atlas.height;

        memset(&dst, 0, sizeof(dst));
        dst.texture = icon_atlas_texture;
        dst.w = (Uint32)atlas.width;
        dst.h = (Uint32)atlas.height;
        dst.d = 1;

        SDL_UploadToGPUTexture(copy, &src, &dst, false);
        SDL_EndGPUCopyPass(copy);
        SDL_SubmitGPUCommandBuffer(cmd);
        SDL_ReleaseGPUTransferBuffer(gpu_device, xfer);

        memset(&samp_info, 0, sizeof(samp_info));
        samp_info.min_filter = SDL_GPU_FILTER_LINEAR;
        samp_info.mag_filter = SDL_GPU_FILTER_LINEAR;
        samp_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        samp_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        samp_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        samp_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        icon_atlas_sampler = SDL_CreateGPUSampler(gpu_device, &samp_info);
        if (!icon_atlas_sampler) {
            fprintf(stderr, "Failed to create icon atlas sampler: %s\n", SDL_GetError());
            msdf_atlas_free(&atlas);
            return -1;
        }
    }

    msdf_atlas_free(&atlas);
    if (icon_glyph_count <= 0) {
        fprintf(stderr, "Icon font loaded but no supported glyphs were generated.\n");
        return -1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// HarfBuzz init and text measurement
// ---------------------------------------------------------------------------

static unsigned int hb_scale = 0;  // units per em used for HarfBuzz

static int harfbuzz_init(void) {
    hb_editor_blob = hb_blob_create((const char*)font_ttf_buffer,
        (unsigned int)font_ttf_size, HB_MEMORY_MODE_READONLY, NULL, NULL);
    hb_editor_face = hb_face_create(hb_editor_blob, 0);
    hb_editor_font = hb_font_create(hb_editor_face);
    hb_ot_font_set_funcs(hb_editor_font);
    hb_scale = hb_face_get_upem(hb_editor_face);
    hb_font_set_scale(hb_editor_font, (int)hb_scale, (int)hb_scale);
    printf("HarfBuzz initialized (upem=%u)\n", hb_scale);
    return 0;
}

static float font_measure_width(const char *text, uint32_t len, float font_size) {
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

static int utf8_decode_at(const char *text, uint32_t len, uint32_t index,
                          uint32_t *out_cp, uint32_t *out_next_index) {
    unsigned char c0;
    uint32_t cp;
    uint32_t next;

    if (!text || index >= len || !out_cp) return 0;

    c0 = (unsigned char)text[index];
    cp = c0;
    next = index + 1;

    if ((c0 & 0x80u) == 0) {
        /* ASCII */
    } else if ((c0 & 0xE0u) == 0xC0u && index + 1 < len) {
        cp = ((uint32_t)(c0 & 0x1Fu) << 6) |
             (uint32_t)(text[index + 1] & 0x3Fu);
        next = index + 2;
    } else if ((c0 & 0xF0u) == 0xE0u && index + 2 < len) {
        cp = ((uint32_t)(c0 & 0x0Fu) << 12) |
             ((uint32_t)(text[index + 1] & 0x3Fu) << 6) |
             (uint32_t)(text[index + 2] & 0x3Fu);
        next = index + 3;
    } else if ((c0 & 0xF8u) == 0xF0u && index + 3 < len) {
        cp = ((uint32_t)(c0 & 0x07u) << 18) |
             ((uint32_t)(text[index + 1] & 0x3Fu) << 12) |
             ((uint32_t)(text[index + 2] & 0x3Fu) << 6) |
             (uint32_t)(text[index + 3] & 0x3Fu);
        next = index + 4;
    }

    *out_cp = cp;
    if (out_next_index) *out_next_index = next;
    return 1;
}

static int text_contains_icon_cp(const char *text, uint32_t len) {
    uint32_t idx = 0;
    while (idx < len) {
        uint32_t cp = 0;
        uint32_t next_idx = idx + 1;
        if (!utf8_decode_at(text, len, idx, &cp, &next_idx)) break;
        if (icon_glyph_lookup(cp)) return 1;
        if (next_idx <= idx) break;
        idx = next_idx;
    }
    return 0;
}

static float icon_inline_y_nudge(uint32_t cp, float font_size) {
    /* Visual baseline tuning: Bootstrap media icons sit slightly high next to text. */
    (void)font_size;
    switch (cp) {
        case BI_ICON_PLAY_FILL: return 0.0f;
        case BI_ICON_STOP_FILL: return 0.0f;
        default: return 0.0f;
    }
}

// ---------------------------------------------------------------------------
// Pre-render tab header text to GPU texture (CPU-side rasterization)
// ---------------------------------------------------------------------------

static SDL_GPUTexture *create_header_texture(const char *text, int *out_w) {
    int header_w = (int)(1600 * display_density);
    int header_h = (int)(DOCK_HEADER_HEIGHT * display_density);
    float font_size = 14.0f * display_density;
    float scale = stbtt_ScaleForPixelHeight(&font_stb_info, font_size);
    int ascent_i, descent_i, line_gap_i;
    int x_cursor, ch, i;
    unsigned char *pixels;
    SDL_GPUTexture *tex;
    SDL_GPUTransferBufferCreateInfo tbi;
    SDL_GPUTransferBuffer *transfer;
    SDL_GPUCopyPass *copy_pass;
    SDL_GPUCommandBuffer *cmd;
    SDL_GPUTextureCreateInfo tex_info;
    SDL_GPUTextureRegion tex_region;
    SDL_GPUTextureTransferInfo transfer_info;
    void *mapped;

    stbtt_GetFontVMetrics(&font_stb_info, &ascent_i, &descent_i, &line_gap_i);

    pixels = (unsigned char *)calloc((size_t)(header_w * header_h * 4), 1);
    if (!pixels) return NULL;

    /* Fill with header background color */
    for (i = 0; i < header_w * header_h; i++) {
        pixels[i * 4 + 0] = 38;
        pixels[i * 4 + 1] = 38;
        pixels[i * 4 + 2] = 46;
        pixels[i * 4 + 3] = 255;
    }

    /* Rasterize each character and composite onto the header */
    x_cursor = (int)(8 * display_density); /* left padding */
    {
        int baseline = (int)(ascent_i * scale) + (header_h - (int)(font_size)) / 2;
        const char *p = text;
        while ((ch = (unsigned char)*p++) != 0) {
            int advance, lsb, bw, bh, x0, y0, gx, gy;
            unsigned char *bitmap;

            stbtt_GetCodepointHMetrics(&font_stb_info, ch, &advance, &lsb);
            bitmap = stbtt_GetCodepointBitmap(&font_stb_info, 0, scale, ch, &bw, &bh, &x0, &y0);

            if (bitmap) {
                for (gy = 0; gy < bh; gy++) {
                    for (gx = 0; gx < bw; gx++) {
                        int px = x_cursor + x0 + gx;
                        int py = baseline + y0 + gy;
                        if (px >= 0 && px < header_w && py >= 0 && py < header_h) {
                            int idx = (py * header_w + px) * 4;
                            unsigned char a = bitmap[gy * bw + gx];
                            /* Blend white text onto dark background */
                            pixels[idx + 0] = (unsigned char)(38 + (220 - 38) * a / 255);
                            pixels[idx + 1] = (unsigned char)(38 + (220 - 38) * a / 255);
                            pixels[idx + 2] = (unsigned char)(46 + (220 - 46) * a / 255);
                        }
                    }
                }
                stbtt_FreeBitmap(bitmap, NULL);
            }

            x_cursor += (int)(advance * scale);
            if (*p) {
                x_cursor += (int)(stbtt_GetCodepointKernAdvance(&font_stb_info, ch, (unsigned char)*p) * scale);
            }
        }
    }

    /* Create GPU texture */
    memset(&tex_info, 0, sizeof(tex_info));
    tex_info.type = SDL_GPU_TEXTURETYPE_2D;
    tex_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    tex_info.width = (Uint32)header_w;
    tex_info.height = (Uint32)header_h;
    tex_info.layer_count_or_depth = 1;
    tex_info.num_levels = 1;
    tex_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tex = SDL_CreateGPUTexture(gpu_device, &tex_info);
    if (!tex) { free(pixels); return NULL; }

    /* Upload via transfer buffer */
    memset(&tbi, 0, sizeof(tbi));
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbi.size = (Uint32)(header_w * header_h * 4);
    transfer = SDL_CreateGPUTransferBuffer(gpu_device, &tbi);
    mapped = SDL_MapGPUTransferBuffer(gpu_device, transfer, false);
    memcpy(mapped, pixels, (size_t)(header_w * header_h * 4));
    SDL_UnmapGPUTransferBuffer(gpu_device, transfer);

    cmd = SDL_AcquireGPUCommandBuffer(gpu_device);
    copy_pass = SDL_BeginGPUCopyPass(cmd);

    memset(&transfer_info, 0, sizeof(transfer_info));
    transfer_info.transfer_buffer = transfer;
    transfer_info.rows_per_layer = (Uint32)header_h;
    transfer_info.pixels_per_row = (Uint32)header_w;

    memset(&tex_region, 0, sizeof(tex_region));
    tex_region.texture = tex;
    tex_region.w = (Uint32)header_w;
    tex_region.h = (Uint32)header_h;
    tex_region.d = 1;

    SDL_UploadToGPUTexture(copy_pass, &transfer_info, &tex_region, false);
    SDL_EndGPUCopyPass(copy_pass);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(gpu_device, transfer);

    free(pixels);
    *out_w = (int)((x_cursor + 8) / display_density); /* text width + padding, in logical pixels */
    return tex;
}

// ---------------------------------------------------------------------------
// Clay text measurement callback
// ---------------------------------------------------------------------------

static Clay_Dimensions clay_measure_text(Clay_StringSlice text, Clay_TextElementConfig *config, void *userData) {
    (void)userData;
    float width = font_measure_width(text.chars, text.length, (float)config->fontSize);
    // Ceil to account for pixel snapping in the renderer (prevents glyph clipping)
    float height = ceilf((float)config->fontSize * (font_ascent - font_descent + font_line_gap));
    return (Clay_Dimensions){ceilf(width + 1.0f), height};
}

// ---------------------------------------------------------------------------
// UI rendering: process Clay render commands -> GPU draw calls
// ---------------------------------------------------------------------------

#define MAX_UI_RECT_VERTICES  (4096 * 6)
#define MAX_FONT_VERTICES     (4096 * 6)

// Per-window UI render state
typedef struct UIRenderState {
    ui_rect_vertex rect_verts[MAX_UI_RECT_VERTICES];
    font_vertex    font_verts[MAX_FONT_VERTICES];
    font_vertex    icon_font_verts[MAX_FONT_VERTICES];
    int            rect_vert_count;
    int            font_vert_count;
    int            icon_font_vert_count;
    SDL_GPUBuffer *rect_gpu_buf;
    SDL_GPUBuffer *font_gpu_buf;
    SDL_GPUBuffer *icon_font_gpu_buf;
} UIRenderState;

static UIRenderState ui_game = {0};
static UIRenderState ui_profiler = {0};
static UIRenderState ui_scene_tree = {0};
static UIRenderState ui_project_browser = {0};
static UIRenderState ui_inspector = {0};
static UIRenderState ui_menu_bar = {0};
static UIRenderState ui_editor_toolbar = {0};
static UIRenderState ui_cache_profiler = {0};
static UIRenderState ui_cpu_profiler = {0};

static void ui_push_font_quad(UIRenderState *ui, int use_icon_font,
                              float gx, float gy, float glyph_w, float glyph_h,
                              const msdf_glyph *mg,
                              float r, float g, float b, float a) {
    font_vertex *verts;
    int *vert_count;

    if (!ui || !mg) return;

    if (use_icon_font) {
        verts = ui->icon_font_verts;
        vert_count = &ui->icon_font_vert_count;
    } else {
        verts = ui->font_verts;
        vert_count = &ui->font_vert_count;
    }

    if (*vert_count + 6 > MAX_FONT_VERTICES) return;

    {
        font_vertex *v = &verts[*vert_count];
        v[0] = (font_vertex){gx,            gy,            mg->u0, mg->v0, r, g, b, a};
        v[1] = (font_vertex){gx + glyph_w,  gy,            mg->u1, mg->v0, r, g, b, a};
        v[2] = (font_vertex){gx + glyph_w,  gy + glyph_h,  mg->u1, mg->v1, r, g, b, a};
        v[3] = (font_vertex){gx,            gy,            mg->u0, mg->v0, r, g, b, a};
        v[4] = (font_vertex){gx + glyph_w,  gy + glyph_h,  mg->u1, mg->v1, r, g, b, a};
        v[5] = (font_vertex){gx,            gy + glyph_h,  mg->u0, mg->v1, r, g, b, a};
        *vert_count += 6;
    }
}

// ---------------------------------------------------------------------------
// Build Clay render commands → vertex arrays (window-agnostic)
// ---------------------------------------------------------------------------
static void ui_build_vertices(UIRenderState *ui, Clay_RenderCommandArray commands) {
    /* CPU-side scissor clipping with proper nesting (push on START, pop on END). */
    typedef struct ClipRect { float x0, y0, x1, y1; } ClipRect;
    enum { UI_CLIP_STACK_MAX = 32 };
    ClipRect clip_stack[UI_CLIP_STACK_MAX];
    int clip_depth = 0;
    int clipping = 0;
    float clip_x0 = 0, clip_y0 = 0, clip_x1 = 99999, clip_y1 = 99999;

    ui->rect_vert_count = 0;
    ui->font_vert_count = 0;
    ui->icon_font_vert_count = 0;

    for (int32_t i = 0; i < commands.length; i++) {
        Clay_RenderCommand *cmd = Clay_RenderCommandArray_Get(&commands, i);
        Clay_BoundingBox box = cmd->boundingBox;

        switch (cmd->commandType) {
        case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START: {
            ClipRect next = { box.x, box.y, box.x + box.width, box.y + box.height };
            if (clip_depth > 0) {
                ClipRect parent = clip_stack[clip_depth - 1];
                if (next.x0 < parent.x0) next.x0 = parent.x0;
                if (next.y0 < parent.y0) next.y0 = parent.y0;
                if (next.x1 > parent.x1) next.x1 = parent.x1;
                if (next.y1 > parent.y1) next.y1 = parent.y1;
            }
            if (clip_depth < UI_CLIP_STACK_MAX) {
                clip_stack[clip_depth++] = next;
            } else {
                clip_stack[UI_CLIP_STACK_MAX - 1] = next;
                clip_depth = UI_CLIP_STACK_MAX;
            }
            clipping = 1;
            clip_x0 = clip_stack[clip_depth - 1].x0;
            clip_y0 = clip_stack[clip_depth - 1].y0;
            clip_x1 = clip_stack[clip_depth - 1].x1;
            clip_y1 = clip_stack[clip_depth - 1].y1;
            break;
        }
        case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END: {
            if (clip_depth > 0) clip_depth--;
            if (clip_depth > 0) {
                clipping = 1;
                clip_x0 = clip_stack[clip_depth - 1].x0;
                clip_y0 = clip_stack[clip_depth - 1].y0;
                clip_x1 = clip_stack[clip_depth - 1].x1;
                clip_y1 = clip_stack[clip_depth - 1].y1;
            } else {
                clipping = 0;
                clip_x0 = 0; clip_y0 = 0; clip_x1 = 99999; clip_y1 = 99999;
            }
            break;
        }
        case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
            Clay_RectangleRenderData *rect = &cmd->renderData.rectangle;
            float r = rect->backgroundColor.r / 255.0f;
            float g = rect->backgroundColor.g / 255.0f;
            float b = rect->backgroundColor.b / 255.0f;
            float a = rect->backgroundColor.a / 255.0f;

            float x0 = box.x, y0 = box.y;
            float x1 = box.x + box.width, y1 = box.y + box.height;

            /* Clamp to scissor rect */
            if (clipping) {
                if (x0 < clip_x0) x0 = clip_x0;
                if (y0 < clip_y0) y0 = clip_y0;
                if (x1 > clip_x1) x1 = clip_x1;
                if (y1 > clip_y1) y1 = clip_y1;
                if (x0 >= x1 || y0 >= y1) break; /* fully clipped */
            }

            if (ui->rect_vert_count + 6 <= MAX_UI_RECT_VERTICES) {
                ui_rect_vertex *v = &ui->rect_verts[ui->rect_vert_count];
                v[0] = (ui_rect_vertex){x0, y0, r, g, b, a};
                v[1] = (ui_rect_vertex){x1, y0, r, g, b, a};
                v[2] = (ui_rect_vertex){x1, y1, r, g, b, a};
                v[3] = (ui_rect_vertex){x0, y0, r, g, b, a};
                v[4] = (ui_rect_vertex){x1, y1, r, g, b, a};
                v[5] = (ui_rect_vertex){x0, y1, r, g, b, a};
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
            int has_icon = (icon_atlas_texture && icon_atlas_sampler)
                ? text_contains_icon_cp(text->stringContents.chars, text->stringContents.length)
                : 0;

            /* Skip entire text command if fully outside scissor */
            if (clipping) {
                if (box.x + box.width < clip_x0 || box.x > clip_x1 ||
                    box.y + box.height < clip_y0 || box.y > clip_y1)
                    break;
            }

            if (has_icon) {
                /* For icon-containing labels (Bootstrap icons + text), run a simple
                   UTF-8 glyph pass and route quads to either the regular or icon atlas. */
                float cursor_x = floorf(box.x);
                float baseline_y = floorf(box.y + font_size * font_ascent);
                float text_line_center = box.y + box.height * 0.5f;
                uint32_t idx = 0;
                while (idx < text->stringContents.length) {
                    uint32_t cp = 0;
                    uint32_t next_idx = idx + 1;
                    const msdf_glyph *mg = NULL;
                    int use_icon_font = 0;
                    float glyph_size = font_size;
                    float glyph_w;
                    float glyph_h;
                    float gx;
                    float gy;

                    if (!utf8_decode_at(text->stringContents.chars, text->stringContents.length,
                                        idx, &cp, &next_idx)) {
                        break;
                    }

                    mg = icon_glyph_lookup(cp);
                    if (mg) {
                        use_icon_font = 1;
                        /* Keep icons slightly smaller than text and center them per-line. */
                        glyph_size = font_size * 0.90f;
                    } else if (cp >= 32 && cp < 127) {
                        mg = &font_glyphs[cp];
                    }

                    if (mg && mg->width > 0 && mg->height > 0) {
                        glyph_w = (float)mg->width * (glyph_size / (float)MSDF_SOURCE_SIZE);
                        glyph_h = (float)mg->height * (glyph_size / (float)MSDF_SOURCE_SIZE);
                        gx = floorf(cursor_x + mg->xoff * glyph_size);
                        gy = baseline_y + mg->yoff * glyph_size;

                        if (use_icon_font) {
                            float icon_center = gy + glyph_h * 0.5f;
                            gy = floorf(gy + (text_line_center - icon_center));
                            gy += icon_inline_y_nudge(cp, font_size);
                        }

                        if (!(clipping && (gx + glyph_w < clip_x0 || gx > clip_x1 ||
                                           gy + glyph_h < clip_y0 || gy > clip_y1))) {
                            ui_push_font_quad(ui, use_icon_font, gx, gy, glyph_w, glyph_h, mg, r, g, b, a);
                        }
                    }

                    if (mg) cursor_x += mg->advance * glyph_size;
                    else cursor_x += font_size * 0.5f;

                    if (next_idx <= idx) break;
                    idx = next_idx;
                }
            } else {
                hb_buffer_t *hb_buf = hb_buffer_create();
                unsigned int glyph_count;
                hb_glyph_info_t *glyph_infos;
                hb_glyph_position_t *glyph_positions;
                float cursor_x = floorf(box.x);
                float baseline_y = floorf(box.y + font_size * font_ascent);
                float hb_to_px = font_size / (float)hb_scale;
                unsigned int gi;

                hb_buffer_add_utf8(hb_buf, text->stringContents.chars,
                    (int)text->stringContents.length, 0, (int)text->stringContents.length);
                hb_buffer_set_direction(hb_buf, HB_DIRECTION_LTR);
                hb_buffer_set_script(hb_buf, HB_SCRIPT_LATIN);
                hb_buffer_set_language(hb_buf, hb_language_from_string("en", -1));
                hb_shape(hb_editor_font, hb_buf, NULL, 0);

                glyph_infos = hb_buffer_get_glyph_infos(hb_buf, &glyph_count);
                glyph_positions = hb_buffer_get_glyph_positions(hb_buf, &glyph_count);

                for (gi = 0; gi < glyph_count; gi++) {
                    uint32_t cluster = glyph_infos[gi].cluster;
                    uint32_t cp = 0;
                    const msdf_glyph *mg;
                    float glyph_w;
                    float glyph_h;
                    float gx;
                    float gy;

                    if (!utf8_decode_at(text->stringContents.chars,
                                        text->stringContents.length,
                                        cluster,
                                        &cp,
                                        NULL)) {
                        cp = 0;
                    }

                    if (cp < 32 || cp >= 127) {
                        cursor_x += glyph_positions[gi].x_advance * hb_to_px;
                        continue;
                    }

                    mg = &font_glyphs[cp];
                    if (mg->width > 0 && mg->height > 0) {
                        glyph_w = (float)mg->width * (font_size / (float)MSDF_SOURCE_SIZE);
                        glyph_h = (float)mg->height * (font_size / (float)MSDF_SOURCE_SIZE);
                        gx = floorf(cursor_x + mg->xoff * font_size + glyph_positions[gi].x_offset * hb_to_px);
                        gy = baseline_y + mg->yoff * font_size + glyph_positions[gi].y_offset * hb_to_px;

                        if (!(clipping && (gx + glyph_w < clip_x0 || gx > clip_x1 ||
                                           gy + glyph_h < clip_y0 || gy > clip_y1))) {
                            ui_push_font_quad(ui, 0, gx, gy, glyph_w, glyph_h, mg, r, g, b, a);
                        }
                    }

                    cursor_x += glyph_positions[gi].x_advance * hb_to_px;
                }
                hb_buffer_destroy(hb_buf);
            }
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

        SDL_GPUTransferBufferCreateInfo tbuf_info = {0};
        tbuf_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbuf_info.size = buf_size;
        SDL_GPUTransferBuffer *xfer = SDL_CreateGPUTransferBuffer(gpu_device, &tbuf_info);
        void *mapped = SDL_MapGPUTransferBuffer(gpu_device, xfer, false);
        memcpy(mapped, ui->rect_verts, buf_size);
        SDL_UnmapGPUTransferBuffer(gpu_device, xfer);

        SDL_GPUBufferCreateInfo gbuf_info = {0};
        gbuf_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        gbuf_info.size = buf_size;
        ui->rect_gpu_buf = SDL_CreateGPUBuffer(gpu_device, &gbuf_info);

        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd_buf);
        SDL_GPUTransferBufferLocation src = {0};
        src.transfer_buffer = xfer;
        SDL_GPUBufferRegion dst = {0};
        dst.buffer = ui->rect_gpu_buf;
        dst.size = buf_size;
        SDL_UploadToGPUBuffer(copy, &src, &dst, false);
        SDL_EndGPUCopyPass(copy);
        SDL_ReleaseGPUTransferBuffer(gpu_device, xfer);
    }

    if (ui->font_vert_count > 0) {
        Uint32 buf_size = (Uint32)(ui->font_vert_count * sizeof(font_vertex));

        SDL_GPUTransferBufferCreateInfo tbuf_info = {0};
        tbuf_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbuf_info.size = buf_size;
        SDL_GPUTransferBuffer *xfer = SDL_CreateGPUTransferBuffer(gpu_device, &tbuf_info);
        void *mapped = SDL_MapGPUTransferBuffer(gpu_device, xfer, false);
        memcpy(mapped, ui->font_verts, buf_size);
        SDL_UnmapGPUTransferBuffer(gpu_device, xfer);

        SDL_GPUBufferCreateInfo gbuf_info = {0};
        gbuf_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        gbuf_info.size = buf_size;
        ui->font_gpu_buf = SDL_CreateGPUBuffer(gpu_device, &gbuf_info);

        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd_buf);
        SDL_GPUTransferBufferLocation src = {0};
        src.transfer_buffer = xfer;
        SDL_GPUBufferRegion dst = {0};
        dst.buffer = ui->font_gpu_buf;
        dst.size = buf_size;
        SDL_UploadToGPUBuffer(copy, &src, &dst, false);
        SDL_EndGPUCopyPass(copy);
        SDL_ReleaseGPUTransferBuffer(gpu_device, xfer);
    }

    if (ui->icon_font_vert_count > 0) {
        Uint32 buf_size = (Uint32)(ui->icon_font_vert_count * sizeof(font_vertex));

        SDL_GPUTransferBufferCreateInfo tbuf_info = {0};
        tbuf_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbuf_info.size = buf_size;
        SDL_GPUTransferBuffer *xfer = SDL_CreateGPUTransferBuffer(gpu_device, &tbuf_info);
        void *mapped = SDL_MapGPUTransferBuffer(gpu_device, xfer, false);
        memcpy(mapped, ui->icon_font_verts, buf_size);
        SDL_UnmapGPUTransferBuffer(gpu_device, xfer);

        SDL_GPUBufferCreateInfo gbuf_info = {0};
        gbuf_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        gbuf_info.size = buf_size;
        ui->icon_font_gpu_buf = SDL_CreateGPUBuffer(gpu_device, &gbuf_info);

        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd_buf);
        SDL_GPUTransferBufferLocation src = {0};
        src.transfer_buffer = xfer;
        SDL_GPUBufferRegion dst = {0};
        dst.buffer = ui->icon_font_gpu_buf;
        dst.size = buf_size;
        SDL_UploadToGPUBuffer(copy, &src, &dst, false);
        SDL_EndGPUCopyPass(copy);
        SDL_ReleaseGPUTransferBuffer(gpu_device, xfer);
    }
}

// Game window: run Clay layout for game UI overlay, build + upload vertices
static void ui_prepare_game(SDL_GPUCommandBuffer *cmd_buf, memory *g) {
    Clay_SetCurrentContext(clay_context);

    float win_w = (float)panel_tex_w[PANEL_GAME] / display_density;
    float win_h = (float)panel_tex_h[PANEL_GAME] / display_density;
    if (win_w <= 0) win_w = 800;
    if (win_h <= 0) win_h = 600;
    Clay_SetLayoutDimensions((Clay_Dimensions){win_w, win_h});

    Clay_BeginLayout();
    /* (game HUD elements will go here later) */
    Clay_RenderCommandArray commands = Clay_EndLayout();

    ui_build_vertices(&ui_game, commands);
    ui_upload(cmd_buf, &ui_game);
}

// ---------------------------------------------------------------------------
// Profiler helpers (tree_arena, flatten, format_bytes, colors) live in editor.c
// ---------------------------------------------------------------------------

// Profiler window: read pre-computed Clay commands from editor.dll, build + upload vertices
static void profiler_prepare(SDL_GPUCommandBuffer *cmd_buf, memory *g) {
    if (g->editor.profiler_cmd_count <= 0 || !g->editor.profiler_cmd_array)
        return;

    Clay_RenderCommandArray commands;
    commands.length = g->editor.profiler_cmd_count;
    commands.internalArray = (Clay_RenderCommand *)g->editor.profiler_cmd_array;

    ui_build_vertices(&ui_profiler, commands);

    /* Append scrollbar thumb as an extra rect.
       Uses scroll data + PTreeScroll bounding box exported by profiler_layout. */
    {
        editor_state *e = &g->editor;
        float content_h = e->prof_content_h;
        float container_h = e->prof_container_h;
        float track_h = e->prof_track_h;
        if (content_h > container_h && track_h > 0) {
            float sb_w = 6;  /* scrollbar width in pixels */
            float thumb_frac = container_h / content_h;
            float thumb_h = track_h * thumb_frac;
            if (thumb_h < 20) thumb_h = 20;
            float scroll_frac = (-e->prof_scroll_pos) / (content_h - container_h);
            float thumb_y = e->prof_track_y + scroll_frac * (track_h - thumb_h);
            float sb_x = e->prof_track_x + e->prof_track_w - sb_w - 2;

            if (ui_profiler.rect_vert_count + 6 <= MAX_UI_RECT_VERTICES) {
                ui_rect_vertex *v = &ui_profiler.rect_verts[ui_profiler.rect_vert_count];
                float r = 1.0f, g2 = 1.0f, b = 1.0f, a = 0.3f;
                v[0] = (ui_rect_vertex){sb_x,        thumb_y,          r, g2, b, a};
                v[1] = (ui_rect_vertex){sb_x + sb_w, thumb_y,          r, g2, b, a};
                v[2] = (ui_rect_vertex){sb_x + sb_w, thumb_y + thumb_h, r, g2, b, a};
                v[3] = (ui_rect_vertex){sb_x,        thumb_y,          r, g2, b, a};
                v[4] = (ui_rect_vertex){sb_x + sb_w, thumb_y + thumb_h, r, g2, b, a};
                v[5] = (ui_rect_vertex){sb_x,        thumb_y + thumb_h, r, g2, b, a};
                ui_profiler.rect_vert_count += 6;
            }
        }
    }

    ui_upload(cmd_buf, &ui_profiler);

    /* Upload grid pixel buffer to GPU texture + build quad vertex buffer (copy pass) */
    {
        editor_state *e = &g->editor;
        int gw = e->prof_grid_w, gh = e->prof_grid_h;
        float panel_w = (float)panel_tex_w[PANEL_PROFILER] / display_density;
        float panel_h = (float)panel_tex_h[PANEL_PROFILER] / display_density;

        /* Release previous frame's quad buffer */
        if (grid_quad_buf) { SDL_ReleaseGPUBuffer(gpu_device, grid_quad_buf); grid_quad_buf = NULL; }

        if (gw > 0 && gh > 0 && panel_w > 0 && panel_h > 0 &&
            e->prof_grid_bw > 0 && e->prof_grid_bh > 0) {
            /* Recreate texture if dimensions changed */
            if (grid_tex_w != gw || grid_tex_h != gh || !grid_texture) {
                if (grid_texture) SDL_ReleaseGPUTexture(gpu_device, grid_texture);
                SDL_GPUTextureCreateInfo ti = {0};
                ti.type = SDL_GPU_TEXTURETYPE_2D;
                ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
                ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
                ti.width = (Uint32)gw;
                ti.height = (Uint32)gh;
                ti.layer_count_or_depth = 1;
                ti.num_levels = 1;
                ti.sample_count = SDL_GPU_SAMPLECOUNT_1;
                grid_texture = SDL_CreateGPUTexture(gpu_device, &ti);
                grid_tex_w = gw;
                grid_tex_h = gh;
            }

            /* Upload pixels via transfer buffer */
            {
                Uint32 pixel_size = (Uint32)(gw * gh * 4);
                SDL_GPUTransferBufferCreateInfo tbi = {0};
                tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
                tbi.size = pixel_size;
                SDL_GPUTransferBuffer *xfer = SDL_CreateGPUTransferBuffer(gpu_device, &tbi);
                void *mapped = SDL_MapGPUTransferBuffer(gpu_device, xfer, false);
                memcpy(mapped, e->prof_grid_pixels, pixel_size);
                SDL_UnmapGPUTransferBuffer(gpu_device, xfer);

                SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd_buf);
                SDL_GPUTextureTransferInfo src = {0};
                src.transfer_buffer = xfer;
                SDL_GPUTextureRegion dst = {0};
                dst.texture = grid_texture;
                dst.w = (Uint32)gw;
                dst.h = (Uint32)gh;
                dst.d = 1;
                SDL_UploadToGPUTexture(copy, &src, &dst, false);
                SDL_EndGPUCopyPass(copy);
                SDL_ReleaseGPUTransferBuffer(gpu_device, xfer);
            }

            /* Build and upload quad vertex buffer (pixel → NDC) */
            {
                float x0 = e->prof_grid_x, y0 = e->prof_grid_y;
                float x1 = x0 + e->prof_grid_bw, y1 = y0 + e->prof_grid_bh;
                float nx0 = (x0 / panel_w) * 2.0f - 1.0f;
                float ny0 = 1.0f - (y0 / panel_h) * 2.0f;
                float nx1 = (x1 / panel_w) * 2.0f - 1.0f;
                float ny1 = 1.0f - (y1 / panel_h) * 2.0f;

                composite_vertex verts[6] = {
                    {nx0, ny0, 0, 0, 1, 1, 1, 1},
                    {nx1, ny0, 1, 0, 1, 1, 1, 1},
                    {nx1, ny1, 1, 1, 1, 1, 1, 1},
                    {nx0, ny0, 0, 0, 1, 1, 1, 1},
                    {nx1, ny1, 1, 1, 1, 1, 1, 1},
                    {nx0, ny1, 0, 1, 1, 1, 1, 1}
                };

                Uint32 vsize = sizeof(verts);
                SDL_GPUTransferBufferCreateInfo vtbi = {0};
                vtbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
                vtbi.size = vsize;
                SDL_GPUTransferBuffer *vxfer = SDL_CreateGPUTransferBuffer(gpu_device, &vtbi);
                void *vmapped = SDL_MapGPUTransferBuffer(gpu_device, vxfer, false);
                memcpy(vmapped, verts, vsize);
                SDL_UnmapGPUTransferBuffer(gpu_device, vxfer);

                SDL_GPUBufferCreateInfo vbi = {0};
                vbi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
                vbi.size = vsize;
                grid_quad_buf = SDL_CreateGPUBuffer(gpu_device, &vbi);

                SDL_GPUCopyPass *vcopy = SDL_BeginGPUCopyPass(cmd_buf);
                SDL_GPUTransferBufferLocation vsrc = {0};
                vsrc.transfer_buffer = vxfer;
                SDL_GPUBufferRegion vdst = {0};
                vdst.buffer = grid_quad_buf;
                vdst.size = vsize;
                SDL_UploadToGPUBuffer(vcopy, &vsrc, &vdst, false);
                SDL_EndGPUCopyPass(vcopy);
                SDL_ReleaseGPUTransferBuffer(gpu_device, vxfer);
            }
        }
    }
}

static void scene_tree_prepare(SDL_GPUCommandBuffer *cmd_buf, memory *g) {
    if (g->editor.scene_tree_cmd_count <= 0 || !g->editor.scene_tree_cmd_array)
        return;

    Clay_RenderCommandArray commands;
    commands.length = g->editor.scene_tree_cmd_count;
    commands.internalArray = (Clay_RenderCommand *)g->editor.scene_tree_cmd_array;

    ui_build_vertices(&ui_scene_tree, commands);
    ui_upload(cmd_buf, &ui_scene_tree);
}

static void project_browser_prepare(SDL_GPUCommandBuffer *cmd_buf, memory *g) {
    if (g->editor.project_browser_cmd_count <= 0 || !g->editor.project_browser_cmd_array)
        return;

    Clay_RenderCommandArray commands;
    commands.length = g->editor.project_browser_cmd_count;
    commands.internalArray = (Clay_RenderCommand *)g->editor.project_browser_cmd_array;

    ui_build_vertices(&ui_project_browser, commands);
    ui_upload(cmd_buf, &ui_project_browser);
}

static void inspector_prepare(SDL_GPUCommandBuffer *cmd_buf, memory *g) {
    if (g->editor.inspector_cmd_count <= 0 || !g->editor.inspector_cmd_array)
        return;

    Clay_RenderCommandArray commands;
    commands.length = g->editor.inspector_cmd_count;
    commands.internalArray = (Clay_RenderCommand *)g->editor.inspector_cmd_array;

    ui_build_vertices(&ui_inspector, commands);
    ui_upload(cmd_buf, &ui_inspector);
}

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

static void editor_toolbar_prepare(SDL_GPUCommandBuffer *cmd_buf, memory *g) {
    if (g->editor.editor_toolbar_cmd_count <= 0 || !g->editor.editor_toolbar_cmd_array)
        return;

    Clay_RenderCommandArray commands;
    commands.length = g->editor.editor_toolbar_cmd_count;
    commands.internalArray = (Clay_RenderCommand *)g->editor.editor_toolbar_cmd_array;

    ui_build_vertices(&ui_editor_toolbar, commands);
    ui_upload(cmd_buf, &ui_editor_toolbar);
}

static void menu_bar_prepare(SDL_GPUCommandBuffer *cmd_buf, memory *g) {
    if (g->editor.menu_bar_cmd_count <= 0 || !g->editor.menu_bar_cmd_array)
        return;

    Clay_RenderCommandArray commands;
    commands.length = g->editor.menu_bar_cmd_count;
    commands.internalArray = (Clay_RenderCommand *)g->editor.menu_bar_cmd_array;

    ui_build_vertices(&ui_menu_bar, commands);
    ui_upload(cmd_buf, &ui_menu_bar);
}

/* Draw the grid texture quad during the profiler render pass.
   All GPU resources were prepared by profiler_prepare (copy phase). */
static void grid_tex_draw(SDL_GPURenderPass *pass, SDL_GPUCommandBuffer *cmd_buf) {
    if (!grid_texture || !grid_tex_pipeline || !grid_quad_buf || !grid_sampler)
        return;

    SDL_BindGPUGraphicsPipeline(pass, grid_tex_pipeline);

    SDL_GPUBufferBinding vb = {0};
    vb.buffer = grid_quad_buf;
    SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);

    SDL_GPUTextureSamplerBinding ts = {0};
    ts.texture = grid_texture;
    ts.sampler = grid_sampler;
    SDL_BindGPUFragmentSamplers(pass, 0, &ts, 1);

    SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
}

// Draw UI during render pass (works for any UIRenderState)
static void ui_draw(SDL_GPURenderPass *render_pass, SDL_GPUCommandBuffer *cmd_buf,
                    uniform_data *uniforms, UIRenderState *ui) {
    if (ui->rect_vert_count > 0 && ui->rect_gpu_buf) {
        SDL_BindGPUGraphicsPipeline(render_pass, ui_rect_pipeline);
        SDL_PushGPUVertexUniformData(cmd_buf, 0, uniforms, sizeof(*uniforms));

        SDL_GPUBufferBinding vbuf_binding = {0};
        vbuf_binding.buffer = ui->rect_gpu_buf;
        SDL_BindGPUVertexBuffers(render_pass, 0, &vbuf_binding, 1);
        SDL_DrawGPUPrimitives(render_pass, (Uint32)ui->rect_vert_count, 1, 0, 0);
    }

    if (ui->font_vert_count > 0 && ui->font_gpu_buf &&
        font_atlas_texture && font_atlas_sampler) {
        SDL_BindGPUGraphicsPipeline(render_pass, font_pipeline);
        SDL_PushGPUVertexUniformData(cmd_buf, 0, uniforms, sizeof(*uniforms));

        SDL_GPUTextureSamplerBinding sampler_binding = {0};
        sampler_binding.texture = font_atlas_texture;
        sampler_binding.sampler = font_atlas_sampler;
        SDL_BindGPUFragmentSamplers(render_pass, 0, &sampler_binding, 1);

        SDL_GPUBufferBinding vbuf_binding = {0};
        vbuf_binding.buffer = ui->font_gpu_buf;
        SDL_BindGPUVertexBuffers(render_pass, 0, &vbuf_binding, 1);
        SDL_DrawGPUPrimitives(render_pass, (Uint32)ui->font_vert_count, 1, 0, 0);
    }

    if (ui->icon_font_vert_count > 0 && ui->icon_font_gpu_buf &&
        icon_atlas_texture && icon_atlas_sampler) {
        SDL_BindGPUGraphicsPipeline(render_pass, font_pipeline);
        SDL_PushGPUVertexUniformData(cmd_buf, 0, uniforms, sizeof(*uniforms));

        SDL_GPUTextureSamplerBinding sampler_binding = {0};
        sampler_binding.texture = icon_atlas_texture;
        sampler_binding.sampler = icon_atlas_sampler;
        SDL_BindGPUFragmentSamplers(render_pass, 0, &sampler_binding, 1);

        SDL_GPUBufferBinding vbuf_binding = {0};
        vbuf_binding.buffer = ui->icon_font_gpu_buf;
        SDL_BindGPUVertexBuffers(render_pass, 0, &vbuf_binding, 1);
        SDL_DrawGPUPrimitives(render_pass, (Uint32)ui->icon_font_vert_count, 1, 0, 0);
    }
}

// Cleanup per-frame UI GPU buffers for one window
static void ui_release_buffers(UIRenderState *ui) {
    if (ui->rect_gpu_buf) { SDL_ReleaseGPUBuffer(gpu_device, ui->rect_gpu_buf); ui->rect_gpu_buf = NULL; }
    if (ui->font_gpu_buf) { SDL_ReleaseGPUBuffer(gpu_device, ui->font_gpu_buf); ui->font_gpu_buf = NULL; }
    if (ui->icon_font_gpu_buf) { SDL_ReleaseGPUBuffer(gpu_device, ui->icon_font_gpu_buf); ui->icon_font_gpu_buf = NULL; }
}

// Forward declarations (defined after init_externals, needed by TCC)
static int  ensure_panel_texture(int panel_idx, int w, int h);
static void ensure_panel_textures(dock_state *d);

// ---------------------------------------------------------------------------
// init_externals
// ---------------------------------------------------------------------------

EXPORT int init_externals(struct memory *m) {
    dock_state *dock = NULL;

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

    // 3. Create single docked window (all panels inside)
    window = SDL_CreateWindow("Anitra", 1600, 900, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return -1;
    }

    display_density = SDL_GetWindowPixelDensity(window);

    // 4. Create GPU device
    gpu_device = SDL_CreateGPUDevice(
        SDL_ShaderCross_GetSPIRVShaderFormats(),
        true,  // debug_mode
        NULL);
    if (!gpu_device) {
        fprintf(stderr, "SDL_CreateGPUDevice failed: %s\n", SDL_GetError());
        return -1;
    }

    /* Expose GPU device to engine for asset loading */
    m->game.gpu_device = gpu_device;

    /* Initialize profilers */
    cache_prof_init();
    cpu_prof_init();
    cpu_prof_capture_paused = 0;
    cpu_prof_capture_was_paused = 0;

    // 5. Claim window
    if (!SDL_ClaimWindowForGPUDevice(gpu_device, window)) {
        fprintf(stderr, "SDL_ClaimWindowForGPUDevice failed: %s\n", SDL_GetError());
        return -1;
    }

    /* Store swapchain format — used for offscreen textures + pipelines */
    offscreen_format = SDL_GetGPUSwapchainTextureFormat(gpu_device, window);

    // 6. Compile sprite shaders (split files to avoid DXC including unused resources)
// 6. Compile sprite shaders (split files to avoid DXC including unused resources)
    SDL_GPUShader* sprite_vs = load_shader_from_spirv(
        m->game.shader_sprite_vs, "main", SDL_SHADERCROSS_SHADERSTAGE_VERTEX);
    SDL_GPUShader* sprite_fs = load_shader_from_spirv(
        m->game.shader_sprite_fs, "main", SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT);
    if (!sprite_vs || !sprite_fs) {
        fprintf(stderr, "Failed to compile sprite shaders\n");
        return -1;
    }

    // 7. Create sprite pipeline
    {
        SDL_GPUVertexBufferDescription vbuf_desc = {0};
        vbuf_desc.slot = 0;
        vbuf_desc.pitch = sizeof(sprite_vertex);
        vbuf_desc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        vbuf_desc.instance_step_rate = 0;

        SDL_GPUVertexAttribute attrs[3] = {0};
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

        SDL_GPUColorTargetBlendState blend = {0};
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

        SDL_GPUColorTargetDescription color_target = {0};
        color_target.format = swapchain_format;
        color_target.blend_state = blend;

        SDL_GPUGraphicsPipelineCreateInfo pipe_info = {0};
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
        m->game.shader_debug_lines_vs, "main", SDL_SHADERCROSS_SHADERSTAGE_VERTEX);
    SDL_GPUShader* line_fs = load_shader_from_spirv(
        m->game.shader_debug_lines_fs, "main", SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT);
    if (!line_vs || !line_fs) {
        fprintf(stderr, "Failed to compile debug line shaders\n");
        return -1;
    }

    // 9. Create line pipeline
    {
        SDL_GPUVertexBufferDescription vbuf_desc = {0};
        vbuf_desc.slot = 0;
        vbuf_desc.pitch = sizeof(line_vertex);
        vbuf_desc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        vbuf_desc.instance_step_rate = 0;

        SDL_GPUVertexAttribute attrs[2] = {0};
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

        SDL_GPUColorTargetDescription color_target = {0};
        color_target.format = swapchain_format;

        SDL_GPUGraphicsPipelineCreateInfo pipe_info = {0};
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

    // 9b. Create 3D editor line pipeline (float3 position)
    {
        const char *editor_line_vs_path = "assets/shaders/compiled/editor_line_vs.spv";
        SDL_GPUShader *ed_vs = load_shader_from_spirv(
            editor_line_vs_path, "main", SDL_SHADERCROSS_SHADERSTAGE_VERTEX);
        SDL_GPUShader *ed_fs = load_shader_from_spirv(
            m->game.shader_debug_lines_fs, "main", SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT);
        if (!ed_vs || !ed_fs) {
            fprintf(stderr, "Failed to compile editor line shaders (vs=%s)\n", editor_line_vs_path);
            return -1;
        }

        SDL_GPUVertexBufferDescription vbuf_desc = {0};
        vbuf_desc.slot = 0;
        vbuf_desc.pitch = sizeof(editor_line_vert); /* float3 pos + float3 color = 24 bytes */
        vbuf_desc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

        SDL_GPUVertexAttribute attrs[2] = {0};
        attrs[0].location = 0;
        attrs[0].buffer_slot = 0;
        attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
        attrs[0].offset = 0;
        attrs[1].location = 1;
        attrs[1].buffer_slot = 0;
        attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
        attrs[1].offset = sizeof(float) * 3;

        SDL_GPUTextureFormat swapchain_format =
            SDL_GetGPUSwapchainTextureFormat(gpu_device, window);

        SDL_GPUColorTargetDescription color_target = {0};
        color_target.format = swapchain_format;

        SDL_GPUGraphicsPipelineCreateInfo pipe_info = {0};
        pipe_info.vertex_shader = ed_vs;
        pipe_info.fragment_shader = ed_fs;
        pipe_info.vertex_input_state.vertex_buffer_descriptions = &vbuf_desc;
        pipe_info.vertex_input_state.num_vertex_buffers = 1;
        pipe_info.vertex_input_state.vertex_attributes = attrs;
        pipe_info.vertex_input_state.num_vertex_attributes = 2;
        pipe_info.primitive_type = SDL_GPU_PRIMITIVETYPE_LINELIST;
        pipe_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        pipe_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        pipe_info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipe_info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        pipe_info.target_info.color_target_descriptions = &color_target;
        pipe_info.target_info.num_color_targets = 1;
        pipe_info.target_info.has_depth_stencil_target = true;
        pipe_info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

        pipe_info.depth_stencil_state.enable_depth_test = true;
        pipe_info.depth_stencil_state.enable_depth_write = true;
        pipe_info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;

        editor_line_pipeline = SDL_CreateGPUGraphicsPipeline(gpu_device, &pipe_info);
        if (!editor_line_pipeline) {
            fprintf(stderr, "Failed to create editor line pipeline: %s\n", SDL_GetError());
            return -1;
        }

        SDL_ReleaseGPUShader(gpu_device, ed_vs);
        SDL_ReleaseGPUShader(gpu_device, ed_fs);
    }

    // 10. Compile and create UI rect pipeline
    {
        SDL_GPUShader *ui_vs = load_shader_from_spirv(
            m->game.shader_ui_rect_vs, "main", SDL_SHADERCROSS_SHADERSTAGE_VERTEX);
        SDL_GPUShader *ui_fs = load_shader_from_spirv(
            m->game.shader_ui_rect_fs, "main", SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT);
        if (!ui_vs || !ui_fs) {
            fprintf(stderr, "Failed to compile UI rect shaders\n");
            return -1;
        }

        SDL_GPUVertexBufferDescription vbuf_desc = {0};
        vbuf_desc.slot = 0;
        vbuf_desc.pitch = sizeof(ui_rect_vertex);
        vbuf_desc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

        SDL_GPUVertexAttribute attrs[2] = {0};
        attrs[0].location = 0;
        attrs[0].buffer_slot = 0;
        attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        attrs[0].offset = 0;
        attrs[1].location = 1;
        attrs[1].buffer_slot = 0;
        attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        attrs[1].offset = sizeof(float) * 2;

        SDL_GPUColorTargetBlendState blend = {0};
        blend.enable_blend = true;
        blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        blend.color_blend_op = SDL_GPU_BLENDOP_ADD;
        blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

        SDL_GPUTextureFormat swapchain_format =
            SDL_GetGPUSwapchainTextureFormat(gpu_device, window);

        SDL_GPUColorTargetDescription color_target = {0};
        color_target.format = swapchain_format;
        color_target.blend_state = blend;

        SDL_GPUGraphicsPipelineCreateInfo pipe_info = {0};
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
            m->game.shader_font_vs, "main", SDL_SHADERCROSS_SHADERSTAGE_VERTEX);
        SDL_GPUShader *font_fs = load_shader_from_spirv(
            m->game.shader_font_fs, "main", SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT);
        if (!font_vs || !font_fs) {
            fprintf(stderr, "Failed to compile font shaders\n");
            return -1;
        }

        SDL_GPUVertexBufferDescription vbuf_desc = {0};
        vbuf_desc.slot = 0;
        vbuf_desc.pitch = sizeof(font_vertex);
        vbuf_desc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

        SDL_GPUVertexAttribute attrs[3] = {0};
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

        SDL_GPUColorTargetBlendState blend = {0};
        blend.enable_blend = true;
        blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        blend.color_blend_op = SDL_GPU_BLENDOP_ADD;
        blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

        SDL_GPUTextureFormat swapchain_format =
            SDL_GetGPUSwapchainTextureFormat(gpu_device, window);

        SDL_GPUColorTargetDescription color_target = {0};
        color_target.format = swapchain_format;
        color_target.blend_state = blend;

        SDL_GPUGraphicsPipelineCreateInfo pipe_info = {0};
        pipe_info.vertex_shader = font_vs;
        pipe_info.fragment_shader = font_fs;
        pipe_info.vertex_input_state.vertex_buffer_descriptions = &vbuf_desc;
        pipe_info.vertex_input_state.num_vertex_buffers = 1;
        pipe_info.vertex_input_state.vertex_attributes = attrs;
        pipe_info.vertex_input_state.num_vertex_attributes = 3;
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
            m->game.shader_mesh_vs, "main", SDL_SHADERCROSS_SHADERSTAGE_VERTEX);
        SDL_GPUShader *mesh_fs = load_shader_from_spirv(
            m->game.shader_mesh_fs, "main", SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT);
        if (!mesh_vs || !mesh_fs) {
            fprintf(stderr, "Failed to compile mesh shaders\n");
            return -1;
        }

        SDL_GPUVertexBufferDescription vbuf_desc = {0};
        vbuf_desc.slot = 0;
        vbuf_desc.pitch = sizeof(SkinnedVertex);
        vbuf_desc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

        SDL_GPUVertexAttribute attrs[5] = {0};
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

        SDL_GPUColorTargetBlendState blend = {0};
        blend.enable_blend = false;

        SDL_GPUTextureFormat swapchain_format =
            SDL_GetGPUSwapchainTextureFormat(gpu_device, window);

        SDL_GPUColorTargetDescription color_target = {0};
        color_target.format = swapchain_format;
        color_target.blend_state = blend;

        SDL_GPUGraphicsPipelineCreateInfo pipe_info = {0};
        pipe_info.vertex_shader = mesh_vs;
        pipe_info.fragment_shader = mesh_fs;
        pipe_info.vertex_input_state.vertex_buffer_descriptions = &vbuf_desc;
        pipe_info.vertex_input_state.num_vertex_buffers = 1;
        pipe_info.vertex_input_state.vertex_attributes = attrs;
        pipe_info.vertex_input_state.num_vertex_attributes = 5;
        pipe_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipe_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        /* Dungeon assets include single-sided pieces; disabling cull avoids
           accidental see-through when camera views back faces. */
        pipe_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
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
        SDL_GPUSamplerCreateInfo samp_info = {0};
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
        SDL_GPUTextureCreateInfo tex_info = {0};
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
        SDL_GPUTransferBufferCreateInfo tbuf_info = {0};
        tbuf_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbuf_info.size = 4;
        SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(gpu_device, &tbuf_info);
        void *map = SDL_MapGPUTransferBuffer(gpu_device, transfer, false);
        memcpy(map, &white_pixel, 4);
        SDL_UnmapGPUTransferBuffer(gpu_device, transfer);

        SDL_GPUCommandBuffer *upload_cmd = SDL_AcquireGPUCommandBuffer(gpu_device);
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(upload_cmd);
        SDL_GPUTextureTransferInfo src = {0};
        src.transfer_buffer = transfer;
        SDL_GPUTextureRegion dst = {0};
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
        SDL_GPUBufferCreateInfo buf_info = {0};
        buf_info.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
        buf_info.size = MAX_BONES * sizeof(Mat4);
        bone_storage_buffer = SDL_CreateGPUBuffer(gpu_device, &buf_info);
        if (!bone_storage_buffer) {
            fprintf(stderr, "Failed to create bone storage buffer: %s\n", SDL_GetError());
            return -1;
        }
    }

    // 16. Create identity bone buffer (used for static meshes).
    {
        SDL_GPUBufferCreateInfo buf_info = {0};
        SDL_GPUTransferBufferCreateInfo tbuf_info = {0};
        SDL_GPUTransferBuffer *transfer = NULL;
        SDL_GPUCommandBuffer *upload_cmd = NULL;
        SDL_GPUCopyPass *copy = NULL;
        SDL_GPUTransferBufferLocation src = {0};
        SDL_GPUBufferRegion dst = {0};
        Mat4 *bones;
        int bi;

        buf_info.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
        buf_info.size = MAX_BONES * sizeof(Mat4);
        bone_identity_buffer = SDL_CreateGPUBuffer(gpu_device, &buf_info);
        if (!bone_identity_buffer) {
            fprintf(stderr, "Failed to create identity bone buffer: %s\n", SDL_GetError());
            return -1;
        }

        tbuf_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbuf_info.size = MAX_BONES * sizeof(Mat4);
        transfer = SDL_CreateGPUTransferBuffer(gpu_device, &tbuf_info);
        if (!transfer) {
            fprintf(stderr, "Failed to create identity bone transfer buffer: %s\n", SDL_GetError());
            return -1;
        }

        bones = (Mat4 *)SDL_MapGPUTransferBuffer(gpu_device, transfer, false);
        for (bi = 0; bi < MAX_BONES; bi++) {
            bones[bi] = mat4_identity();
        }
        SDL_UnmapGPUTransferBuffer(gpu_device, transfer);

        upload_cmd = SDL_AcquireGPUCommandBuffer(gpu_device);
        copy = SDL_BeginGPUCopyPass(upload_cmd);
        src.transfer_buffer = transfer;
        dst.buffer = bone_identity_buffer;
        dst.size = MAX_BONES * sizeof(Mat4);
        SDL_UploadToGPUBuffer(copy, &src, &dst, false);
        SDL_EndGPUCopyPass(copy);
        SDL_SubmitGPUCommandBuffer(upload_cmd);
        SDL_ReleaseGPUTransferBuffer(gpu_device, transfer);
    }

    // 17. Create sampler (nearest-neighbor for pixel art)
    {
        SDL_GPUSamplerCreateInfo samp_info = {0};
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

// 11. Load textures from config paths
    gpu_textures[TEXTURE_PLAYER]      = load_gpu_texture(m->game.texture_player);
    gpu_textures[TEXTURE_TILES]       = load_gpu_texture(m->game.texture_tiles);
    gpu_textures[TEXTURE_SLIME]       = load_gpu_texture(m->game.texture_slime);
    gpu_textures[TEXTURE_HEALTH_BAR]  = load_gpu_texture(m->game.texture_health_bar);
    gpu_textures[TEXTURE_HEALTH_FILL] = load_gpu_texture(m->game.texture_health_fill);

    // Load editor font (MSDF atlas) and upload to GPU
    if (font_load_msdf(m->game.font_editor) != 0) {
        fprintf(stderr, "Failed to load editor font from: %s\n", m->game.font_editor);
        return -1;
    }
    if (harfbuzz_init() != 0) {
        fprintf(stderr, "Failed to init HarfBuzz\n");
        return -1;
    }

    if (icon_font_load_msdf(BOOTSTRAP_ICON_FONT_PATH) != 0) {
        fprintf(stderr, "Warning: failed to load Bootstrap Icons font from: %s\n",
                BOOTSTRAP_ICON_FONT_PATH);
    } else {
        printf("Loaded Bootstrap Icons font (%d glyphs).\n", icon_glyph_count);
    }

    // Pre-compute ASCII advance table for editor.dll's Clay text measurement
    {
        float scale1 = stbtt_ScaleForPixelHeight(&font_stb_info, 1.0f);
        int ch;
        for (ch = 0; ch < 128; ch++) {
            int advance, lsb;
            stbtt_GetCodepointHMetrics(&font_stb_info, ch, &advance, &lsb);
            m->editor.font_advances[ch] = (float)advance * scale1;
        }
        m->editor.font_line_height = font_ascent - font_descent + font_line_gap;
    }

    // Initialize arena (single allocation for all engine memory)
    {
        uint32_t arena_size = 500 * 1024 * 1024; // 500 MB
        void *arena_mem = malloc(arena_size);
        arena_init(&m->arena, arena_mem, arena_size);
        printf("Arena initialized (%u bytes)\n", arena_size);
    }

    // Set cross-references so game_state and editor_state can reach root arena
    m->game.root_arena = &m->arena;
    m->editor.root_arena = &m->arena;

    // Allocate editor sub-arena (dock state, editor-specific allocations)
    m->editor.editor_arena = arena_alloc_subarena(&m->arena, 50 * 1024 * 1024, 16, "editor");
    m->editor.dock = arena_alloc(m->editor.editor_arena, sizeof(dock_state), 16, "dock_state");
    dock = (dock_state *)m->editor.dock;

    // Initialize Clay UI — game context (from main arena, for in-game UI: pause menu, HUD)
    {
        uint64_t clay_mem_size = Clay_MinMemorySize();
        clay_arena_game = arena_alloc_subarena(&m->arena, (uint32_t)clay_mem_size, 16, "clay_game");

        Clay_Arena clay_arena = Clay_CreateArenaWithCapacityAndMemory(clay_mem_size, clay_arena_game->base);

        int window_w, window_h;
        SDL_GetWindowSize(window, &window_w, &window_h);

        Clay_ErrorHandler err_handler = {0};
        clay_context = Clay_Initialize(clay_arena, (Clay_Dimensions){(float)window_w, (float)window_h}, err_handler);
        Clay_SetMeasureTextFunction(clay_measure_text, NULL);
        printf("Clay game context initialized (%llu bytes from main arena)\n", (unsigned long long)clay_mem_size);
    }

    // Clay editor context is now created by editor.dll (init_editor) — we just publish the game context
    m->game.clay_game = clay_context;

    // Initialize dock system (single window, three-column layout)
    if (!dock->initialized) {
        dock_init_default(dock);
    }
    dock->windows[0].sdl_window = window;
    m->editor.open = 1;
    m->editor.window = window;  /* editor gets the main window handle for focus/mouse checks */

    // Create composite pipeline (draws panel textures into window)
    {
        SDL_GPUShader *comp_vs = load_shader_from_spirv(
            m->game.shader_composite_vs, "main", SDL_SHADERCROSS_SHADERSTAGE_VERTEX);
        SDL_GPUShader *comp_fs = load_shader_from_spirv(
            m->game.shader_composite_fs, "main", SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT);
        if (!comp_vs || !comp_fs) {
            fprintf(stderr, "Failed to compile composite shaders\n");
            return -1;
        }

        SDL_GPUVertexBufferDescription vbuf_desc = {0};
        vbuf_desc.slot = 0;
        vbuf_desc.pitch = sizeof(composite_vertex);
        vbuf_desc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

        SDL_GPUVertexAttribute attrs[3] = {0};
        attrs[0].location = 0;
        attrs[0].buffer_slot = 0;
        attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        attrs[0].offset = 0;
        attrs[1].location = 1;
        attrs[1].buffer_slot = 0;
        attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        attrs[1].offset = sizeof(float) * 2;
        attrs[2].location = 2;
        attrs[2].buffer_slot = 0;
        attrs[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        attrs[2].offset = sizeof(float) * 4;

        SDL_GPUColorTargetDescription ct = {0};
        ct.format = offscreen_format;
        ct.blend_state.enable_blend = true;
        ct.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        ct.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        ct.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        ct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

        SDL_GPUGraphicsPipelineCreateInfo pipe_info = {0};
        pipe_info.vertex_shader = comp_vs;
        pipe_info.fragment_shader = comp_fs;
        pipe_info.vertex_input_state.vertex_buffer_descriptions = &vbuf_desc;
        pipe_info.vertex_input_state.num_vertex_buffers = 1;
        pipe_info.vertex_input_state.vertex_attributes = attrs;
        pipe_info.vertex_input_state.num_vertex_attributes = 3;
        pipe_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipe_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        pipe_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        pipe_info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        pipe_info.target_info.color_target_descriptions = &ct;
        pipe_info.target_info.num_color_targets = 1;
        pipe_info.target_info.has_depth_stencil_target = false;

        composite_pipeline = SDL_CreateGPUGraphicsPipeline(gpu_device, &pipe_info);
        if (!composite_pipeline) {
            fprintf(stderr, "Failed to create composite pipeline: %s\n", SDL_GetError());
            return -1;
        }
        SDL_ReleaseGPUShader(gpu_device, comp_vs);
        SDL_ReleaseGPUShader(gpu_device, comp_fs);
    }

    // Create composite sampler (linear for smooth panel blitting)
    {
        SDL_GPUSamplerCreateInfo si = {0};
        si.min_filter = SDL_GPU_FILTER_LINEAR;
        si.mag_filter = SDL_GPU_FILTER_LINEAR;
        si.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        composite_sampler = SDL_CreateGPUSampler(gpu_device, &si);
    }

    // Create grid texture pipeline (same shaders as composite, but with depth stencil
    // so it can draw inside the profiler render pass which has a depth target)
    {
        SDL_GPUShader *grid_vs = load_shader_from_spirv(
            m->game.shader_composite_vs, "main", SDL_SHADERCROSS_SHADERSTAGE_VERTEX);
        SDL_GPUShader *grid_fs = load_shader_from_spirv(
            m->game.shader_composite_fs, "main", SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT);

        SDL_GPUVertexBufferDescription vbuf_desc = {0};
        vbuf_desc.slot = 0;
        vbuf_desc.pitch = sizeof(composite_vertex);
        vbuf_desc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

        SDL_GPUVertexAttribute attrs[3] = {0};
        attrs[0].location = 0; attrs[0].buffer_slot = 0;
        attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2; attrs[0].offset = 0;
        attrs[1].location = 1; attrs[1].buffer_slot = 0;
        attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2; attrs[1].offset = sizeof(float)*2;
        attrs[2].location = 2; attrs[2].buffer_slot = 0;
        attrs[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4; attrs[2].offset = sizeof(float)*4;

        SDL_GPUColorTargetDescription ct = {0};
        ct.format = offscreen_format;
        ct.blend_state.enable_blend = true;
        ct.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        ct.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        ct.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        ct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

        SDL_GPUGraphicsPipelineCreateInfo pi = {0};
        pi.vertex_shader = grid_vs;
        pi.fragment_shader = grid_fs;
        pi.vertex_input_state.vertex_buffer_descriptions = &vbuf_desc;
        pi.vertex_input_state.num_vertex_buffers = 1;
        pi.vertex_input_state.vertex_attributes = attrs;
        pi.vertex_input_state.num_vertex_attributes = 3;
        pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        pi.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        pi.target_info.color_target_descriptions = &ct;
        pi.target_info.num_color_targets = 1;
        pi.target_info.has_depth_stencil_target = true;
        pi.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

        grid_tex_pipeline = SDL_CreateGPUGraphicsPipeline(gpu_device, &pi);
        if (!grid_tex_pipeline) {
            fprintf(stderr, "Failed to create grid texture pipeline: %s\n", SDL_GetError());
            return -1;
        }
        SDL_ReleaseGPUShader(gpu_device, grid_vs);
        SDL_ReleaseGPUShader(gpu_device, grid_fs);

        /* Nearest-neighbor sampler for crisp pixel cells */
        SDL_GPUSamplerCreateInfo gsi = {0};
        gsi.min_filter = SDL_GPU_FILTER_NEAREST;
        gsi.mag_filter = SDL_GPU_FILTER_NEAREST;
        gsi.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        gsi.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        gsi.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        grid_sampler = SDL_CreateGPUSampler(gpu_device, &gsi);
    }

    // Create pre-rendered tab header textures (once at init)
    {
        int i;
        for (i = 0; i < PANEL_COUNT; i++) {
            header_textures[i] = create_header_texture(panel_names[i], &header_tex_w[i]);
            if (!header_textures[i]) {
                fprintf(stderr, "Failed to create header texture for panel %d\n", i);
            }
        }
    }

    // Allocate rendering sub-arena (draw_commands, mesh_commands, debug_lines, debug_vertices)
    {
        arena *rendering = arena_alloc_subarena(&m->arena, 256 * 1024, 16, "rendering");

        m->game.dl.sprite_capacity = MAX_DRAW_COMMANDS;
        m->game.dl.sprites = (draw_command*)arena_alloc(rendering,
            (uint32_t)(MAX_DRAW_COMMANDS * sizeof(draw_command)), 16, "draw_commands");
        m->game.dl.mesh_capacity = DRAW_LIST_MAX_MESH_COMMANDS;
        m->game.dl.meshes = (mesh_draw_command*)arena_alloc(rendering,
            (uint32_t)(DRAW_LIST_MAX_MESH_COMMANDS * sizeof(mesh_draw_command)), 16, "mesh_commands");
        m->game.dl.line_capacity = DRAW_LIST_MAX_DEBUG_LINES;
        m->game.dl.lines = (debug_line_command*)arena_alloc(rendering,
            (uint32_t)(DRAW_LIST_MAX_DEBUG_LINES * sizeof(debug_line_command)), 16, "debug_lines");

        m->game.dbg.max_lines = 1000;
        m->game.dbg.current_line_count = 0;
        m->game.dbg.vertex_buffer = (float*)arena_alloc(rendering,
            (uint32_t)(1000 * 10 * sizeof(float)), 16, "debug_vertices");
    }

    // Allocate gameplay sub-arena (entities, etc.)
    m->game.gameplay = arena_alloc_subarena(&m->arena, 4 * 1024 * 1024, 16, "gameplay");

    // Create initial panel textures (dock layout → panel rects → offscreen textures)
    {
        int win_w, win_h;
        SDL_GetWindowSize(window, &win_w, &win_h);
        dock_layout(dock, 0, win_w, win_h);
        ensure_panel_textures(dock);
    }

    // Init game timing
    m->game._t_prev = (double)SDL_GetTicks() / 1000.0;
    m->game.dt = 0.0f;
    m->game.play = true;

    printf("Externals initialized (SDL3 GPU, docked panels)\n");
    return 1;
}


// ---------------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------------

static void update_input(memory *m) {
    m->game.input.horizontal = 0.0f;
    m->game.input.vertical = 0.0f;
    m->game.input.input_mask = 0;

    if (!m->game.editor_play_mode) {
        return;
    }

    /* Suppress keyboard game input when the app is unfocused. */
    SDL_Window *focused = SDL_GetKeyboardFocus();
    bool game_has_focus = (focused == window) || (focused == NULL);
    if (!game_has_focus) {
        dock_state *dock = (dock_state *)m->editor.dock;
        if (dock) {
            int wi;
            for (wi = 0; wi < MAX_DOCK_WINDOWS; wi++) {
                if (!dock->windows[wi].in_use) continue;
                if (focused == (SDL_Window *)dock->windows[wi].sdl_window) {
                    game_has_focus = true;
                    break;
                }
            }
        }
    }

    // Keyboard input
    float kb_horizontal = 0.0f;
    float kb_vertical = 0.0f;

    if (game_has_focus) {
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

        if (keys[SDL_SCANCODE_SPACE] || keys[SDL_SCANCODE_J])
            m->game.input.input_mask |= INPUT_A;
        if (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT] || keys[SDL_SCANCODE_K])
            m->game.input.input_mask |= INPUT_B;
        if (keys[SDL_SCANCODE_E] || keys[SDL_SCANCODE_L])
            m->game.input.input_mask |= INPUT_X;
        if (keys[SDL_SCANCODE_Q] || keys[SDL_SCANCODE_I] || keys[SDL_SCANCODE_TAB])
            m->game.input.input_mask |= INPUT_Y;
    }

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
                m->game.input.input_mask |= INPUT_A;
            if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST))
                m->game.input.input_mask |= INPUT_B;
            if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_WEST))
                m->game.input.input_mask |= INPUT_X;
            if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_NORTH))
                m->game.input.input_mask |= INPUT_Y;

            SDL_CloseGamepad(pad);
        }
    }
    SDL_free(gamepads);

    // Combine keyboard and gamepad (take stronger input)
    m->game.input.horizontal = (fabsf(kb_horizontal) > fabsf(gp_horizontal)) ? kb_horizontal : gp_horizontal;
    m->game.input.vertical   = (fabsf(kb_vertical)   > fabsf(gp_vertical))   ? kb_vertical   : gp_vertical;
}

/* Refresh in-memory project data from disk so play-stop reset uses latest saved scene. */
static int reload_project_state_from_disk(game_state *gs) {
    project_data loaded_project;
    int li;

    if (!gs) return 0;
    if (!gs->project_loaded) return 1;
    if (!gs->project_path[0]) return 1;

    if (project_load(gs->project_path, &loaded_project) != 0) {
        fprintf(stderr, "Warning: failed to reload project from '%s'; keeping current runtime scene\n",
                gs->project_path);
        return 0;
    }

    gs->project = loaded_project;
    gs->project_loaded = 1;

    if (gs->project.has_camera) {
        gs->mesh3d.camera_eye = VEC3(gs->project.camera.eye[0],
                                     gs->project.camera.eye[1], gs->project.camera.eye[2]);
        gs->mesh3d.camera_target = VEC3(gs->project.camera.target[0],
                                        gs->project.camera.target[1], gs->project.camera.target[2]);
        gs->mesh3d.camera_up = VEC3(gs->project.camera.up[0],
                                    gs->project.camera.up[1], gs->project.camera.up[2]);
        gs->mesh3d.camera_fov_deg = gs->project.camera.fov;
        gs->mesh3d.camera_set_by_project = 1;
    }

    if (gs->project.has_lighting) {
        gs->lighting.ambient = VEC3(gs->project.lighting.ambient[0],
                                    gs->project.lighting.ambient[1],
                                    gs->project.lighting.ambient[2]);
        gs->lighting.light_count = gs->project.lighting.point_light_count;
        for (li = 0; li < gs->project.lighting.point_light_count; li++) {
            const project_point_light *src = &gs->project.lighting.point_lights[li];
            gs->lighting.lights[li].position = VEC3(src->position[0], src->position[1], src->position[2]);
            gs->lighting.lights[li].color = VEC3(src->color[0], src->color[1], src->color[2]);
            gs->lighting.lights[li].intensity = src->intensity;
            gs->lighting.lights[li].radius = src->radius;
        }
    }

    return 1;
}

// ---------------------------------------------------------------------------
// Dock panel texture management
// ---------------------------------------------------------------------------

/* Create or resize an offscreen texture pair (color + depth) for a panel.
   Returns 1 if textures were (re)created, 0 if already correct. */
static int ensure_panel_texture(int panel_idx, int w, int h)
{
    if (w <= 0 || h <= 0) return 0;
    if (panel_tex_w[panel_idx] == w && panel_tex_h[panel_idx] == h
        && panel_color[panel_idx] != NULL)
        return 0;

    /* Release old textures */
    if (panel_color[panel_idx]) {
        SDL_ReleaseGPUTexture(gpu_device, panel_color[panel_idx]);
        panel_color[panel_idx] = NULL;
    }
    if (panel_depth[panel_idx]) {
        SDL_ReleaseGPUTexture(gpu_device, panel_depth[panel_idx]);
        panel_depth[panel_idx] = NULL;
    }

    /* Color target (also sampled by composite pass) */
    {
        SDL_GPUTextureCreateInfo ci = {0};
        ci.type = SDL_GPU_TEXTURETYPE_2D;
        ci.format = offscreen_format;
        ci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ci.width = (Uint32)w;
        ci.height = (Uint32)h;
        ci.layer_count_or_depth = 1;
        ci.num_levels = 1;
        ci.sample_count = SDL_GPU_SAMPLECOUNT_1;
        panel_color[panel_idx] = SDL_CreateGPUTexture(gpu_device, &ci);
    }

    /* Depth target */
    {
        SDL_GPUTextureCreateInfo di = {0};
        di.type = SDL_GPU_TEXTURETYPE_2D;
        di.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        di.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        di.width = (Uint32)w;
        di.height = (Uint32)h;
        di.layer_count_or_depth = 1;
        di.num_levels = 1;
        di.sample_count = SDL_GPU_SAMPLECOUNT_1;
        panel_depth[panel_idx] = SDL_CreateGPUTexture(gpu_device, &di);
    }

    panel_tex_w[panel_idx] = w;
    panel_tex_h[panel_idx] = h;
    return 1;
}

/* Ensure all visible panel textures match their dock layout rects */
static void ensure_panel_textures(dock_state *d)
{
    int i, j;
    for (i = 0; i < MAX_DOCK_WINDOWS; i++) {
        if (!d->windows[i].in_use) continue;
        for (j = 0; j < MAX_DOCK_NODES; j++) {
            DockNode *n = &d->nodes[j];
            if (!n->in_use || n->type != DOCK_TABS || n->panel_count == 0) continue;
            {
                PanelId pid = n->panels[n->active_tab];
                int pw = (int)(n->w * display_density);
                int ph = (int)((n->h - DOCK_HEADER_HEIGHT) * display_density);
                if (ph < 1) ph = 1;
                ensure_panel_texture((int)pid, pw, ph);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// update_externals
// ---------------------------------------------------------------------------

EXPORT void update_externals(struct memory *m) {
    dock_state *dock = (dock_state *)m->editor.dock;
    static int previous_editor_play_mode = -1;
    TracyCZoneN(ctx_update, "update_externals", 1);
    // --- Timing ---
    double now = (double)SDL_GetTicks() / 1000.0;
    double dtd = now - m->game._t_prev;
    m->game._t_prev = now;
    if (dtd < 0.0) dtd = 0.0;
    if (dtd > 0.1) dtd = 0.1;
    m->game.dt = (float)dtd;

    // --- Events ---
    {
        TracyCZoneN(ctx_poll, "SDL_PollEvents", 1);
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                m->game.play = false;
                TracyCZoneEnd(ctx_poll);
                TracyCZoneEnd(ctx_update);
                return;
            }

            /* Window close: main window = quit, tear-off = return panel */
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                SDL_WindowID closing_id = event.window.windowID;
                int wi;
                int is_main = 0;
                for (wi = 0; wi < MAX_DOCK_WINDOWS; wi++) {
                    SDL_Window *dw = (SDL_Window *)dock->windows[wi].sdl_window;
                    if (dw && SDL_GetWindowID(dw) == closing_id) {
                        if (wi == 0) {
                            is_main = 1;
                        } else {
                            /* Tear-off window closed: return panels to main window */
                            PanelId recovered[PANEL_COUNT];
                            int rcount = 0;
                            int ri;
                            dock_collect_leaves(dock, dock->windows[wi].root_node,
                                                recovered, &rcount, PANEL_COUNT);
                            dock_free_subtree(dock, dock->windows[wi].root_node);

                            /* Add recovered panels as tabs to main window's first leaf */
                            {
                                int main_leaf = -1;
                                int mni;
                                for (mni = 0; mni < MAX_DOCK_NODES; mni++) {
                                    DockNode *mn = &dock->nodes[mni];
                                    if (mn->in_use && mn->type == DOCK_TABS && mn->panel_count > 0) {
                                        /* Check if this node belongs to window 0 */
                                        int chk = dock_leaf_for_panel(dock, dock->windows[0].root_node, mn->panels[0]);
                                        if (chk == mni) { main_leaf = mni; break; }
                                    }
                                }
                                if (main_leaf >= 0) {
                                    DockNode *ml = &dock->nodes[main_leaf];
                                    for (ri = 0; ri < rcount && ml->panel_count < MAX_TABS_PER_NODE; ri++) {
                                        ml->panels[ml->panel_count] = recovered[ri];
                                        ml->panel_count++;
                                    }
                                }
                            }

                            /* Release and destroy the tear-off window */
                            SDL_ReleaseWindowFromGPUDevice(gpu_device, dw);
                            SDL_DestroyWindow(dw);
                            dock->windows[wi].in_use = 0;
                            dock->windows[wi].sdl_window = NULL;
                        }
                        break;
                    }
                }
                if (is_main) {
                    m->game.play = false;
                    TracyCZoneEnd(ctx_poll);
                    TracyCZoneEnd(ctx_update);
                    return;
                }
                continue; /* don't forward close events to editor */
            }

            /* Dispatch all events to editor.dll (dock interaction + camera + gizmo) */
            if (g_editor_handle_event) {
                if (g_editor_handle_event(&m->game, &m->editor, &event))
                    continue; /* editor consumed the event */
            }
        }
        TracyCZoneEnd(ctx_poll);
    }

    /* Leaving Play mode: rebuild scene from project so runtime mutations are discarded. */
    if (previous_editor_play_mode < 0) {
        previous_editor_play_mode = m->game.editor_play_mode ? 1 : 0;
    }
    if (previous_editor_play_mode == 1 && !m->game.editor_play_mode) {
        int can_rebuild_scene = reload_project_state_from_disk(&m->game);
        if (g_init && can_rebuild_scene) {
            g_init(&m->game);
            m->game.editor_play_mode = 0;
            m->game.input.horizontal = 0.0f;
            m->game.input.vertical = 0.0f;
            m->game.input.input_mask = 0;
        }
    }
    previous_editor_play_mode = m->game.editor_play_mode ? 1 : 0;

    // --- Input ---
    FRAME_CPU_ZONE_BEGIN("ext_input_update");
    FRAME_CACHE_ZONE_BEGIN("ext_input_update");
    update_input(m);
    FRAME_CACHE_ZONE_END();
    FRAME_CPU_ZONE_END();

    // --- Process dock commands from editor.dll ---

    // Tear-off: editor set cmd_tear_off, we create the window + mutate tree
    if (dock->cmd_tear_off) {
        FRAME_CPU_ZONE_BEGIN("dock_tear_off");
        PanelId tp = dock->drag.panel;
        int twi, new_win_idx = -1;
        for (twi = 1; twi < MAX_DOCK_WINDOWS; twi++) {
            if (!dock->windows[twi].in_use) { new_win_idx = twi; break; }
        }
        if (new_win_idx >= 0) {
            SDL_Window *new_win = SDL_CreateWindow(panel_names[tp],
                                                    600, 500, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
            if (new_win) {
                int new_root = -1;
                int src_win_idx;
                int move_ok = 0;
                error_value err = ERRV_OK;
                SDL_SetWindowPosition(new_win,
                    (int)(dock->cmd_screen_x - 300),
                    (int)(dock->cmd_screen_y - 12));
                SDL_ClaimWindowForGPUDevice(gpu_device, new_win);

                new_root = dock_alloc_node(dock);
                if (new_root < 0) {
                    fprintf(stderr, "dock_tear_off failed: unable to allocate node for torn-off panel\n");
                } else {
                    dock->nodes[new_root].panels[0] = tp;
                    dock->nodes[new_root].panel_count = 1;
                    dock->nodes[new_root].active_tab = 0;

                    src_win_idx = dock->drag.source_window;
                    if (dock_remove_panel_with_error(dock, dock->drag.source_node, tp, &err) == 0) {
                        int new_src_root = -1;
                        err = ERRV_OK;
                        if (dock_collapse_empty_with_error(dock,
                                                           dock->windows[src_win_idx].root_node,
                                                           &new_src_root,
                                                           &err) == 0) {
                            dock->windows[src_win_idx].root_node = new_src_root;
                            move_ok = 1;
                        } else {
                            fprintf(stderr,
                                    "dock_tear_off collapse failed (%d): %s [%s:%d]\n",
                                    err.code,
                                    err.message ? err.message : "(no message)",
                                    err.file ? err.file : "(unknown)",
                                    err.line);
                        }
                    } else {
                        fprintf(stderr,
                                "dock_tear_off remove failed (%d): %s [%s:%d]\n",
                                err.code,
                                err.message ? err.message : "(no message)",
                                err.file ? err.file : "(unknown)",
                                err.line);
                    }

                    if (move_ok) {
                        dock->windows[new_win_idx].in_use = 1;
                        dock->windows[new_win_idx].sdl_window = new_win;
                        dock->windows[new_win_idx].root_node = new_root;
                    }
                }

                if (move_ok) {
                    if (dock->windows[src_win_idx].root_node < 0 && src_win_idx != 0) {
                        SDL_Window *dead_win = (SDL_Window *)dock->windows[src_win_idx].sdl_window;
                        if (dead_win) {
                            SDL_ReleaseWindowFromGPUDevice(gpu_device, dead_win);
                            SDL_DestroyWindow(dead_win);
                        }
                        dock->windows[src_win_idx].in_use = 0;
                        dock->windows[src_win_idx].sdl_window = NULL;
                    }
                } else {
                    if (new_root >= 0) {
                        dock_free_node(dock, new_root);
                    }
                    SDL_ReleaseWindowFromGPUDevice(gpu_device, new_win);
                    SDL_DestroyWindow(new_win);
                }
            }
        }
        dock->cmd_tear_off = 0;
        dock->drag.phase = DRAG_IDLE;
        FRAME_CPU_ZONE_END();
    }

    // Re-dock: editor set cmd_redock, we add panel as tab + destroy empty source
    if (dock->cmd_redock) {
        FRAME_CPU_ZONE_BEGIN("dock_redock");
        PanelId rp = dock->drag.panel;
        int src_win_idx = dock->drag.source_window;
        int src_node = dock->drag.source_node;
        int tgt_win_idx = dock->cmd_redock_target;

        {
            int tgt_root = dock->windows[tgt_win_idx].root_node;
            int tgt_leaf = -1;
            int ni;
            for (ni = 0; ni < MAX_DOCK_NODES && tgt_leaf < 0; ni++) {
                if (!dock->nodes[ni].in_use || dock->nodes[ni].type != DOCK_TABS) continue;
                if (dock->nodes[ni].panel_count == 0) continue;
                if (dock_leaf_for_panel(dock, tgt_root, dock->nodes[ni].panels[0]) == ni)
                    tgt_leaf = ni;
            }

            if (tgt_leaf >= 0) {
                int moved = 0;
                error_value err = ERRV_OK;
                int new_tab = dock_add_panel_with_error(dock, tgt_leaf, rp, &err);
                if (new_tab >= 0) {
                    dock_set_active_tab(dock, tgt_leaf, new_tab);
                    err = ERRV_OK;
                    if (dock_remove_panel_with_error(dock, src_node, rp, &err) == 0) {
                        int new_src_root = -1;
                        moved = 1;
                        err = ERRV_OK;
                        if (dock_collapse_empty_with_error(dock,
                                                           dock->windows[src_win_idx].root_node,
                                                           &new_src_root,
                                                           &err) == 0) {
                            dock->windows[src_win_idx].root_node = new_src_root;
                        } else {
                            fprintf(stderr,
                                    "dock_redock collapse failed (%d): %s [%s:%d]\n",
                                    err.code,
                                    err.message ? err.message : "(no message)",
                                    err.file ? err.file : "(unknown)",
                                    err.line);
                        }
                    } else {
                        error_value rb_err = ERRV_OK;
                        fprintf(stderr,
                                "dock_redock remove failed (%d): %s [%s:%d]\n",
                                err.code,
                                err.message ? err.message : "(no message)",
                                err.file ? err.file : "(unknown)",
                                err.line);
                        if (dock_remove_panel_with_error(dock, tgt_leaf, rp, &rb_err) < 0) {
                            fprintf(stderr,
                                    "dock_redock rollback failed (%d): %s [%s:%d]\n",
                                    rb_err.code,
                                    rb_err.message ? rb_err.message : "(no message)",
                                    rb_err.file ? rb_err.file : "(unknown)",
                                    rb_err.line);
                        }
                    }
                } else {
                    fprintf(stderr,
                            "dock_redock add failed (%d): %s [%s:%d]\n",
                            err.code,
                            err.message ? err.message : "(no message)",
                            err.file ? err.file : "(unknown)",
                            err.line);
                }

                if (moved && dock->windows[src_win_idx].root_node < 0 && src_win_idx != 0) {
                    SDL_Window *dead_win = (SDL_Window *)dock->windows[src_win_idx].sdl_window;
                    if (dead_win) {
                        SDL_ReleaseWindowFromGPUDevice(gpu_device, dead_win);
                        SDL_DestroyWindow(dead_win);
                    }
                    dock->windows[src_win_idx].in_use = 0;
                    dock->windows[src_win_idx].sdl_window = NULL;
                }
            }
        }

        dock->cmd_redock = 0;
        dock->drag.phase = DRAG_IDLE;
        FRAME_CPU_ZONE_END();
    }

    // Cleanup: destroy any empty dock windows (editor set cmd_cleanup_windows)
    if (dock->cmd_cleanup_windows) {
        FRAME_CPU_ZONE_BEGIN("dock_cleanup_windows");
        int cwi2;
        for (cwi2 = 1; cwi2 < MAX_DOCK_WINDOWS; cwi2++) {
            if (!dock->windows[cwi2].in_use) continue;
            if (dock->windows[cwi2].root_node >= 0) continue;
            {
                SDL_Window *dead = (SDL_Window *)dock->windows[cwi2].sdl_window;
                if (dead) {
                    SDL_ReleaseWindowFromGPUDevice(gpu_device, dead);
                    SDL_DestroyWindow(dead);
                }
                dock->windows[cwi2].in_use = 0;
                dock->windows[cwi2].sdl_window = NULL;
            }
        }
        dock->cmd_cleanup_windows = 0;
        FRAME_CPU_ZONE_END();
    }

    // --- Update editor (dock layout, panel rects, profiler Clay, camera, gizmo, lines) ---
    // Editor.dll now does: dock_layout for all windows, game panel size, editor panel
    // rect, profiler Clay layout. Must run BEFORE ensure_panel_textures and ortho.
    // Sync game Clay arena usage so profiler display is accurate
    FRAME_CPU_ZONE_BEGIN("editor_update");
    FRAME_CACHE_ZONE_BEGIN("editor_update");
    if (clay_arena_game && clay_context)
        clay_arena_game->used = (uint32_t)clay_context->internalArena.nextAllocation;
    if (m->editor.open && g_editor_update) g_editor_update(&m->game, &m->editor);
    FRAME_CACHE_ZONE_END();
    FRAME_CPU_ZONE_END();

    cpu_prof_capture_paused = (m->editor.cpu_prof_timeline_paused != 0);
    cpu_prof_set_capture_enabled(!cpu_prof_capture_paused);
    if (!cpu_prof_capture_paused) {
        if (cpu_prof_capture_was_paused) {
            /* Discard stale pre-pause data so resumed capture starts cleanly. */
            cpu_prof_clear_current_frame();
        } else {
            cpu_prof_frame_end();
        }
    }
    cpu_prof_capture_was_paused = cpu_prof_capture_paused;

    // --- Panel textures (after dock_layout in editor, which set node rects) ---
    FRAME_CPU_ZONE_BEGIN("ensure_panel_textures");
    ensure_panel_textures(dock);
    FRAME_CPU_ZONE_END();

    // --- Ortho projection (based on game panel size, set by editor.dll) ---
    FRAME_CPU_ZONE_BEGIN("ortho_projection");
    {
        float w = (float)m->game.width;
        float h = (float)m->game.height;
        float ortho[16] = {
            2.0f/w, 0,      0,     0,
            0,      2.0f/h, 0,     0,
            0,      0,     -1.0f,  0,
            0,      0,      0,     1.0f
        };
        memcpy(m->game.dl.ortho_projection, ortho, sizeof(ortho));
    }
    FRAME_CPU_ZONE_END();

    // --- Reset draw list ---
    FRAME_CPU_ZONE_BEGIN("reset_draw_list");
    m->game.dl.sprite_count = 0;
    m->game.dl.line_count = 0;
    m->game.dl.mesh_count = 0;
    FRAME_CPU_ZONE_END();

    // --- Call engine update (fills draw_list + updates mesh3d animation) ---
    FRAME_CPU_ZONE_BEGIN("engine_update_call");
    FRAME_CACHE_ZONE_BEGIN("engine_update_call");
    cache_prof_frame_reset();
    g_update(&m->game);
    FRAME_CACHE_ZONE_END();
    FRAME_CPU_ZONE_END();

    // --- Render ---
    FRAME_CPU_ZONE_BEGIN("gpu_cmd_acquire");
    SDL_GPUCommandBuffer *cmd_buf = SDL_AcquireGPUCommandBuffer(gpu_device);
    FRAME_CPU_ZONE_END();
    if (!cmd_buf) {
        fprintf(stderr, "Failed to acquire command buffer: %s\n", SDL_GetError());
        TracyCZoneEnd(ctx_update);
        return;
    }

    // ================================================================
    // ALL COPY PASSES FIRST (both windows) — no render passes yet
    // ================================================================

    // --- Build raw draw command uploads (sprites + debug lines) ---
    draw_upload_buffers draw_upload;
    int sprite_count;
    int line_count;
    int line_vertex_count;
    SDL_GPUBuffer *sprite_gpu_buf;
    SDL_GPUBuffer *line_gpu_buf;

    FRAME_CPU_ZONE_BEGIN("draw_upload_build");
    FRAME_CACHE_ZONE_BEGIN("draw_upload_build");
    draw_upload_build(gpu_device, cmd_buf, &m->game.dl, &draw_upload);
    FRAME_CACHE_ZONE_END();
    FRAME_CPU_ZONE_END();
    sprite_count = draw_upload.sprite_count;
    line_count = draw_upload.line_count;
    line_vertex_count = draw_upload.line_vertex_count;
    sprite_gpu_buf = draw_upload.sprite_gpu_buf;
    line_gpu_buf = draw_upload.line_gpu_buf;

    /* --- Upload bone matrices (computed by engine) --- */
    {
        void *bone_src = NULL;
        Uint32 bone_size = 0;

        if (m->game.anim.pool_initialized && m->game.anim.pool.skin_mats &&
            m->game.anim.pool.skin_mats_count > 0) {
            bone_src = m->game.anim.pool.skin_mats;
            bone_size = (Uint32)(m->game.anim.pool.skin_mats_count * sizeof(Mat4));
        } else if (m->game.mesh3d.visible && m->game.mesh3d.skeleton.joint_count > 0 &&
                   m->game.mesh3d.skin_mats) {
            bone_src = m->game.mesh3d.skin_mats;
            bone_size = m->game.mesh3d.skeleton.joint_count * sizeof(Mat4);
        }

        if (bone_src && bone_size > 0) {
            FRAME_CPU_ZONE_BEGIN("upload_bone_matrices");
            {
                SDL_GPUTransferBufferCreateInfo tbuf_info = {0};
                SDL_GPUTransferBuffer *bone_transfer;
                void *map;
                SDL_GPUCopyPass *copy_pass;
                SDL_GPUTransferBufferLocation src_loc = {0};
                SDL_GPUBufferRegion dst_region = {0};

                tbuf_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
                tbuf_info.size = bone_size;
                bone_transfer = SDL_CreateGPUTransferBuffer(gpu_device, &tbuf_info);
                map = SDL_MapGPUTransferBuffer(gpu_device, bone_transfer, false);
                memcpy(map, bone_src, bone_size);
                SDL_UnmapGPUTransferBuffer(gpu_device, bone_transfer);

                copy_pass = SDL_BeginGPUCopyPass(cmd_buf);
                src_loc.transfer_buffer = bone_transfer;
                dst_region.buffer = bone_storage_buffer;
                dst_region.size = bone_size;
                SDL_UploadToGPUBuffer(copy_pass, &src_loc, &dst_region, false);
                SDL_EndGPUCopyPass(copy_pass);
                SDL_ReleaseGPUTransferBuffer(gpu_device, bone_transfer);
            }
            FRAME_CPU_ZONE_END();
        }
    }

    // --- Prepare Clay UI: game window (layout + upload) ---
    FRAME_CPU_ZONE_BEGIN("ui_prepare_game");
    ui_prepare_game(cmd_buf, m);
    FRAME_CPU_ZONE_END();

    // --- Prepare Clay UI: menu bar (main window overlay) ---
    FRAME_CPU_ZONE_BEGIN("ui_prepare_menu_bar");
    menu_bar_prepare(cmd_buf, m);
    FRAME_CPU_ZONE_END();

    // --- Prepare Clay UI: profiler window (layout + upload) ---
    if (panel_color[PANEL_PROFILER]) {
        FRAME_CPU_ZONE_BEGIN("ui_prepare_profiler");
        profiler_prepare(cmd_buf, m);
        FRAME_CPU_ZONE_END();
    }

    // --- Prepare Clay UI: scene tree window (layout + upload) ---
    if (panel_color[PANEL_SCENE_TREE]) {
        FRAME_CPU_ZONE_BEGIN("ui_prepare_scene_tree");
        scene_tree_prepare(cmd_buf, m);
        FRAME_CPU_ZONE_END();
    }

    // --- Prepare Clay UI: project browser window (layout + upload) ---
    if (panel_color[PANEL_ASSETS]) {
        FRAME_CPU_ZONE_BEGIN("ui_prepare_project_browser");
        project_browser_prepare(cmd_buf, m);
        FRAME_CPU_ZONE_END();
    }

    // --- Prepare Clay UI: inspector window (layout + upload) ---
    if (panel_color[PANEL_INSPECTOR]) {
        FRAME_CPU_ZONE_BEGIN("ui_prepare_inspector");
        inspector_prepare(cmd_buf, m);
        FRAME_CPU_ZONE_END();
    }

    // --- Prepare Clay UI: cache profiler (layout + upload) ---
    if (panel_color[PANEL_CACHE_PROFILER]) {
        FRAME_CPU_ZONE_BEGIN("ui_prepare_cache_profiler");
        cache_profiler_prepare(cmd_buf, m);
        FRAME_CPU_ZONE_END();
    }

    // --- Prepare Clay UI: cpu profiler (layout + upload) ---
    if (panel_color[PANEL_CPU_PROFILER]) {
        FRAME_CPU_ZONE_BEGIN("ui_prepare_cpu_profiler");
        cpu_profiler_prepare(cmd_buf, m);
        FRAME_CPU_ZONE_END();
    }

    // --- Prepare Clay UI: editor viewport toolbar (layout + upload) ---
    if (panel_color[PANEL_EDITOR]) {
        FRAME_CPU_ZONE_BEGIN("ui_prepare_editor_toolbar");
        editor_toolbar_prepare(cmd_buf, m);
        FRAME_CPU_ZONE_END();
    }

    // --- Upload editor 3D lines (populated by editor.dll) ---
    SDL_GPUBuffer *editor_line_gpu_buf = NULL;
    int editor_vert_count = m->editor.line_count * 2;
    if (m->editor.open && editor_vert_count > 0) {
        FRAME_CPU_ZONE_BEGIN("upload_editor_lines");
        Uint32 ed_buf_size = (Uint32)(editor_vert_count * sizeof(editor_line_vert));

        SDL_GPUTransferBufferCreateInfo ed_tbuf_info = {0};
        ed_tbuf_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        ed_tbuf_info.size = ed_buf_size;

        SDL_GPUTransferBuffer *ed_transfer = SDL_CreateGPUTransferBuffer(gpu_device, &ed_tbuf_info);
        void *ed_mapped = SDL_MapGPUTransferBuffer(gpu_device, ed_transfer, false);
        memcpy(ed_mapped, m->editor.lines, ed_buf_size);
        SDL_UnmapGPUTransferBuffer(gpu_device, ed_transfer);

        SDL_GPUBufferCreateInfo ed_gpu_info = {0};
        ed_gpu_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        ed_gpu_info.size = ed_buf_size;
        editor_line_gpu_buf = SDL_CreateGPUBuffer(gpu_device, &ed_gpu_info);

        SDL_GPUCopyPass *ed_copy = SDL_BeginGPUCopyPass(cmd_buf);
        SDL_GPUTransferBufferLocation ed_src = {0};
        ed_src.transfer_buffer = ed_transfer;
        SDL_GPUBufferRegion ed_dst = {0};
        ed_dst.buffer = editor_line_gpu_buf;
        ed_dst.size = ed_buf_size;
        SDL_UploadToGPUBuffer(ed_copy, &ed_src, &ed_dst, false);
        SDL_EndGPUCopyPass(ed_copy);
        SDL_ReleaseGPUTransferBuffer(gpu_device, ed_transfer);
        FRAME_CPU_ZONE_END();
    }

    // --- Pre-build composite quads PER WINDOW (copy pass BEFORE render) ---
    // Each visible leaf produces: N tab header quads + 1 panel content quad.
    CompWindowData comp_data[MAX_DOCK_WINDOWS];
    FRAME_CPU_ZONE_BEGIN("build_composite_quads");
    {
        int cwi;
        for (cwi = 0; cwi < MAX_DOCK_WINDOWS; cwi++) {
            comp_data[cwi].gpu_buf = NULL;
            comp_data[cwi].quad_count = 0;
        }
    }
    {
        int cwi;
        for (cwi = 0; cwi < MAX_DOCK_WINDOWS; cwi++) {
            int total_quads_count, qcount;
            Uint32 total_quads, comp_buf_size;
            SDL_GPUTransferBufferCreateInfo comp_tbi;
            SDL_GPUTransferBuffer *comp_transfer;
            composite_vertex *comp_verts;
            SDL_GPUBufferCreateInfo comp_bi;
            SDL_GPUCopyPass *comp_copy;
            SDL_GPUTransferBufferLocation comp_src;
            SDL_GPUBufferRegion comp_dst;
            int ww, wh;

            if (!dock->windows[cwi].in_use || !dock->windows[cwi].sdl_window) continue;

            total_quads_count = count_tree_quads(dock, dock->windows[cwi].root_node);
            /* Reserve +1 quad for drop zone overlay if drag active on this window */
            if (dock->drag.phase == DRAG_ACTIVE && dock->drag.hover_window == cwi &&
                dock->drag.hover_node >= 0 && dock->drag.hover_zone != DROP_NONE)
                total_quads_count++;
            if (total_quads_count <= 0) continue;

            SDL_GetWindowSize((SDL_Window *)dock->windows[cwi].sdl_window, &ww, &wh);

            total_quads = (Uint32)total_quads_count;
            comp_buf_size = total_quads * 6 * (Uint32)sizeof(composite_vertex);

            memset(&comp_tbi, 0, sizeof(comp_tbi));
            comp_tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            comp_tbi.size = comp_buf_size;
            comp_transfer = SDL_CreateGPUTransferBuffer(gpu_device, &comp_tbi);
            comp_verts = (composite_vertex *)SDL_MapGPUTransferBuffer(gpu_device, comp_transfer, false);

            qcount = 0;
            {
                int vi = 0;
                vi = build_tree_quads(dock, dock->windows[cwi].root_node,
                                      comp_verts, vi, (float)ww, (float)wh,
                                      comp_data[cwi].quads, &qcount, MAX_COMP_QUADS);
                vi = build_drop_zone_quad(dock, cwi,
                                          comp_verts, vi, (float)ww, (float)wh,
                                          comp_data[cwi].quads, &qcount, MAX_COMP_QUADS);
            }
            comp_data[cwi].quad_count = qcount;

            SDL_UnmapGPUTransferBuffer(gpu_device, comp_transfer);

            memset(&comp_bi, 0, sizeof(comp_bi));
            comp_bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
            comp_bi.size = comp_buf_size;
            comp_data[cwi].gpu_buf = SDL_CreateGPUBuffer(gpu_device, &comp_bi);

            comp_copy = SDL_BeginGPUCopyPass(cmd_buf);
            memset(&comp_src, 0, sizeof(comp_src));
            comp_src.transfer_buffer = comp_transfer;
            memset(&comp_dst, 0, sizeof(comp_dst));
            comp_dst.buffer = comp_data[cwi].gpu_buf;
            comp_dst.size = comp_buf_size;
            SDL_UploadToGPUBuffer(comp_copy, &comp_src, &comp_dst, false);
            SDL_EndGPUCopyPass(comp_copy);
            SDL_ReleaseGPUTransferBuffer(gpu_device, comp_transfer);
        }
    }
    FRAME_CPU_ZONE_END();

    // ================================================================
    // RENDER PASSES — each panel renders to its offscreen texture
    // ================================================================

    // --- GAME PANEL RENDER PASS (offscreen) ---
    if (panel_color[PANEL_GAME] && panel_depth[PANEL_GAME]) {
        FRAME_CPU_ZONE_BEGIN("render_game_panel");
        Uint32 gw = (Uint32)panel_tex_w[PANEL_GAME];
        Uint32 gh = (Uint32)panel_tex_h[PANEL_GAME];

        SDL_GPUColorTargetInfo color_target = {0};
        color_target.texture = panel_color[PANEL_GAME];
        color_target.clear_color = (SDL_FColor){0.45f, 0.55f, 0.60f, 1.0f};
        color_target.load_op = SDL_GPU_LOADOP_CLEAR;
        color_target.store_op = SDL_GPU_STOREOP_STORE;

        SDL_GPUDepthStencilTargetInfo depth_target = {0};
        depth_target.texture = panel_depth[PANEL_GAME];
        depth_target.clear_depth = 1.0f;
        depth_target.load_op = SDL_GPU_LOADOP_CLEAR;
        depth_target.store_op = SDL_GPU_STOREOP_DONT_CARE;
        depth_target.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        depth_target.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;

        SDL_GPURenderPass *render_pass = SDL_BeginGPURenderPass(cmd_buf, &color_target, 1, &depth_target);
        {
            SDL_GPUViewport vp = {0};
            SDL_Rect sc = {0};
            vp.x = 0.0f;
            vp.y = 0.0f;
            vp.w = (float)gw;
            vp.h = (float)gh;
            vp.min_depth = 0.0f;
            vp.max_depth = 1.0f;
            sc.x = 0;
            sc.y = 0;
            sc.w = (int)gw;
            sc.h = (int)gh;
            SDL_SetGPUViewport(render_pass, &vp);
            SDL_SetGPUScissor(render_pass, &sc);
        }

        // 3D meshes from ECS scene (all model assets from project TOML)
        {
            float aspect = (float)gw / (float)gh;
            float fov_deg = m->game.mesh3d.camera_fov_deg > 0.0f ? m->game.mesh3d.camera_fov_deg : 60.0f;
            float near_plane = m->game.mesh3d.camera_near > 0.0f ? m->game.mesh3d.camera_near : 0.1f;
            float far_plane = m->game.mesh3d.camera_far > near_plane ? m->game.mesh3d.camera_far : 100.0f;
            Mat4 proj = mat4_perspective(fov_deg * 3.14159265f / 180.0f, aspect, near_plane, far_plane);
            Mat4 view = mat4_look_at(m->game.mesh3d.camera_eye, m->game.mesh3d.camera_target, m->game.mesh3d.camera_up);
            draw_scene_meshes(m, render_pass, cmd_buf, proj, view);
        }

        // 2D sprites + lines
        {
            uniform_data uniforms;
            memcpy(uniforms.projection, m->game.dl.ortho_projection, sizeof(float) * 16);
            memcpy(uniforms.view, m->game.dl.view_matrix, sizeof(float) * 16);

            if (sprite_count > 0 && sprite_gpu_buf) {
                SDL_BindGPUGraphicsPipeline(render_pass, sprite_pipeline);
                SDL_PushGPUVertexUniformData(cmd_buf, 0, &uniforms, sizeof(uniforms));

                SDL_GPUBufferBinding vbuf_binding = {0};
                vbuf_binding.buffer = sprite_gpu_buf;
                SDL_BindGPUVertexBuffers(render_pass, 0, &vbuf_binding, 1);

                for (int tex_id = 0; tex_id < TEXTURE_COUNT; tex_id++) {
                    if (!gpu_textures[tex_id]) continue;
                    bool bound = false;
                    for (int i = 0; i < sprite_count; i++) {
                        if (m->game.dl.sprites[i].texture_id == tex_id) {
                            if (!bound) {
                                SDL_GPUTextureSamplerBinding tex_binding = {0};
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

            if (line_count > 0 && line_gpu_buf) {
                SDL_BindGPUGraphicsPipeline(render_pass, line_pipeline);
                SDL_PushGPUVertexUniformData(cmd_buf, 0, &uniforms, sizeof(uniforms));

                SDL_GPUBufferBinding vbuf_binding = {0};
                vbuf_binding.buffer = line_gpu_buf;
                SDL_BindGPUVertexBuffers(render_pass, 0, &vbuf_binding, 1);
                SDL_DrawGPUPrimitives(render_pass, (Uint32)line_vertex_count, 1, 0, 0);
            }
        }

        // Game Clay UI overlay
        {
            uniform_data ui_uniforms;
            float ui_w = (float)gw / display_density;
            float ui_h = (float)gh / display_density;
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
        FRAME_CPU_ZONE_END();
    }

    // --- PROFILER PANEL RENDER PASS (offscreen) ---
    if (panel_color[PANEL_PROFILER]) {
        FRAME_CPU_ZONE_BEGIN("render_memory_profiler_panel");
        Uint32 pw = (Uint32)panel_tex_w[PANEL_PROFILER];
        Uint32 ph = (Uint32)panel_tex_h[PANEL_PROFILER];

        SDL_GPUColorTargetInfo prof_ct = {0};
        prof_ct.texture = panel_color[PANEL_PROFILER];
        prof_ct.clear_color = (SDL_FColor){0.10f, 0.10f, 0.12f, 1.0f};
        prof_ct.load_op = SDL_GPU_LOADOP_CLEAR;
        prof_ct.store_op = SDL_GPU_STOREOP_STORE;

        SDL_GPUDepthStencilTargetInfo prof_dt = {0};
        prof_dt.texture = panel_depth[PANEL_PROFILER];
        prof_dt.clear_depth = 1.0f;
        prof_dt.load_op = SDL_GPU_LOADOP_CLEAR;
        prof_dt.store_op = SDL_GPU_STOREOP_DONT_CARE;
        prof_dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        prof_dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;

        SDL_GPURenderPass *prof_pass = SDL_BeginGPURenderPass(cmd_buf, &prof_ct, 1, &prof_dt);

        uniform_data prof_uniforms;
        float prof_lw = (float)pw / display_density;
        float prof_lh = (float)ph / display_density;
        float prof_ortho[16] = {
            2.0f/prof_lw,    0,               0,     0,
            0,              -2.0f/prof_lh,     0,     0,
            0,               0,              -1.0f,  0,
           -1.0f,            1.0f,             0,     1.0f
        };
        memcpy(prof_uniforms.projection, prof_ortho, sizeof(prof_ortho));
        float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        memcpy(prof_uniforms.view, identity, sizeof(identity));

        ui_draw(prof_pass, cmd_buf, &prof_uniforms, &ui_profiler);
        grid_tex_draw(prof_pass, cmd_buf);

        SDL_EndGPURenderPass(prof_pass);
        FRAME_CPU_ZONE_END();
    }

    // --- SCENE TREE PANEL RENDER PASS (offscreen) ---
    if (panel_color[PANEL_SCENE_TREE] && panel_depth[PANEL_SCENE_TREE]) {
        FRAME_CPU_ZONE_BEGIN("render_scene_tree_panel");
        Uint32 sw = (Uint32)panel_tex_w[PANEL_SCENE_TREE];
        Uint32 sh = (Uint32)panel_tex_h[PANEL_SCENE_TREE];

        SDL_GPUColorTargetInfo st_ct = {0};
        st_ct.texture = panel_color[PANEL_SCENE_TREE];
        st_ct.clear_color = (SDL_FColor){0.09f, 0.12f, 0.16f, 1.0f};
        st_ct.load_op = SDL_GPU_LOADOP_CLEAR;
        st_ct.store_op = SDL_GPU_STOREOP_STORE;

        SDL_GPUDepthStencilTargetInfo st_dt = {0};
        st_dt.texture = panel_depth[PANEL_SCENE_TREE];
        st_dt.clear_depth = 1.0f;
        st_dt.load_op = SDL_GPU_LOADOP_CLEAR;
        st_dt.store_op = SDL_GPU_STOREOP_DONT_CARE;
        st_dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        st_dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;

        SDL_GPURenderPass *st_pass = SDL_BeginGPURenderPass(cmd_buf, &st_ct, 1, &st_dt);

        {
            uniform_data st_uniforms;
            float st_lw = (float)sw / display_density;
            float st_lh = (float)sh / display_density;
            float st_ortho[16] = {
                2.0f/st_lw,    0,             0,     0,
                0,            -2.0f/st_lh,    0,     0,
                0,             0,            -1.0f,  0,
               -1.0f,          1.0f,          0,     1.0f
            };
            float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            memcpy(st_uniforms.projection, st_ortho, sizeof(st_ortho));
            memcpy(st_uniforms.view, identity, sizeof(identity));
            ui_draw(st_pass, cmd_buf, &st_uniforms, &ui_scene_tree);
        }

        SDL_EndGPURenderPass(st_pass);
        FRAME_CPU_ZONE_END();
    }

    // --- PROJECT BROWSER PANEL RENDER PASS (offscreen) ---
    if (panel_color[PANEL_ASSETS] && panel_depth[PANEL_ASSETS]) {
        FRAME_CPU_ZONE_BEGIN("render_project_panel");
        Uint32 aw = (Uint32)panel_tex_w[PANEL_ASSETS];
        Uint32 ah = (Uint32)panel_tex_h[PANEL_ASSETS];

        SDL_GPUColorTargetInfo pb_ct = {0};
        pb_ct.texture = panel_color[PANEL_ASSETS];
        pb_ct.clear_color = (SDL_FColor){0.09f, 0.12f, 0.16f, 1.0f};
        pb_ct.load_op = SDL_GPU_LOADOP_CLEAR;
        pb_ct.store_op = SDL_GPU_STOREOP_STORE;

        SDL_GPUDepthStencilTargetInfo pb_dt = {0};
        pb_dt.texture = panel_depth[PANEL_ASSETS];
        pb_dt.clear_depth = 1.0f;
        pb_dt.load_op = SDL_GPU_LOADOP_CLEAR;
        pb_dt.store_op = SDL_GPU_STOREOP_DONT_CARE;
        pb_dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        pb_dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;

        SDL_GPURenderPass *pb_pass = SDL_BeginGPURenderPass(cmd_buf, &pb_ct, 1, &pb_dt);

        {
            uniform_data pb_uniforms;
            float pb_lw = (float)aw / display_density;
            float pb_lh = (float)ah / display_density;
            float pb_ortho[16] = {
                2.0f/pb_lw,    0,             0,     0,
                0,            -2.0f/pb_lh,    0,     0,
                0,             0,            -1.0f,  0,
               -1.0f,          1.0f,          0,     1.0f
            };
            float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            memcpy(pb_uniforms.projection, pb_ortho, sizeof(pb_ortho));
            memcpy(pb_uniforms.view, identity, sizeof(identity));
            ui_draw(pb_pass, cmd_buf, &pb_uniforms, &ui_project_browser);
        }

        SDL_EndGPURenderPass(pb_pass);

        /* Thumbnail 3D preview pass over project-browser swatches. */
        if (m->editor.pb_thumbnail_count > 0 && mesh_pipeline && bone_identity_buffer) {
            FRAME_CPU_ZONE_BEGIN("render_project_thumbnails");
            SDL_GPUColorTargetInfo tb_ct = {0};
            SDL_GPUDepthStencilTargetInfo tb_dt = {0};
            SDL_GPURenderPass *tb_pass;
            SDL_GPUBuffer *identity_bones = bone_identity_buffer;
            int ti;

            tb_ct.texture = panel_color[PANEL_ASSETS];
            tb_ct.load_op = SDL_GPU_LOADOP_LOAD;
            tb_ct.store_op = SDL_GPU_STOREOP_STORE;

            tb_dt.texture = panel_depth[PANEL_ASSETS];
            tb_dt.clear_depth = 1.0f;
            tb_dt.load_op = SDL_GPU_LOADOP_CLEAR;
            tb_dt.store_op = SDL_GPU_STOREOP_DONT_CARE;
            tb_dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
            tb_dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;

            tb_pass = SDL_BeginGPURenderPass(cmd_buf, &tb_ct, 1, &tb_dt);
            SDL_BindGPUGraphicsPipeline(tb_pass, mesh_pipeline);
            SDL_BindGPUVertexStorageBuffers(tb_pass, 0, &identity_bones, 1);

            for (ti = 0; ti < m->editor.pb_thumbnail_count; ti++) {
                pb_thumbnail_request *req = &m->editor.pb_thumbnails[ti];
                scene_model_asset *asset = NULL;
                GltfMesh *mesh;
                float radius;
                float fov_rad;
                float aspect;
                float dist;
                float near_plane;
                float far_plane;
                Vec3 center;
                Vec3 eye;
                Mat4 view_mat;
                Mat4 proj_mat;
                Mat4 model_mat;
                mesh_uniform_data mesh_uniforms;
                SDL_GPUViewport vp;
                SDL_Rect scissor;
                int ai;
                uint32_t p;

                /* Deferred in this pass: sprites only. */
                if (req->type == 3) continue;
                if (req->w < 1.0f || req->h < 1.0f) continue;

                if (req->type == 1) {
                    const char *mesh_key = NULL;

                    /* Find a scene mesh explicitly paired with this animation asset. */
                    {
                        int ai2;
                        for (ai2 = 0; ai2 < m->game.project.anim_count; ai2++) {
                            const project_anim *pa = &m->game.project.anims[ai2];
                            const project_mesh *pm;
                            if (!pa->asset[0]) continue;
                            if (strcmp(pa->asset, req->key) != 0) continue;
                            pm = project_find_mesh(&m->game.project, pa->entity);
                            if (pm && pm->model[0]) {
                                mesh_key = pm->model;
                                break;
                            }
                        }
                    }

                    /* Prefer the paired mesh (same rig relationship as scene setup). */
                    if (mesh_key) {
                        for (ai = 0; ai < m->game.scene_model_asset_count; ai++) {
                            scene_model_asset *cand = &m->game.scene_model_assets[ai];
                            if (strcmp(cand->key, mesh_key) != 0) continue;
                            if (!cand->loaded || cand->model.mesh.primitive_count == 0) continue;
                            if (!cand->has_skeleton) continue;
                            asset = cand;
                            break;
                        }
                        if (!asset) {
                            for (ai = 0; ai < m->game.scene_model_asset_count; ai++) {
                                scene_model_asset *cand = &m->game.scene_model_assets[ai];
                                if (strcmp(cand->key, mesh_key) != 0) continue;
                                if (!cand->loaded || cand->model.mesh.primitive_count == 0) continue;
                                asset = cand;
                                break;
                            }
                        }
                    }

                    /* Fallback: first loaded skinned mesh, then any loaded mesh. */
                    if (!asset) {
                        for (ai = 0; ai < m->game.scene_model_asset_count; ai++) {
                            scene_model_asset *cand = &m->game.scene_model_assets[ai];
                            if (!cand->loaded || cand->model.mesh.primitive_count == 0) continue;
                            if (!cand->has_skeleton) continue;
                            asset = cand;
                            break;
                        }
                    }
                    if (!asset) {
                        for (ai = 0; ai < m->game.scene_model_asset_count; ai++) {
                            scene_model_asset *cand = &m->game.scene_model_assets[ai];
                            if (!cand->loaded || cand->model.mesh.primitive_count == 0) continue;
                            asset = cand;
                            break;
                        }
                    }
                } else {
                    for (ai = 0; ai < m->game.scene_model_asset_count; ai++) {
                        if (strcmp(m->game.scene_model_assets[ai].key, req->key) == 0) {
                            asset = &m->game.scene_model_assets[ai];
                            break;
                        }
                    }
                }
                if (!asset || !asset->loaded || asset->model.mesh.primitive_count == 0) continue;

                mesh = &asset->model.mesh;
                radius = mesh->bounds_radius > 0.001f ? mesh->bounds_radius : 1.0f;
                center = VEC3(mesh->bounds_center[0], mesh->bounds_center[1], mesh->bounds_center[2]);

                fov_rad = 45.0f * 3.14159265f / 180.0f;
                aspect = req->w / req->h;
                if (aspect < 0.01f) aspect = 1.0f;

                dist = radius / sinf(fov_rad * 0.5f);
                eye = vec3_add(center, vec3_scale(vec3_normalize(VEC3(0.612f, 0.5f, 0.612f)), dist));

                near_plane = dist * 0.01f;
                if (near_plane < 0.01f) near_plane = 0.01f;
                far_plane = dist * 4.0f;
                if (far_plane <= near_plane + 0.01f) far_plane = near_plane + 10.0f;

                view_mat = mat4_look_at(eye, center, VEC3(0, 1, 0));
                proj_mat = mat4_perspective(fov_rad, aspect, near_plane, far_plane);
                model_mat = mat4_identity();

                memcpy(mesh_uniforms.projection, proj_mat.m, sizeof(mesh_uniforms.projection));
                memcpy(mesh_uniforms.view, view_mat.m, sizeof(mesh_uniforms.view));
                memcpy(mesh_uniforms.model, model_mat.m, sizeof(mesh_uniforms.model));
                mesh_uniforms.bone_offset = 0;
                SDL_PushGPUVertexUniformData(cmd_buf, 0, &mesh_uniforms, sizeof(mesh_uniforms));

                vp.x = req->x * display_density;
                vp.y = req->y * display_density;
                vp.w = req->w * display_density;
                vp.h = req->h * display_density;
                vp.min_depth = 0.0f;
                vp.max_depth = 1.0f;
                SDL_SetGPUViewport(tb_pass, &vp);

                scissor.x = (int)(req->x * display_density);
                scissor.y = (int)(req->y * display_density);
                scissor.w = (int)(req->w * display_density);
                scissor.h = (int)(req->h * display_density);
                if (scissor.x < 0) {
                    scissor.w += scissor.x;
                    scissor.x = 0;
                }
                if (scissor.y < 0) {
                    scissor.h += scissor.y;
                    scissor.y = 0;
                }
                if (scissor.x + scissor.w > (int)aw) {
                    scissor.w = (int)aw - scissor.x;
                }
                if (scissor.y + scissor.h > (int)ah) {
                    scissor.h = (int)ah - scissor.y;
                }
                if (scissor.w < 1 || scissor.h < 1) continue;
                SDL_SetGPUScissor(tb_pass, &scissor);

                for (p = 0; p < mesh->primitive_count; p++) {
                    GltfPrimitive *prim = &mesh->primitives[p];
                    SDL_GPUBufferBinding vbuf_binding = {0};
                    SDL_GPUBufferBinding ibuf_binding = {0};
                    SDL_GPUTextureSamplerBinding tex_bind = {0};

                    vbuf_binding.buffer = (SDL_GPUBuffer *)prim->vertex_buffer;
                    SDL_BindGPUVertexBuffers(tb_pass, 0, &vbuf_binding, 1);

                    ibuf_binding.buffer = (SDL_GPUBuffer *)prim->index_buffer;
                    SDL_BindGPUIndexBuffer(tb_pass, &ibuf_binding, SDL_GPU_INDEXELEMENTSIZE_16BIT);

                    tex_bind.texture = prim->texture ? (SDL_GPUTexture *)prim->texture : white_texture;
                    tex_bind.sampler = mesh_sampler;
                    SDL_BindGPUFragmentSamplers(tb_pass, 0, &tex_bind, 1);

                    SDL_DrawGPUIndexedPrimitives(tb_pass, prim->index_count, 1, 0, 0, 0);
                }
            }

            SDL_EndGPURenderPass(tb_pass);
            FRAME_CPU_ZONE_END();
        }
        FRAME_CPU_ZONE_END();
    }

    // --- INSPECTOR PANEL RENDER PASS (offscreen) ---
    if (panel_color[PANEL_INSPECTOR] && panel_depth[PANEL_INSPECTOR]) {
        FRAME_CPU_ZONE_BEGIN("render_inspector_panel");
        Uint32 iw = (Uint32)panel_tex_w[PANEL_INSPECTOR];
        Uint32 ih = (Uint32)panel_tex_h[PANEL_INSPECTOR];

        SDL_GPUColorTargetInfo in_ct = {0};
        in_ct.texture = panel_color[PANEL_INSPECTOR];
        in_ct.clear_color = (SDL_FColor){0.09f, 0.12f, 0.16f, 1.0f};
        in_ct.load_op = SDL_GPU_LOADOP_CLEAR;
        in_ct.store_op = SDL_GPU_STOREOP_STORE;

        SDL_GPUDepthStencilTargetInfo in_dt = {0};
        in_dt.texture = panel_depth[PANEL_INSPECTOR];
        in_dt.clear_depth = 1.0f;
        in_dt.load_op = SDL_GPU_LOADOP_CLEAR;
        in_dt.store_op = SDL_GPU_STOREOP_DONT_CARE;
        in_dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        in_dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;

        SDL_GPURenderPass *in_pass = SDL_BeginGPURenderPass(cmd_buf, &in_ct, 1, &in_dt);

        {
            uniform_data in_uniforms;
            float in_lw = (float)iw / display_density;
            float in_lh = (float)ih / display_density;
            float in_ortho[16] = {
                2.0f/in_lw,    0,             0,     0,
                0,            -2.0f/in_lh,    0,     0,
                0,             0,            -1.0f,  0,
               -1.0f,          1.0f,          0,     1.0f
            };
            float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            memcpy(in_uniforms.projection, in_ortho, sizeof(in_ortho));
            memcpy(in_uniforms.view, identity, sizeof(identity));
            ui_draw(in_pass, cmd_buf, &in_uniforms, &ui_inspector);
        }

        SDL_EndGPURenderPass(in_pass);
        FRAME_CPU_ZONE_END();
    }

    // --- CACHE PROFILER PANEL RENDER PASS (offscreen) ---
    if (panel_color[PANEL_CACHE_PROFILER] && panel_depth[PANEL_CACHE_PROFILER]) {
        FRAME_CPU_ZONE_BEGIN("render_cache_profiler_panel");
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
        FRAME_CPU_ZONE_END();
    }

    // --- CPU PROFILER PANEL RENDER PASS (offscreen) ---
    if (panel_color[PANEL_CPU_PROFILER] && panel_depth[PANEL_CPU_PROFILER]) {
        FRAME_CPU_ZONE_BEGIN("render_cpu_profiler_panel");
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
        FRAME_CPU_ZONE_END();
    }

    // --- EDITOR PANEL RENDER PASS (offscreen) ---
    if (m->editor.open && panel_color[PANEL_EDITOR] && panel_depth[PANEL_EDITOR]) {
        FRAME_CPU_ZONE_BEGIN("render_editor_panel");
        Uint32 ew = (Uint32)panel_tex_w[PANEL_EDITOR];
        Uint32 eh = (Uint32)panel_tex_h[PANEL_EDITOR];

        SDL_GPUColorTargetInfo ed_ct = {0};
        ed_ct.texture = panel_color[PANEL_EDITOR];
        ed_ct.clear_color = (SDL_FColor){0.15f, 0.18f, 0.22f, 1.0f};
        ed_ct.load_op = SDL_GPU_LOADOP_CLEAR;
        ed_ct.store_op = SDL_GPU_STOREOP_STORE;

        SDL_GPUDepthStencilTargetInfo ed_dt = {0};
        ed_dt.texture = panel_depth[PANEL_EDITOR];
        ed_dt.clear_depth = 1.0f;
        ed_dt.load_op = SDL_GPU_LOADOP_CLEAR;
        ed_dt.store_op = SDL_GPU_STOREOP_DONT_CARE;
        ed_dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        ed_dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;

        SDL_GPURenderPass *ed_pass = SDL_BeginGPURenderPass(cmd_buf, &ed_ct, 1, &ed_dt);
        {
            SDL_GPUViewport vp = {0};
            SDL_Rect sc = {0};
            vp.x = 0.0f;
            vp.y = 0.0f;
            vp.w = (float)ew;
            vp.h = (float)eh;
            vp.min_depth = 0.0f;
            vp.max_depth = 1.0f;
            sc.x = 0;
            sc.y = 0;
            sc.w = (int)ew;
            sc.h = (int)eh;
            SDL_SetGPUViewport(ed_pass, &vp);
            SDL_SetGPUScissor(ed_pass, &sc);
        }

        /* Editor camera matrices */
        float ed_aspect = (float)ew / (float)eh;
        Mat4 ed_proj;
        float ed_cp = cosf(m->editor.cam_pitch);
        Vec3 ed_fwd = VEC3(ed_cp * sinf(m->editor.cam_yaw),
                           sinf(m->editor.cam_pitch),
                           ed_cp * cosf(m->editor.cam_yaw));
        Mat4 ed_view = mat4_look_at(m->editor.cam_pos,
                                     vec3_add(m->editor.cam_pos, ed_fwd),
                                     VEC3(0, 1, 0));
        if (m->editor.cam_projection_mode == EDITOR_CAMERA_ORTHOGRAPHIC) {
            float ortho_size = m->editor.cam_ortho_size > 0.05f ? m->editor.cam_ortho_size : 12.0f;
            float half_h = ortho_size * 0.5f;
            float half_w = half_h * ed_aspect;
            ed_proj = mat4_orthographic(-half_w, half_w, -half_h, half_h, 0.1f, 200.0f);
        } else {
            ed_proj = mat4_perspective(60.0f * 3.14159265f / 180.0f, ed_aspect, 0.1f, 200.0f);
        }

        /* 1. 3D meshes (editor camera) */
        draw_scene_meshes(m, ed_pass, cmd_buf, ed_proj, ed_view);

        /* 2. Editor 3D lines (grid + gizmo) */
        if (editor_vert_count > 0 && editor_line_gpu_buf) {
            SDL_BindGPUGraphicsPipeline(ed_pass, editor_line_pipeline);

            uniform_data ed_line_u;
            memcpy(ed_line_u.projection, ed_proj.m, sizeof(float) * 16);
            memcpy(ed_line_u.view, ed_view.m, sizeof(float) * 16);
            SDL_PushGPUVertexUniformData(cmd_buf, 0, &ed_line_u, sizeof(ed_line_u));

            SDL_GPUBufferBinding ed_lb = {0};
            ed_lb.buffer = editor_line_gpu_buf;
            SDL_BindGPUVertexBuffers(ed_pass, 0, &ed_lb, 1);

            SDL_DrawGPUPrimitives(ed_pass, (Uint32)editor_vert_count, 1, 0, 0);
        }

        /* 3. Editor toolbar UI overlay */
        {
            uniform_data ed_ui_uniforms;
            float ed_lw = (float)ew / display_density;
            float ed_lh = (float)eh / display_density;
            float ed_ortho[16] = {
                2.0f/ed_lw,    0,             0,     0,
                0,            -2.0f/ed_lh,    0,     0,
                0,             0,            -1.0f,  0,
               -1.0f,          1.0f,          0,     1.0f
            };
            float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            memcpy(ed_ui_uniforms.projection, ed_ortho, sizeof(ed_ortho));
            memcpy(ed_ui_uniforms.view, identity, sizeof(identity));
            ui_draw(ed_pass, cmd_buf, &ed_ui_uniforms, &ui_editor_toolbar);
        }

        SDL_EndGPURenderPass(ed_pass);
        FRAME_CPU_ZONE_END();
    }

    // ================================================================
    // COMPOSITE PASS — per-window: draw headers + panel textures into each swapchain
    // ================================================================
    FRAME_CPU_ZONE_BEGIN("composite_windows");
    {
        int cwi;
        for (cwi = 0; cwi < MAX_DOCK_WINDOWS; cwi++) {
            SDL_GPUTexture *swapchain_texture = NULL;
            Uint32 sc_w, sc_h;
            SDL_Window *dw;
            SDL_GPURenderPass *comp_pass;
            SDL_GPUColorTargetInfo comp_ct;

            if (!dock->windows[cwi].in_use || !dock->windows[cwi].sdl_window) continue;
            if (comp_data[cwi].quad_count <= 0 || !comp_data[cwi].gpu_buf) continue;

            dw = (SDL_Window *)dock->windows[cwi].sdl_window;
            if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd_buf, dw, &swapchain_texture, &sc_w, &sc_h)
                || !swapchain_texture) continue;

            memset(&comp_ct, 0, sizeof(comp_ct));
            comp_ct.texture = swapchain_texture;
            comp_ct.clear_color = (SDL_FColor){0.08f, 0.08f, 0.10f, 1.0f};
            comp_ct.load_op = SDL_GPU_LOADOP_CLEAR;
            comp_ct.store_op = SDL_GPU_STOREOP_STORE;

            comp_pass = SDL_BeginGPURenderPass(cmd_buf, &comp_ct, 1, NULL);

            if (composite_pipeline) {
                int qi;
                SDL_GPUBufferBinding comp_vb;

                SDL_BindGPUGraphicsPipeline(comp_pass, composite_pipeline);

                memset(&comp_vb, 0, sizeof(comp_vb));
                comp_vb.buffer = comp_data[cwi].gpu_buf;
                SDL_BindGPUVertexBuffers(comp_pass, 0, &comp_vb, 1);

                /* Draw quads: each quad has a type (header or panel) and a panel ID.
                   Bind the appropriate texture per quad. */
                for (qi = 0; qi < comp_data[cwi].quad_count; qi++) {
                    CompQuadInfo *cq = &comp_data[cwi].quads[qi];
                    SDL_GPUTextureSamplerBinding tex_bind = {0};

                    if (cq->type == CQUAD_HEADER) {
                        if (!header_textures[cq->panel]) continue;
                        tex_bind.texture = header_textures[cq->panel];
                    } else if (cq->type == CQUAD_DROP_ZONE) {
                        if (!white_texture) continue;
                        tex_bind.texture = white_texture;
                    } else {
                        if (!panel_color[cq->panel]) continue;
                        tex_bind.texture = panel_color[cq->panel];
                    }
                    tex_bind.sampler = composite_sampler;
                    SDL_BindGPUFragmentSamplers(comp_pass, 0, &tex_bind, 1);
                    SDL_DrawGPUPrimitives(comp_pass, 6, 1, (Uint32)(qi * 6), 0);
                }
            }

            if (cwi == 0 && (ui_menu_bar.rect_vert_count > 0 ||
                             ui_menu_bar.font_vert_count > 0 ||
                             ui_menu_bar.icon_font_vert_count > 0)) {
                uniform_data menu_uniforms;
                float menu_lw = (float)sc_w / display_density;
                float menu_lh = (float)sc_h / display_density;
                float menu_ortho[16] = {
                    2.0f/menu_lw,  0,             0,     0,
                    0,            -2.0f/menu_lh,  0,     0,
                    0,             0,            -1.0f,  0,
                   -1.0f,          1.0f,          0,     1.0f
                };
                float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
                memcpy(menu_uniforms.projection, menu_ortho, sizeof(menu_ortho));
                memcpy(menu_uniforms.view, identity, sizeof(identity));
                ui_draw(comp_pass, cmd_buf, &menu_uniforms, &ui_menu_bar);
            }

            SDL_EndGPURenderPass(comp_pass);
        }
    }
    FRAME_CPU_ZONE_END();

    FRAME_CPU_ZONE_BEGIN("gpu_submit");
    SDL_SubmitGPUCommandBuffer(cmd_buf);
    FRAME_CPU_ZONE_END();

    // Release per-frame GPU buffers
    FRAME_CPU_ZONE_BEGIN("release_frame_resources");
    draw_upload_release(gpu_device, &draw_upload);
    if (editor_line_gpu_buf) SDL_ReleaseGPUBuffer(gpu_device, editor_line_gpu_buf);
    {
        int cwi;
        for (cwi = 0; cwi < MAX_DOCK_WINDOWS; cwi++) {
            if (comp_data[cwi].gpu_buf) {
                SDL_ReleaseGPUBuffer(gpu_device, comp_data[cwi].gpu_buf);
                comp_data[cwi].gpu_buf = NULL;
            }
        }
    }
    ui_release_buffers(&ui_game);
    ui_release_buffers(&ui_profiler);
    ui_release_buffers(&ui_scene_tree);
    ui_release_buffers(&ui_project_browser);
    ui_release_buffers(&ui_inspector);
    ui_release_buffers(&ui_menu_bar);
    ui_release_buffers(&ui_editor_toolbar);
    if (grid_quad_buf) { SDL_ReleaseGPUBuffer(gpu_device, grid_quad_buf); grid_quad_buf = NULL; }
    FRAME_CPU_ZONE_END();

    TracyCZoneEnd(ctx_update);
    TracyCFrameMark;
}

// ---------------------------------------------------------------------------
// end_externals
// ---------------------------------------------------------------------------

EXPORT void end_externals(struct memory *m) {
    dock_state *dock = (dock_state *)m->editor.dock;
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

    // Release editor mouse look if active
    if (m->editor.cam_mouse_look && window) {
        SDL_SetWindowRelativeMouseMode(window, false);
        m->editor.cam_mouse_look = 0;
    }

    // Panel offscreen textures
    {
        int i;
        for (i = 0; i < PANEL_COUNT; i++) {
            if (panel_color[i]) { SDL_ReleaseGPUTexture(gpu_device, panel_color[i]); panel_color[i] = NULL; }
            if (panel_depth[i]) { SDL_ReleaseGPUTexture(gpu_device, panel_depth[i]); panel_depth[i] = NULL; }
            panel_tex_w[i] = 0;
            panel_tex_h[i] = 0;
        }
    }

    // Composite pipeline + sampler + header textures
    if (composite_pipeline) { SDL_ReleaseGPUGraphicsPipeline(gpu_device, composite_pipeline); composite_pipeline = NULL; }
    if (composite_sampler)  { SDL_ReleaseGPUSampler(gpu_device, composite_sampler); composite_sampler = NULL; }
    // Grid texture pipeline + resources
    if (grid_tex_pipeline) { SDL_ReleaseGPUGraphicsPipeline(gpu_device, grid_tex_pipeline); grid_tex_pipeline = NULL; }
    if (grid_sampler)      { SDL_ReleaseGPUSampler(gpu_device, grid_sampler); grid_sampler = NULL; }
    if (grid_texture)      { SDL_ReleaseGPUTexture(gpu_device, grid_texture); grid_texture = NULL; }
    if (grid_quad_buf)     { SDL_ReleaseGPUBuffer(gpu_device, grid_quad_buf); grid_quad_buf = NULL; }
    {
        int i;
        for (i = 0; i < PANEL_COUNT; i++) {
            if (header_textures[i]) { SDL_ReleaseGPUTexture(gpu_device, header_textures[i]); header_textures[i] = NULL; }
        }
    }
    if (editor_line_pipeline) {
        SDL_ReleaseGPUGraphicsPipeline(gpu_device, editor_line_pipeline);
        editor_line_pipeline = NULL;
    }

    // Release HarfBuzz
    if (hb_editor_font) { hb_font_destroy(hb_editor_font); hb_editor_font = NULL; }
    if (hb_editor_face) { hb_face_destroy(hb_editor_face); hb_editor_face = NULL; }
    if (hb_editor_blob) { hb_blob_destroy(hb_editor_blob); hb_editor_blob = NULL; }

    // Release font MSDF atlas
    if (font_atlas_texture) { SDL_ReleaseGPUTexture(gpu_device, font_atlas_texture); font_atlas_texture = NULL; }
    if (font_atlas_sampler) { SDL_ReleaseGPUSampler(gpu_device, font_atlas_sampler); font_atlas_sampler = NULL; }
    if (icon_atlas_texture) { SDL_ReleaseGPUTexture(gpu_device, icon_atlas_texture); icon_atlas_texture = NULL; }
    if (icon_atlas_sampler) { SDL_ReleaseGPUSampler(gpu_device, icon_atlas_sampler); icon_atlas_sampler = NULL; }
    icon_glyph_count = 0;

    if (font_ttf_buffer) { SDL_free(font_ttf_buffer); font_ttf_buffer = NULL; font_ttf_size = 0; }
    if (icon_ttf_buffer) { SDL_free(icon_ttf_buffer); icon_ttf_buffer = NULL; icon_ttf_size = 0; }

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
    /* depth_texture replaced by panel_depth[] — already freed above */
    if (white_texture) { SDL_ReleaseGPUTexture(gpu_device, white_texture); white_texture = NULL; }
    if (bone_storage_buffer) { SDL_ReleaseGPUBuffer(gpu_device, bone_storage_buffer); bone_storage_buffer = NULL; }
    if (bone_identity_buffer) { SDL_ReleaseGPUBuffer(gpu_device, bone_identity_buffer); bone_identity_buffer = NULL; }

    // Release glTF model GPU resources (all scene model assets)
    {
        int ai;
        for (ai = 0; ai < m->game.scene_model_asset_count; ai++) {
            scene_model_asset *asset = &m->game.scene_model_assets[ai];
            uint32_t p;
            if (!asset->loaded) continue;
            for (p = 0; p < asset->model.mesh.primitive_count; p++) {
                GltfPrimitive *prim = &asset->model.mesh.primitives[p];
                if (prim->vertex_buffer) SDL_ReleaseGPUBuffer(gpu_device, (SDL_GPUBuffer *)prim->vertex_buffer);
                if (prim->index_buffer) SDL_ReleaseGPUBuffer(gpu_device, (SDL_GPUBuffer *)prim->index_buffer);
                if (prim->texture) SDL_ReleaseGPUTexture(gpu_device, (SDL_GPUTexture *)prim->texture);
            }
        }
    }

    // Shadercross
    SDL_ShaderCross_Quit();

    // Destroy tear-off windows (indices 1+)
    {
        int i;
        for (i = 1; i < MAX_DOCK_WINDOWS; i++) {
            if (dock->windows[i].in_use && dock->windows[i].sdl_window) {
                SDL_ReleaseWindowFromGPUDevice(gpu_device, (SDL_Window *)dock->windows[i].sdl_window);
                SDL_DestroyWindow((SDL_Window *)dock->windows[i].sdl_window);
                dock->windows[i].in_use = 0;
                dock->windows[i].sdl_window = NULL;
            }
        }
    }

    /* Shutdown profilers */
    cache_prof_shutdown();
    cpu_prof_shutdown();

    // Main window & device
    if (gpu_device) {
        if (window) {
            SDL_ReleaseWindowFromGPUDevice(gpu_device, window);
        }
        SDL_DestroyGPUDevice(gpu_device);
        gpu_device = NULL;
    }
    if (window) {
        SDL_DestroyWindow(window);
        window = NULL;
    }

    // Free arena (single free for all engine memory)
    if (m->arena.base) {
        free(m->arena.base);
        m->arena.base = NULL;
    }

    SDL_Quit();
}

// ---------------------------------------------------------------------------
// Engine callbacks
// ---------------------------------------------------------------------------

EXPORT void init_engine(struct memory *m) {
    g_init(&m->game);
}

EXPORT void destroy_engine(struct memory *m) {
    g_destroy(&m->game);
}

EXPORT void assign_init(engine_init_fn func) {
    g_init = func;
}

EXPORT void assign_destroy(engine_destroy_fn func) {
    g_destroy = func;
}

EXPORT void assign_update(engine_update_fn func) {
    g_update = func;
}

// ---------------------------------------------------------------------------
// Editor callbacks
// ---------------------------------------------------------------------------

EXPORT void init_editor(struct memory *m) {
    if (g_editor_init) g_editor_init(&m->game, &m->editor);
}

EXPORT void destroy_editor(struct memory *m) {
    if (g_editor_destroy) g_editor_destroy(&m->game, &m->editor);
}

EXPORT void assign_editor_init(editor_init_fn func) {
    g_editor_init = func;
}

EXPORT void assign_editor_destroy(editor_destroy_fn func) {
    g_editor_destroy = func;
}

EXPORT void assign_editor_update(editor_update_fn func) {
    g_editor_update = func;
}

EXPORT void assign_editor_handle_event(editor_handle_event_fn func) {
    g_editor_handle_event = func;
}

/* Profiler zone wrappers — called by core.dll, forwarded to engine.dll via function pointers */
EXPORT void ext_cache_zone_begin(const char *name) { cache_zone_begin(name); }
EXPORT void ext_cache_zone_end(void) { cache_zone_end(); }
EXPORT void ext_cpu_zone_begin(const char *name) {
    if (!cpu_prof_capture_paused) cpu_zone_begin(name);
}
EXPORT void ext_cpu_zone_end(void) {
    if (!cpu_prof_capture_paused) cpu_zone_end();
}
EXPORT void ext_cache_prof_frame_reset(void) { cache_prof_frame_reset(); }
EXPORT void ext_cpu_prof_frame_end(void) {
    if (!cpu_prof_capture_paused) cpu_prof_frame_end();
}
