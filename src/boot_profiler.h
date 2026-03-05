#ifndef BOOT_PROFILER_H
#define BOOT_PROFILER_H

/*
 * boot_profiler.h — nanoprof-based startup profiler for Anitra
 *
 * Instruments every significant phase from init_externals() through
 * init_engine() and init_editor() to find boot-time bottlenecks.
 *
 * Output: build/Debug/boot.nanoprof
 * View:   timedilator-2 view build/Debug/boot.nanoprof
 *    or:  cat boot.nanoprof | nanoprof2chrome.d > boot.json
 *         then open about://tracing in Chromium
 */

#ifdef BOOT_PROFILER_IMPL
#include <nanoprof.h>
#endif

/* ── Timepoint IDs ──────────────────────────────────────────────── */

enum {
    /* Level 0: top-level phases */
    TP_BOOT = 0,

    /* Level 1: major init functions */
    TP_INIT_EXTERNALS,
    TP_INIT_ENGINE,
    TP_INIT_EDITOR,

    /* Level 2: init_externals sub-phases */
    TP_EXT_ALLOC_MEMORY,
    TP_EXT_LOAD_PROJECT,
    TP_EXT_SDL_INIT,
    TP_EXT_SHADERCROSS_INIT,
    TP_EXT_CREATE_WINDOW,
    TP_EXT_CREATE_GPU_DEVICE,
    TP_EXT_CLAIM_WINDOW,
    TP_EXT_PROFILERS_INIT,

    /* Level 2: shader compilation */
    TP_EXT_SHADER_SPRITE,
    TP_EXT_SHADER_DEBUG_LINES,
    TP_EXT_SHADER_EDITOR_LINES,
    TP_EXT_SHADER_UI_RECT,
    TP_EXT_SHADER_FONT,
    TP_EXT_SHADER_MESH,
    TP_EXT_SHADER_COMPOSITE,
    TP_EXT_SHADER_GRID,

    /* Level 2: pipeline creation */
    TP_EXT_PIPELINE_SPRITE,
    TP_EXT_PIPELINE_DEBUG_LINES,
    TP_EXT_PIPELINE_EDITOR_LINES,
    TP_EXT_PIPELINE_UI_RECT,
    TP_EXT_PIPELINE_FONT,
    TP_EXT_PIPELINE_MESH,
    TP_EXT_PIPELINE_COMPOSITE,
    TP_EXT_PIPELINE_GRID,

    /* Level 2: resource creation */
    TP_EXT_CREATE_SAMPLERS,
    TP_EXT_CREATE_BONE_BUFFERS,
    TP_EXT_CREATE_WHITE_TEXTURE,

    /* Level 2: texture loading */
    TP_EXT_LOAD_TEXTURES,
    TP_EXT_LOAD_TEXTURE_PLAYER,
    TP_EXT_LOAD_TEXTURE_TILES,
    TP_EXT_LOAD_TEXTURE_SLIME,
    TP_EXT_LOAD_TEXTURE_HEALTH_BAR,
    TP_EXT_LOAD_TEXTURE_HEALTH_FILL,

    /* Level 2: font loading */
    TP_EXT_FONT_LOAD_MSDF,
    TP_EXT_HARFBUZZ_INIT,
    TP_EXT_ICON_FONT_LOAD,
    TP_EXT_FONT_ADVANCE_TABLE,

    /* Level 3: font_load_msdf sub-phases */
    TP_FONT_FILE_LOAD,        /* SDL_LoadFile + stbtt_InitFont */
    TP_FONT_ATLAS_BUILD,      /* msdf_build_atlas (SDF generation) */
    TP_FONT_GPU_UPLOAD,       /* GPU texture + transfer + upload */
    TP_FONT_SAMPLER,          /* create sampler */

    /* Level 3: icon_font_load_msdf sub-phases */
    TP_ICON_FILE_LOAD,
    TP_ICON_ATLAS_BUILD,
    TP_ICON_GPU_UPLOAD,

    /* Level 2: arena / UI / dock */
    TP_EXT_ARENA_INIT,
    TP_EXT_CLAY_INIT,
    TP_EXT_DOCK_INIT,
    TP_EXT_HEADER_TEXTURES,
    TP_EXT_RENDERING_ARENA,
    TP_EXT_GAMEPLAY_ARENA,
    TP_EXT_PANEL_TEXTURES,

    /* Level 2: init_engine sub-phases */
    TP_ENG_MIGRATION,
    TP_ENG_REGISTER_SYSTEMS,
    TP_ENG_SCENE_STORAGE,
    TP_ENG_ANIM_POOL,
    TP_ENG_BUILD_SCENE,
    TP_ENG_LOAD_MODEL_ASSETS,
    TP_ENG_SYNC_MESH,
    TP_ENG_INIT_POSE,
    TP_ENG_ANIM_SM_SETUP,

