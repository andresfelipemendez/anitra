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

// ---------------------------------------------------------------------------
// Uniform data (projection + view, matching HLSL cbuffer)
// ---------------------------------------------------------------------------

struct uniform_data {
    float projection[16];
    float view[16];
};

// ---------------------------------------------------------------------------
// Helper: compile HLSL -> SPIRV -> SDL_GPUShader
// ---------------------------------------------------------------------------

static SDL_GPUShader* compile_shader_from_hlsl(
    const char* filename,
    const char* entrypoint,
    SDL_ShaderCross_ShaderStage stage)
{
    // Read HLSL source from disk
    size_t file_size = 0;
    void* file_data = SDL_LoadFile(filename, &file_size);
    if (!file_data) {
        fprintf(stderr, "Failed to load shader file: %s (%s)\n", filename, SDL_GetError());
        return NULL;
    }

    // Compile HLSL -> SPIRV
    SDL_ShaderCross_HLSL_Info hlsl_info = {};
    hlsl_info.source = (const char*)file_data;
    hlsl_info.entrypoint = entrypoint;
    hlsl_info.include_dir = NULL;
    hlsl_info.defines = NULL;
    hlsl_info.shader_stage = stage;
    hlsl_info.props = 0;

    size_t spirv_size = 0;
    void* spirv_bytecode = SDL_ShaderCross_CompileSPIRVFromHLSL(&hlsl_info, &spirv_size);
    SDL_free(file_data);

    if (!spirv_bytecode) {
        fprintf(stderr, "Failed to compile HLSL to SPIRV: %s entry=%s (%s)\n",
                filename, entrypoint, SDL_GetError());
        return NULL;
    }

    // Reflect to get resource info
    SDL_ShaderCross_GraphicsShaderMetadata* metadata =
        SDL_ShaderCross_ReflectGraphicsSPIRV((const Uint8*)spirv_bytecode, spirv_size, 0);
    if (!metadata) {
        fprintf(stderr, "Failed to reflect SPIRV: %s entry=%s (%s)\n",
                filename, entrypoint, SDL_GetError());
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
        fprintf(stderr, "Failed to compile GPU shader: %s entry=%s (%s)\n",
                filename, entrypoint, SDL_GetError());
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
    SDL_GPUShader* sprite_vs = compile_shader_from_hlsl(
        "assets\\shaders\\sprite_vs.hlsl", "VSMain", SDL_SHADERCROSS_SHADERSTAGE_VERTEX);
    SDL_GPUShader* sprite_fs = compile_shader_from_hlsl(
        "assets\\shaders\\sprite_fs.hlsl", "PSMain", SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT);
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
    SDL_GPUShader* line_vs = compile_shader_from_hlsl(
        "assets\\shaders\\debug_lines_vs.hlsl", "VSMain", SDL_SHADERCROSS_SHADERSTAGE_VERTEX);
    SDL_GPUShader* line_fs = compile_shader_from_hlsl(
        "assets\\shaders\\debug_lines_fs.hlsl", "PSMain", SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT);
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

    // 10. Create sampler (nearest-neighbor for pixel art)
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
    gpu_textures[TEXTURE_PLAYER]      = load_gpu_texture("assets\\char_spritesheet.png");
    gpu_textures[TEXTURE_TILES]       = load_gpu_texture("assets\\Dungeon_Tileset.png");
    gpu_textures[TEXTURE_SLIME]       = load_gpu_texture("assets\\pinkslime_spritesheet.png");
    gpu_textures[TEXTURE_HEALTH_BAR]  = load_gpu_texture("assets\\health_bar_hud.png");
    gpu_textures[TEXTURE_HEALTH_FILL] = load_gpu_texture("assets\\health_hud.png");

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

    SDL_EndGPURenderPass(render_pass);
    SDL_SubmitGPUCommandBuffer(cmd_buf);

    // Release per-frame GPU buffers
    if (sprite_gpu_buf) SDL_ReleaseGPUBuffer(gpu_device, sprite_gpu_buf);
    if (line_gpu_buf)   SDL_ReleaseGPUBuffer(gpu_device, line_gpu_buf);
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

    // Release pipelines
    if (sprite_pipeline) {
        SDL_ReleaseGPUGraphicsPipeline(gpu_device, sprite_pipeline);
        sprite_pipeline = NULL;
    }
    if (line_pipeline) {
        SDL_ReleaseGPUGraphicsPipeline(gpu_device, line_pipeline);
        line_pipeline = NULL;
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