    /* Level 2: init_editor sub-phases */
    TP_ED_MIGRATION,
    TP_ED_DEFAULTS,

    /* Level 1: first frame (from init_editor end to GPU submit) */
    TP_FIRST_FRAME,

    /* Level 2: first frame sub-phases */
    TP_FRAME_EDITOR_UPDATE,
    TP_FRAME_ENGINE_UPDATE,
    TP_FRAME_GPU_RENDER,
    TP_FRAME_GPU_SUBMIT,

    /* Level 4: sub-phases inside load_glb / load_animations_glb */
    TP_SUB_PARSE,         /* cgltf_parse_file + load_buffers */
    TP_SUB_MESHES,        /* extract_mesh loop (all mesh nodes) */
    TP_SUB_SKELETON,      /* extract_skeleton */
    TP_SUB_CLIPS,         /* extract_anim_clip loop */
    TP_SUB_REMAP,         /* joint remap (load_animations_glb) */

    /* Level 5: sub-phases inside extract_mesh (per-primitive) */
    TP_SUB_VTX,           /* vertex decode + GPU upload */
    TP_SUB_IDX,           /* index extract + GPU upload */
    TP_SUB_TEX,           /* texture decode + GPU upload */

    /* Level 3: per-asset loads (dynamically registered by engine) */
    TP_ASSET_BASE,
    TP_ASSET_MAX = TP_ASSET_BASE + 255,

    TP_COUNT  /* must be last */
};

/* ── Global profiler instance (lives in externals.dll) ────────── */

#ifdef BOOT_PROFILER_IMPL

static struct nanoprof *g_boot_prof = NULL;
static FILE            *g_boot_prof_file = NULL;

static void boot_prof_create(void) {
    g_boot_prof_file = fopen("build/Debug/boot.nanoprof", "wb");
    if (!g_boot_prof_file) {
        fprintf(stderr, "[boot_prof] failed to open output file\n");
        return;
    }
    g_boot_prof = (struct nanoprof *)malloc(sizeof(struct nanoprof));
    if (!g_boot_prof) {
        fclose(g_boot_prof_file);
        g_boot_prof_file = NULL;
        return;
    }
    *g_boot_prof = nanoprof_create(4 * 1024 * 1024, g_boot_prof_file);

    /* Register all timepoints with names and nest levels */

    /* Level 0 */
    nanoprof_event_register(g_boot_prof, TP_BOOT, 0, "boot");

    /* Level 1 */
    nanoprof_event_register(g_boot_prof, TP_INIT_EXTERNALS,  1, "init_externals");
    nanoprof_event_register(g_boot_prof, TP_INIT_ENGINE,     1, "init_engine");
    nanoprof_event_register(g_boot_prof, TP_INIT_EDITOR,     1, "init_editor");

    /* Level 2: init_externals sub-phases */
    nanoprof_event_register(g_boot_prof, TP_EXT_ALLOC_MEMORY,     2, "alloc_memory");
    nanoprof_event_register(g_boot_prof, TP_EXT_LOAD_PROJECT,     2, "load_project");
    nanoprof_event_register(g_boot_prof, TP_EXT_SDL_INIT,         2, "SDL_Init");
    nanoprof_event_register(g_boot_prof, TP_EXT_SHADERCROSS_INIT, 2, "ShaderCross_Init");
    nanoprof_event_register(g_boot_prof, TP_EXT_CREATE_WINDOW,    2, "CreateWindow");
    nanoprof_event_register(g_boot_prof, TP_EXT_CREATE_GPU_DEVICE,2, "CreateGPUDevice");
    nanoprof_event_register(g_boot_prof, TP_EXT_CLAIM_WINDOW,     2, "ClaimWindow");
    nanoprof_event_register(g_boot_prof, TP_EXT_PROFILERS_INIT,   2, "profilers_init");

    /* Level 2: shader compilation */
    nanoprof_event_register(g_boot_prof, TP_EXT_SHADER_SPRITE,       2, "shader:sprite");
    nanoprof_event_register(g_boot_prof, TP_EXT_SHADER_DEBUG_LINES,  2, "shader:debug_lines");
    nanoprof_event_register(g_boot_prof, TP_EXT_SHADER_EDITOR_LINES, 2, "shader:editor_lines");
    nanoprof_event_register(g_boot_prof, TP_EXT_SHADER_UI_RECT,      2, "shader:ui_rect");
    nanoprof_event_register(g_boot_prof, TP_EXT_SHADER_FONT,         2, "shader:font");
    nanoprof_event_register(g_boot_prof, TP_EXT_SHADER_MESH,         2, "shader:mesh");
    nanoprof_event_register(g_boot_prof, TP_EXT_SHADER_COMPOSITE,    2, "shader:composite");
    nanoprof_event_register(g_boot_prof, TP_EXT_SHADER_GRID,         2, "shader:grid");

    /* Level 2: pipeline creation */
    nanoprof_event_register(g_boot_prof, TP_EXT_PIPELINE_SPRITE,       2, "pipeline:sprite");
    nanoprof_event_register(g_boot_prof, TP_EXT_PIPELINE_DEBUG_LINES,  2, "pipeline:debug_lines");
    nanoprof_event_register(g_boot_prof, TP_EXT_PIPELINE_EDITOR_LINES, 2, "pipeline:editor_lines");
    nanoprof_event_register(g_boot_prof, TP_EXT_PIPELINE_UI_RECT,      2, "pipeline:ui_rect");
    nanoprof_event_register(g_boot_prof, TP_EXT_PIPELINE_FONT,         2, "pipeline:font");
    nanoprof_event_register(g_boot_prof, TP_EXT_PIPELINE_MESH,         2, "pipeline:mesh");
    nanoprof_event_register(g_boot_prof, TP_EXT_PIPELINE_COMPOSITE,    2, "pipeline:composite");
    nanoprof_event_register(g_boot_prof, TP_EXT_PIPELINE_GRID,         2, "pipeline:grid");

    /* Level 2: resource creation */
    nanoprof_event_register(g_boot_prof, TP_EXT_CREATE_SAMPLERS,      2, "create_samplers");
    nanoprof_event_register(g_boot_prof, TP_EXT_CREATE_BONE_BUFFERS,  2, "create_bone_buffers");
    nanoprof_event_register(g_boot_prof, TP_EXT_CREATE_WHITE_TEXTURE, 2, "create_white_texture");

    /* Level 2: texture loading */
    nanoprof_event_register(g_boot_prof, TP_EXT_LOAD_TEXTURES,          2, "load_textures");
    nanoprof_event_register(g_boot_prof, TP_EXT_LOAD_TEXTURE_PLAYER,    3, "tex:player");
    nanoprof_event_register(g_boot_prof, TP_EXT_LOAD_TEXTURE_TILES,     3, "tex:tiles");
    nanoprof_event_register(g_boot_prof, TP_EXT_LOAD_TEXTURE_SLIME,     3, "tex:slime");
    nanoprof_event_register(g_boot_prof, TP_EXT_LOAD_TEXTURE_HEALTH_BAR,3, "tex:health_bar");
    nanoprof_event_register(g_boot_prof, TP_EXT_LOAD_TEXTURE_HEALTH_FILL,3,"tex:health_fill");

    /* Level 2: font loading */
    nanoprof_event_register(g_boot_prof, TP_EXT_FONT_LOAD_MSDF,     2, "font_load_msdf");
    nanoprof_event_register(g_boot_prof, TP_EXT_HARFBUZZ_INIT,      2, "harfbuzz_init");
    nanoprof_event_register(g_boot_prof, TP_EXT_ICON_FONT_LOAD,     2, "icon_font_load");
    nanoprof_event_register(g_boot_prof, TP_EXT_FONT_ADVANCE_TABLE, 2, "font_advance_table");

    /* Level 3: font_load_msdf sub-phases */
    nanoprof_event_register(g_boot_prof, TP_FONT_FILE_LOAD,   3, "font:file_load");
    nanoprof_event_register(g_boot_prof, TP_FONT_ATLAS_BUILD, 3, "font:atlas_build");
    nanoprof_event_register(g_boot_prof, TP_FONT_GPU_UPLOAD,  3, "font:gpu_upload");
    nanoprof_event_register(g_boot_prof, TP_FONT_SAMPLER,     3, "font:sampler");

    /* Level 3: icon_font sub-phases */
    nanoprof_event_register(g_boot_prof, TP_ICON_FILE_LOAD,   3, "icon:file_load");
    nanoprof_event_register(g_boot_prof, TP_ICON_ATLAS_BUILD, 3, "icon:atlas_build");
    nanoprof_event_register(g_boot_prof, TP_ICON_GPU_UPLOAD,  3, "icon:gpu_upload");

    /* Level 2: arena / UI / dock */
    nanoprof_event_register(g_boot_prof, TP_EXT_ARENA_INIT,       2, "arena_init");
    nanoprof_event_register(g_boot_prof, TP_EXT_CLAY_INIT,        2, "clay_init");
    nanoprof_event_register(g_boot_prof, TP_EXT_DOCK_INIT,        2, "dock_init");
    nanoprof_event_register(g_boot_prof, TP_EXT_HEADER_TEXTURES,  2, "header_textures");
    nanoprof_event_register(g_boot_prof, TP_EXT_RENDERING_ARENA,  2, "rendering_arena");
    nanoprof_event_register(g_boot_prof, TP_EXT_GAMEPLAY_ARENA,   2, "gameplay_arena");
    nanoprof_event_register(g_boot_prof, TP_EXT_PANEL_TEXTURES,   2, "panel_textures");

    /* Level 2: init_engine sub-phases */
    nanoprof_event_register(g_boot_prof, TP_ENG_MIGRATION,        2, "eng:migration");
    nanoprof_event_register(g_boot_prof, TP_ENG_REGISTER_SYSTEMS, 2, "eng:register_systems");
    nanoprof_event_register(g_boot_prof, TP_ENG_SCENE_STORAGE,    2, "eng:scene_storage");
    nanoprof_event_register(g_boot_prof, TP_ENG_ANIM_POOL,        2, "eng:anim_pool");
    nanoprof_event_register(g_boot_prof, TP_ENG_BUILD_SCENE,      2, "eng:build_scene");
    nanoprof_event_register(g_boot_prof, TP_ENG_LOAD_MODEL_ASSETS,2, "eng:load_model_assets");
    nanoprof_event_register(g_boot_prof, TP_ENG_SYNC_MESH,        2, "eng:sync_mesh");
    nanoprof_event_register(g_boot_prof, TP_ENG_INIT_POSE,        2, "eng:init_pose");
    nanoprof_event_register(g_boot_prof, TP_ENG_ANIM_SM_SETUP,    2, "eng:anim_sm_setup");

    /* Level 2: init_editor sub-phases */
    nanoprof_event_register(g_boot_prof, TP_ED_MIGRATION, 2, "ed:migration");
    nanoprof_event_register(g_boot_prof, TP_ED_DEFAULTS,  2, "ed:defaults");

    /* Level 1: first frame */
    nanoprof_event_register(g_boot_prof, TP_FIRST_FRAME,         1, "first_frame");
    nanoprof_event_register(g_boot_prof, TP_FRAME_EDITOR_UPDATE, 2, "frame:editor_update");
    nanoprof_event_register(g_boot_prof, TP_FRAME_ENGINE_UPDATE, 2, "frame:engine_update");
    nanoprof_event_register(g_boot_prof, TP_FRAME_GPU_RENDER,    2, "frame:gpu_render");
    nanoprof_event_register(g_boot_prof, TP_FRAME_GPU_SUBMIT,    2, "frame:gpu_submit");

    /* Level 4: loader sub-phases */
    nanoprof_event_register(g_boot_prof, TP_SUB_PARSE,    4, "parse");
    nanoprof_event_register(g_boot_prof, TP_SUB_MESHES,   4, "meshes");
    nanoprof_event_register(g_boot_prof, TP_SUB_SKELETON,  4, "skeleton");
    nanoprof_event_register(g_boot_prof, TP_SUB_CLIPS,    4, "clips");
    nanoprof_event_register(g_boot_prof, TP_SUB_REMAP,    4, "remap");

    /* Level 5: per-primitive sub-phases */
    nanoprof_event_register(g_boot_prof, TP_SUB_VTX, 5, "vtx");
    nanoprof_event_register(g_boot_prof, TP_SUB_IDX, 5, "idx");
    nanoprof_event_register(g_boot_prof, TP_SUB_TEX, 5, "tex");

    printf("[boot_prof] profiler created (%d events)\n", TP_COUNT);
}

static void boot_prof_destroy(void) {
    if (g_boot_prof) {
        nanoprof_destroy(g_boot_prof);
        free(g_boot_prof);
        g_boot_prof = NULL;
    }
    if (g_boot_prof_file) {
        fclose(g_boot_prof_file);
        g_boot_prof_file = NULL;
    }
    printf("[boot_prof] profile written to build/Debug/boot.nanoprof\n");
}

#endif /* BOOT_PROFILER_IMPL */

/* ── Convenience macros ───────────────────────────────────────── */

#define BOOT_PROF_BEGIN(id)  do { if (g_boot_prof) nanoprof_event_timepoint(g_boot_prof, (id)); } while(0)
#define BOOT_PROF_END(id)    do { if (g_boot_prof) nanoprof_event_timepoint_end(g_boot_prof, (id)); } while(0)

#endif /* BOOT_PROFILER_H */
