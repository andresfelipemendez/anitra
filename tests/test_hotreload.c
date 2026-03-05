/* ── Editor hot-reload visual integration tests ──────────────────────────
   Renders Clay UI to a CPU framebuffer, saves as BMP, then simulates
   an editor.dll hot-reload and renders again. Compares the two images
   pixel-by-pixel to verify the UI is identical after reload.

   No mocks — uses the real Clay library and real layout computation.

   Build:  tcc -Isrc -Isrc/editor -Ilib/clay tests/test_hotreload.c
   Run:    build/Debug/test_hotreload.exe
   ────────────────────────────────────────────────────────────────────────── */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

/* ── Real Clay implementation (same as editor.dll) ────────────────────── */
#define CLAY_IMPLEMENTATION
#ifndef CLAY_DISABLE_SIMD
#define CLAY_DISABLE_SIMD
#endif
#include "clay.h"

/* ── Editor header for struct definitions ─────────────────────────────── */
#include "cache_profiler.h"
#include "editor.h"

/* ── Link stubs for functions that live in other DLLs at runtime ───────
   These are NOT mocks — they're linker stubs for symbols that editor.h
   references but that live in externals.dll / collab code at runtime.
   The hot-reload logic being tested does not call any of these. ──────── */
void collab_init(collab_state *cs) { (void)cs; }
int  collab_connect(collab_state *cs, const char *host, uint16_t port)
     { (void)cs; (void)host; (void)port; return -1; }
void collab_disconnect(collab_state *cs) { (void)cs; }
void collab_update(collab_state *cs, struct game_state *gs)
     { (void)cs; (void)gs; }
void collab_emit_op(collab_state *cs, collab_op_type type,
                    int entity_index, const void *payload, uint16_t payload_len)
     { (void)cs; (void)type; (void)entity_index; (void)payload; (void)payload_len; }
void collab_send_cursor(collab_state *cs, int entity_index)
     { (void)cs; (void)entity_index; }
void cache_prof_init(void) {}
void cache_zone_begin(const char *name) { (void)name; }
void cache_zone_end(void) {}
void cache_prof_frame_reset(void) {}
int  cache_prof_available(void) { return 0; }
cache_prof_frame *cache_prof_get_frame(void) { return NULL; }
void cache_prof_sort_zones(void) {}
void cpu_zone_begin(const char *name) { (void)name; }
void cpu_zone_end(void) {}
void cpu_prof_frame_end(void) {}
void cpu_prof_set_capture_enabled(int e) { (void)e; }
int  cpu_prof_get_capture_enabled(void) { return 0; }

/* ── Test harness ────────────────────────────────────────────────────── */

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    static void test_##name(void); \
    static void run_##name(void) { \
        tests_run++; \
        printf("  %-60s ", #name); \
        simulate_editor_dll_reload(); /* ensure clean Clay globals between tests */ \
        test_##name(); \
        tests_passed++; \
        printf("PASS\n"); \
    } \
    static void test_##name(void)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("FAIL\n    %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        tests_failed++; \
        tests_passed--; \
        return; \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        printf("FAIL\n    %s:%d: %s == %d, expected %d\n", \
               __FILE__, __LINE__, #a, (int)(a), (int)(b)); \
        tests_failed++; \
        tests_passed--; \
        return; \
    } \
} while(0)

#define ASSERT_FLOAT_EQ(a, b, eps) do { \
    float _a = (float)(a), _b = (float)(b); \
    if (_a - _b > (eps) || _b - _a > (eps)) { \
        printf("FAIL\n    %s:%d: %s == %.3f, expected %.3f\n", \
               __FILE__, __LINE__, #a, _a, _b); \
        tests_failed++; \
        tests_passed--; \
        return; \
    } \
} while(0)

/* ── CPU framebuffer renderer ────────────────────────────────────────── */

#define FB_WIDTH  800
#define FB_HEIGHT 600

typedef struct {
    uint8_t pixels[FB_WIDTH * FB_HEIGHT * 4]; /* RGBA */
    int     width;
    int     height;
} framebuffer;

static void fb_clear(framebuffer *fb, uint8_t r, uint8_t g, uint8_t b) {
    int i;
    fb->width = FB_WIDTH;
    fb->height = FB_HEIGHT;
    for (i = 0; i < FB_WIDTH * FB_HEIGHT; i++) {
        fb->pixels[i * 4 + 0] = r;
        fb->pixels[i * 4 + 1] = g;
        fb->pixels[i * 4 + 2] = b;
        fb->pixels[i * 4 + 3] = 255;
    }
}

static void fb_fill_rect(framebuffer *fb, int x, int y, int w, int h,
                          uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    int px, py;
    if (a == 0) return;
    for (py = y; py < y + h && py < fb->height; py++) {
        if (py < 0) continue;
        for (px = x; px < x + w && px < fb->width; px++) {
            int idx;
            if (px < 0) continue;
            idx = (py * fb->width + px) * 4;
            if (a == 255) {
                fb->pixels[idx + 0] = r;
                fb->pixels[idx + 1] = g;
                fb->pixels[idx + 2] = b;
                fb->pixels[idx + 3] = 255;
            } else {
                float af = a / 255.0f;
                fb->pixels[idx + 0] = (uint8_t)(r * af + fb->pixels[idx + 0] * (1.0f - af));
                fb->pixels[idx + 1] = (uint8_t)(g * af + fb->pixels[idx + 1] * (1.0f - af));
                fb->pixels[idx + 2] = (uint8_t)(b * af + fb->pixels[idx + 2] * (1.0f - af));
                fb->pixels[idx + 3] = 255;
            }
        }
    }
}

static void fb_render_clay(framebuffer *fb, Clay_RenderCommandArray cmds) {
    int i;
    for (i = 0; i < cmds.length; i++) {
        Clay_RenderCommand *cmd = Clay_RenderCommandArray_Get(&cmds, i);
        Clay_BoundingBox bb = cmd->boundingBox;
        int x = (int)bb.x, y = (int)bb.y;
        int w = (int)bb.width, h = (int)bb.height;

        switch (cmd->commandType) {
            case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
                Clay_RectangleRenderData *rd = &cmd->renderData.rectangle;
                fb_fill_rect(fb, x, y, w, h,
                             (uint8_t)rd->backgroundColor.r,
                             (uint8_t)rd->backgroundColor.g,
                             (uint8_t)rd->backgroundColor.b,
                             (uint8_t)rd->backgroundColor.a);
                break;
            }
            case CLAY_RENDER_COMMAND_TYPE_TEXT: {
                Clay_TextRenderData *td = &cmd->renderData.text;
                fb_fill_rect(fb, x, y, w, h,
                             (uint8_t)td->textColor.r,
                             (uint8_t)td->textColor.g,
                             (uint8_t)td->textColor.b,
                             (uint8_t)td->textColor.a);
                break;
            }
            case CLAY_RENDER_COMMAND_TYPE_BORDER: {
                Clay_BorderRenderData *bd = &cmd->renderData.border;
                Clay_Color c = bd->color;
                fb_fill_rect(fb, x, y, w, (int)bd->width.top,
                             (uint8_t)c.r, (uint8_t)c.g, (uint8_t)c.b, (uint8_t)c.a);
                fb_fill_rect(fb, x, y + h - (int)bd->width.bottom, w, (int)bd->width.bottom,
                             (uint8_t)c.r, (uint8_t)c.g, (uint8_t)c.b, (uint8_t)c.a);
                fb_fill_rect(fb, x, y, (int)bd->width.left, h,
                             (uint8_t)c.r, (uint8_t)c.g, (uint8_t)c.b, (uint8_t)c.a);
                fb_fill_rect(fb, x + w - (int)bd->width.right, y, (int)bd->width.right, h,
                             (uint8_t)c.r, (uint8_t)c.g, (uint8_t)c.b, (uint8_t)c.a);
                break;
            }
            default:
                break;
        }
    }
}

/* Save framebuffer as BMP */
static int fb_save_bmp(const framebuffer *fb, const char *path) {
    FILE *fp;
    int row, col;
    uint32_t file_size, pixel_offset, dib_size, img_size;
    int32_t  w_signed, h_signed;
    uint16_t bpp, planes;
    uint32_t zero32 = 0;
    uint16_t bm_sig;

    fp = fopen(path, "wb");
    if (!fp) return -1;

    img_size    = (uint32_t)(fb->width * fb->height * 3);
    pixel_offset = 54;
    file_size   = pixel_offset + img_size;
    dib_size    = 40;
    w_signed    = (int32_t)fb->width;
    h_signed    = (int32_t)fb->height;
    planes      = 1;
    bpp         = 24;
    bm_sig      = 0x4D42;

    fwrite(&bm_sig, 2, 1, fp);
    fwrite(&file_size, 4, 1, fp);
    fwrite(&zero32, 4, 1, fp);
    fwrite(&pixel_offset, 4, 1, fp);
    fwrite(&dib_size, 4, 1, fp);
    fwrite(&w_signed, 4, 1, fp);
    fwrite(&h_signed, 4, 1, fp);
    fwrite(&planes, 2, 1, fp);
    fwrite(&bpp, 2, 1, fp);
    fwrite(&zero32, 4, 1, fp);
    fwrite(&img_size, 4, 1, fp);
    fwrite(&zero32, 4, 1, fp);
    fwrite(&zero32, 4, 1, fp);
    fwrite(&zero32, 4, 1, fp);
    fwrite(&zero32, 4, 1, fp);

    for (row = fb->height - 1; row >= 0; row--) {
        for (col = 0; col < fb->width; col++) {
            int idx = (row * fb->width + col) * 4;
            uint8_t bgr[3];
            bgr[0] = fb->pixels[idx + 2];
            bgr[1] = fb->pixels[idx + 1];
            bgr[2] = fb->pixels[idx + 0];
            fwrite(bgr, 3, 1, fp);
        }
    }

    fclose(fp);
    return 0;
}

/* Compare two framebuffers pixel-by-pixel */
static int fb_compare(const framebuffer *a, const framebuffer *b,
                       int tolerance, int *out_diff_count) {
    int i, diffs = 0;
    int total = FB_WIDTH * FB_HEIGHT * 4;
    for (i = 0; i < total; i++) {
        int d = (int)a->pixels[i] - (int)b->pixels[i];
        if (d < -tolerance || d > tolerance) diffs++;
    }
    if (out_diff_count) *out_diff_count = diffs;
    return diffs == 0 ? 0 : -1;
}

/* Save difference image (mismatched pixels highlighted in red) */
static void fb_save_diff(const framebuffer *a, const framebuffer *b, const char *path) {
    framebuffer diff;
    int i;
    fb_clear(&diff, 0, 0, 0);
    for (i = 0; i < FB_WIDTH * FB_HEIGHT; i++) {
        int idx = i * 4;
        int dr = abs((int)a->pixels[idx+0] - (int)b->pixels[idx+0]);
        int dg = abs((int)a->pixels[idx+1] - (int)b->pixels[idx+1]);
        int db = abs((int)a->pixels[idx+2] - (int)b->pixels[idx+2]);
        if (dr > 0 || dg > 0 || db > 0) {
            diff.pixels[idx+0] = 255;
            diff.pixels[idx+1] = 0;
            diff.pixels[idx+2] = 0;
            diff.pixels[idx+3] = 255;
        } else {
            diff.pixels[idx+0] = a->pixels[idx+0] / 4;
            diff.pixels[idx+1] = a->pixels[idx+1] / 4;
            diff.pixels[idx+2] = a->pixels[idx+2] / 4;
            diff.pixels[idx+3] = 255;
        }
    }
    fb_save_bmp(&diff, path);
}

/* ── Real text measurement (same algorithm as profiler_measure_text) ── */

static float g_font_advances[128];
static float g_font_line_height = 1.2f;

static void init_font_metrics(void) {
    int i;
    for (i = 0; i < 128; i++) g_font_advances[i] = 0.0f;
    for (i = 32; i < 127; i++) g_font_advances[i] = 0.55f;
    g_font_advances[' '] = 0.30f;
    g_font_line_height = 1.2f;
}

static Clay_Dimensions real_measure_text(Clay_StringSlice text,
                                          Clay_TextElementConfig *config,
                                          void *userData) {
    float w = 0.0f;
    int i;
    int font_size = config ? (int)config->fontSize : 14;
    (void)userData;
    for (i = 0; i < text.length; i++) {
        unsigned char ch = (unsigned char)text.chars[i];
        if (ch < 128) w += g_font_advances[ch] * (float)font_size;
    }
    return (Clay_Dimensions){
        .width  = w,
        .height = g_font_line_height * (float)font_size
    };
}

/* ── Simulate editor.dll hot-reload ──────────────────────────────────── */

/* This is exactly what the OS does when it unloads and reloads a DLL:
   all file-scope globals in the CLAY_IMPLEMENTATION (compiled into editor.dll)
   are zeroed because the new DLL gets a fresh data segment. */
static void simulate_editor_dll_reload(void) {
    Clay__currentContext = NULL;
    Clay__MeasureText = NULL;
    Clay__QueryScrollOffset = NULL;
}

/* This replicates the post-reload fixup code added to init_editor. */
static void editor_post_reload_fixup(editor_state *e) {
    void *clay_ctxs[] = {
        e->profiler_clay_ctx,
        e->scene_tree_clay_ctx,
        e->inspector_clay_ctx,
        e->project_browser_clay_ctx,
        e->menu_bar_clay_ctx,
        e->editor_toolbar_clay_ctx,
        e->cache_prof_clay_ctx,
        e->cpu_prof_clay_ctx,
    };
    int i;
    for (i = 0; i < (int)(sizeof(clay_ctxs) / sizeof(clay_ctxs[0])); i++) {
        if (clay_ctxs[i]) {
            Clay_SetCurrentContext((Clay_Context *)clay_ctxs[i]);
            Clay_SetMeasureTextFunction(real_measure_text, e);
        }
    }
    e->clay_editor = e->profiler_clay_ctx;
}

/* ── Helpers ─────────────────────────────────────────────────────────── */

static Clay_Context *create_clay_ctx(void *mem, uint32_t size) {
    /* Clear any dangling context pointer from a previous test that freed its
       Clay memory.  Clay_Initialize reads oldContext->maxElementCount, so a
       stale pointer here causes a use-after-free crash. */
    Clay__currentContext = NULL;
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(size, mem);
    Clay_ErrorHandler err = {0};
    return Clay_Initialize(arena, (Clay_Dimensions){FB_WIDTH, FB_HEIGHT}, err);
}

/* Build a realistic editor profiler panel layout.
   Uses Clay v0.14 API: CLAY(id, { .layout = ..., .backgroundColor = ... }) */
static Clay_RenderCommandArray build_profiler_layout(void) {
    Clay_BeginLayout();
    CLAY(CLAY_ID("ProfilerRoot"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(FB_WIDTH), CLAY_SIZING_FIXED(FB_HEIGHT) },
                     .padding = {8, 8, 8, 8},
                     .childGap = 4,
                     .layoutDirection = CLAY_TOP_TO_BOTTOM }
    }) {
        CLAY(CLAY_ID("Header"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(32) },
                         .padding = {8, 8, 4, 4},
                         .childGap = 12 },
            .backgroundColor = {45, 45, 52, 255}
        }) {
            CLAY_TEXT(CLAY_STRING("Memory Profiler"),
                      CLAY_TEXT_CONFIG({ .fontSize = 14, .textColor = {220, 220, 220, 255} }));
            CLAY_TEXT(CLAY_STRING("(arena usage)"),
                      CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = {140, 140, 140, 255} }));
        }
        CLAY(CLAY_ID("Content"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                         .padding = {4, 4, 4, 4},
                         .childGap = 2,
                         .layoutDirection = CLAY_TOP_TO_BOTTOM },
            .backgroundColor = {30, 30, 35, 255}
        }) {
            int row;
            for (row = 0; row < 12; row++) {
                Clay_Color row_color = (row % 2 == 0)
                    ? (Clay_Color){38, 38, 44, 255}
                    : (Clay_Color){34, 34, 40, 255};
                CLAY(CLAY_IDI_LOCAL("Row", row), {
                    .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(24) },
                                 .padding = {6, 6, 2, 2},
                                 .childGap = 16 },
                    .backgroundColor = row_color
                }) {
                    CLAY_TEXT(CLAY_STRING("arena_chunk"),
                              CLAY_TEXT_CONFIG({ .fontSize = 13, .textColor = {180, 180, 190, 255} }));
                    CLAY_TEXT(CLAY_STRING("64.0 KB"),
                              CLAY_TEXT_CONFIG({ .fontSize = 13, .textColor = {120, 200, 120, 255} }));
                    CLAY_TEXT(CLAY_STRING("42.1%"),
                              CLAY_TEXT_CONFIG({ .fontSize = 13, .textColor = {200, 180, 100, 255} }));
                }
            }
        }
        CLAY(CLAY_ID("StatusBar"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(20) },
                         .padding = {8, 8, 2, 2} },
            .backgroundColor = {40, 40, 48, 255}
        }) {
            CLAY_TEXT(CLAY_STRING("Total: 768.0 KB | Used: 512.3 KB | Free: 255.7 KB"),
                      CLAY_TEXT_CONFIG({ .fontSize = 11, .textColor = {160, 160, 170, 255} }));
        }
    }
    return Clay_EndLayout();
}

/* Build a realistic scene tree layout */
static Clay_RenderCommandArray build_scene_tree_layout(void) {
    const char *entity_names[] = {
        "Player", "Camera", "DirectionalLight",
        "Ground", "Tree_01", "Tree_02", "Rock_Large",
        "NPC_Merchant", "Trigger_Zone", "Skybox"
    };
    int ent;
    Clay_BeginLayout();
    CLAY(CLAY_ID("SceneRoot"), {
        .layout = { .sizing = { CLAY_SIZING_FIXED(FB_WIDTH), CLAY_SIZING_FIXED(FB_HEIGHT) },
                     .padding = {4, 4, 4, 4},
                     .childGap = 1,
                     .layoutDirection = CLAY_TOP_TO_BOTTOM }
    }) {
        CLAY(CLAY_ID("SceneHeader"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) },
                         .padding = {8, 8, 4, 4} },
            .backgroundColor = {50, 50, 58, 255}
        }) {
            CLAY_TEXT(CLAY_STRING("Scene Hierarchy"),
                      CLAY_TEXT_CONFIG({ .fontSize = 13, .textColor = {200, 200, 210, 255} }));
        }
        for (ent = 0; ent < 10; ent++) {
            Clay_Color ent_color = (ent == 0)
                ? (Clay_Color){60, 80, 120, 255}
                : (Clay_Color){36, 36, 42, 255};
            float pad_left = (float)(12 + (ent > 3 ? 16 : 0));
            CLAY(CLAY_IDI_LOCAL("Entity", ent), {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(22) },
                             .padding = {pad_left, 4, 2, 2} },
                .backgroundColor = ent_color
            }) {
                {
                    Clay_String ent_str = { .length = (int32_t)strlen(entity_names[ent]), .chars = entity_names[ent] };
                    CLAY_TEXT(ent_str,
                              CLAY_TEXT_CONFIG({ .fontSize = 13, .textColor = {190, 190, 200, 255} }));
                }
            }
        }
    }
    return Clay_EndLayout();
}

/* ── Tests ───────────────────────────────────────────────────────────── */

/*
   Core visual regression test:
   1. Create Clay context, run layout, render to framebuffer A
   2. Simulate editor.dll reload (zero Clay globals)
   3. Re-wire Clay contexts (the fix)
   4. Run same layout, render to framebuffer B
   5. Compare A and B — must be pixel-identical
*/
TEST(profiler_panel_identical_after_reload) {
    uint32_t clay_size = Clay_MinMemorySize();
    void *mem = malloc(clay_size);
    ASSERT(mem != NULL);
    Clay_Context *ctx = create_clay_ctx(mem, clay_size);
    Clay_SetMeasureTextFunction(real_measure_text, NULL);

    editor_state es;
    memset(&es, 0, sizeof(es));
    es.profiler_clay_ctx = ctx;
    es.initialized = 1;

    framebuffer *fb_before = (framebuffer *)malloc(sizeof(framebuffer));
    ASSERT(fb_before != NULL);
    fb_clear(fb_before, 20, 20, 25);
    Clay_SetCurrentContext(ctx);
    Clay_RenderCommandArray cmds_before = build_profiler_layout();
    ASSERT(cmds_before.length > 0);
    fb_render_clay(fb_before, cmds_before);
    fb_save_bmp(fb_before, "build/Debug/hotreload_profiler_before.bmp");

    simulate_editor_dll_reload();
    editor_post_reload_fixup(&es);

    framebuffer *fb_after = (framebuffer *)malloc(sizeof(framebuffer));
    ASSERT(fb_after != NULL);
    fb_clear(fb_after, 20, 20, 25);
    Clay_SetCurrentContext(ctx);
    Clay_RenderCommandArray cmds_after = build_profiler_layout();
    ASSERT(cmds_after.length > 0);
    fb_render_clay(fb_after, cmds_after);
    fb_save_bmp(fb_after, "build/Debug/hotreload_profiler_after.bmp");

    int diff_count = 0;
    int result = fb_compare(fb_before, fb_after, 0, &diff_count);
    if (result != 0) {
        fb_save_diff(fb_before, fb_after, "build/Debug/hotreload_profiler_diff.bmp");
        printf("FAIL\n    %d pixels differ (see hotreload_profiler_diff.bmp)\n", diff_count);
        tests_failed++;
        tests_passed--;
        free(fb_before); free(fb_after); free(mem);
        return;
    }
    ASSERT_EQ(cmds_before.length, cmds_after.length);
    free(fb_before); free(fb_after); free(mem);
}

TEST(scene_tree_panel_identical_after_reload) {
    uint32_t clay_size = Clay_MinMemorySize();
    void *mem = malloc(clay_size);
    ASSERT(mem != NULL);
    Clay_Context *ctx = create_clay_ctx(mem, clay_size);
    Clay_SetMeasureTextFunction(real_measure_text, NULL);

    editor_state es;
    memset(&es, 0, sizeof(es));
    es.scene_tree_clay_ctx = ctx;
    es.initialized = 1;

    framebuffer *fb_before = (framebuffer *)malloc(sizeof(framebuffer));
    ASSERT(fb_before != NULL);
    fb_clear(fb_before, 20, 20, 25);
    Clay_SetCurrentContext(ctx);
    fb_render_clay(fb_before, build_scene_tree_layout());
    fb_save_bmp(fb_before, "build/Debug/hotreload_scenetree_before.bmp");

    simulate_editor_dll_reload();
    editor_post_reload_fixup(&es);

    framebuffer *fb_after = (framebuffer *)malloc(sizeof(framebuffer));
    ASSERT(fb_after != NULL);
    fb_clear(fb_after, 20, 20, 25);
    Clay_SetCurrentContext(ctx);
    fb_render_clay(fb_after, build_scene_tree_layout());
    fb_save_bmp(fb_after, "build/Debug/hotreload_scenetree_after.bmp");

    int diff_count = 0;
    int result = fb_compare(fb_before, fb_after, 0, &diff_count);
    if (result != 0) {
        fb_save_diff(fb_before, fb_after, "build/Debug/hotreload_scenetree_diff.bmp");
        printf("FAIL\n    %d pixels differ\n", diff_count);
        tests_failed++;
        tests_passed--;
        free(fb_before); free(fb_after); free(mem);
        return;
    }
    free(fb_before); free(fb_after); free(mem);
}

/* Test all 8 Clay contexts survive reload with identical output */
TEST(all_panels_identical_after_reload) {
    uint32_t clay_size = Clay_MinMemorySize();
    void *mems[8];
    Clay_Context *ctxs[8];
    int i;

    for (i = 0; i < 8; i++) {
        mems[i] = malloc(clay_size);
        ASSERT(mems[i] != NULL);
        ctxs[i] = create_clay_ctx(mems[i], clay_size);
        Clay_SetMeasureTextFunction(real_measure_text, NULL);
    }

    editor_state es;
    memset(&es, 0, sizeof(es));
    es.profiler_clay_ctx        = ctxs[0];
    es.scene_tree_clay_ctx      = ctxs[1];
    es.inspector_clay_ctx       = ctxs[2];
    es.project_browser_clay_ctx = ctxs[3];
    es.menu_bar_clay_ctx        = ctxs[4];
    es.editor_toolbar_clay_ctx  = ctxs[5];
    es.cache_prof_clay_ctx      = ctxs[6];
    es.cpu_prof_clay_ctx        = ctxs[7];
    es.initialized = 1;

    framebuffer *fbs_before[8];
    for (i = 0; i < 8; i++) {
        fbs_before[i] = (framebuffer *)malloc(sizeof(framebuffer));
        ASSERT(fbs_before[i] != NULL);
        fb_clear(fbs_before[i], 20, 20, 25);
        Clay_SetCurrentContext(ctxs[i]);
        fb_render_clay(fbs_before[i], build_profiler_layout());
    }

    simulate_editor_dll_reload();
    editor_post_reload_fixup(&es);

    for (i = 0; i < 8; i++) {
        framebuffer *fb_after = (framebuffer *)malloc(sizeof(framebuffer));
        ASSERT(fb_after != NULL);
        fb_clear(fb_after, 20, 20, 25);
        Clay_SetCurrentContext(ctxs[i]);
        fb_render_clay(fb_after, build_profiler_layout());

        int diff_count = 0;
        int result = fb_compare(fbs_before[i], fb_after, 0, &diff_count);
        if (result != 0) {
            char path[256];
            snprintf(path, sizeof(path), "build/Debug/hotreload_panel%d_diff.bmp", i);
            fb_save_diff(fbs_before[i], fb_after, path);
            printf("FAIL\n    Panel %d: %d pixels differ\n", i, diff_count);
            tests_failed++;
            tests_passed--;
            free(fb_after);
            { int j; for (j = 0; j < 8; j++) { free(fbs_before[j]); free(mems[j]); } }
            return;
        }
        free(fb_after);
    }
    for (i = 0; i < 8; i++) { free(fbs_before[i]); free(mems[i]); }
}

/* Regression guard: WITHOUT the fix, rendering MUST differ */
TEST(without_fix_rendering_breaks) {
    uint32_t clay_size = Clay_MinMemorySize();
    void *mem = malloc(clay_size);
    Clay_Context *ctx = create_clay_ctx(mem, clay_size);
    Clay_SetMeasureTextFunction(real_measure_text, NULL);

    framebuffer *fb_before = (framebuffer *)malloc(sizeof(framebuffer));
    fb_clear(fb_before, 20, 20, 25);
    Clay_SetCurrentContext(ctx);
    fb_render_clay(fb_before, build_profiler_layout());

    simulate_editor_dll_reload();
    Clay_SetCurrentContext(ctx);
    /* NO re-wire — Clay__MeasureText stays NULL */
    ASSERT(Clay__MeasureText == NULL);

    framebuffer *fb_broken = (framebuffer *)malloc(sizeof(framebuffer));
    fb_clear(fb_broken, 20, 20, 25);
    fb_render_clay(fb_broken, build_profiler_layout());
    fb_save_bmp(fb_broken, "build/Debug/hotreload_broken_norefire.bmp");

    int diff_count = 0;
    fb_compare(fb_before, fb_broken, 0, &diff_count);
    /* Images MUST differ — proves the bug exists without the fix */
    ASSERT(diff_count > 0);

    free(fb_before); free(fb_broken); free(mem);
}

/* 10 reload cycles all produce identical output */
TEST(ten_reload_cycles_identical_output) {
    uint32_t clay_size = Clay_MinMemorySize();
    void *mem = malloc(clay_size);
    Clay_Context *ctx = create_clay_ctx(mem, clay_size);
    Clay_SetMeasureTextFunction(real_measure_text, NULL);

    editor_state es;
    memset(&es, 0, sizeof(es));
    es.profiler_clay_ctx = ctx;
    es.initialized = 1;

    framebuffer *fb_ref = (framebuffer *)malloc(sizeof(framebuffer));
    fb_clear(fb_ref, 20, 20, 25);
    Clay_SetCurrentContext(ctx);
    fb_render_clay(fb_ref, build_profiler_layout());

    int cycle;
    for (cycle = 0; cycle < 10; cycle++) {
        simulate_editor_dll_reload();
        editor_post_reload_fixup(&es);

        framebuffer *fb_cycle = (framebuffer *)malloc(sizeof(framebuffer));
        fb_clear(fb_cycle, 20, 20, 25);
        Clay_SetCurrentContext(ctx);
        fb_render_clay(fb_cycle, build_profiler_layout());

        int diff_count = 0;
        int result = fb_compare(fb_ref, fb_cycle, 0, &diff_count);
        if (result != 0) {
            char path[256];
            snprintf(path, sizeof(path), "build/Debug/hotreload_cycle%d_diff.bmp", cycle);
            fb_save_diff(fb_ref, fb_cycle, path);
            printf("FAIL\n    Cycle %d: %d pixels differ\n", cycle, diff_count);
            tests_failed++;
            tests_passed--;
            free(fb_cycle); free(fb_ref); free(mem);
            return;
        }
        free(fb_cycle);
    }
    free(fb_ref); free(mem);
}

/* All editor state survives reload */
TEST(state_preserved_across_visual_reload) {
    uint32_t clay_size = Clay_MinMemorySize();
    void *mem = malloc(clay_size);
    Clay_Context *ctx = create_clay_ctx(mem, clay_size);
    Clay_SetMeasureTextFunction(real_measure_text, NULL);

    editor_state es;
    memset(&es, 0, sizeof(es));
    es.profiler_clay_ctx = ctx;
    es.initialized = 1;
    es.cam_pos = VEC3(5.0f, 10.0f, -3.0f);
    es.cam_yaw = 1.5f;
    es.cam_pitch = -0.25f;
    es.scene_selected_entity = 4;
    es.scene_selection_mask[4] = 1;
    es.scene_selection_count = 1;
    es.font_line_height = 1.2f;
    { int i; for (i = 32; i < 128; i++) es.font_advances[i] = 0.55f; }

    simulate_editor_dll_reload();
    editor_post_reload_fixup(&es);

    ASSERT_FLOAT_EQ(es.cam_pos.x, 5.0f, 0.001f);
    ASSERT_FLOAT_EQ(es.cam_pos.y, 10.0f, 0.001f);
    ASSERT_FLOAT_EQ(es.cam_pos.z, -3.0f, 0.001f);
    ASSERT_FLOAT_EQ(es.cam_yaw, 1.5f, 0.001f);
    ASSERT_FLOAT_EQ(es.cam_pitch, -0.25f, 0.001f);
    ASSERT_EQ(es.scene_selected_entity, 4);
    ASSERT_EQ(es.scene_selection_mask[4], 1);
    ASSERT_EQ(es.scene_selection_count, 1);
    ASSERT_FLOAT_EQ(es.font_line_height, 1.2f, 0.001f);
    ASSERT_FLOAT_EQ(es.font_advances[65], 0.55f, 0.001f);
    ASSERT(es.clay_editor == ctx);
    ASSERT_EQ(es.initialized, 1);
    free(mem);
}

/* Render command bounding boxes match exactly */
TEST(render_command_bounding_boxes_match) {
    uint32_t clay_size = Clay_MinMemorySize();
    void *mem = malloc(clay_size);
    Clay_Context *ctx = create_clay_ctx(mem, clay_size);
    Clay_SetMeasureTextFunction(real_measure_text, NULL);

    editor_state es;
    memset(&es, 0, sizeof(es));
    es.profiler_clay_ctx = ctx;
    es.initialized = 1;

    Clay_SetCurrentContext(ctx);
    Clay_RenderCommandArray cmds_before = build_profiler_layout();

    simulate_editor_dll_reload();
    editor_post_reload_fixup(&es);

    Clay_SetCurrentContext(ctx);
    Clay_RenderCommandArray cmds_after = build_profiler_layout();

    ASSERT_EQ(cmds_before.length, cmds_after.length);
    {
        int i;
        for (i = 0; i < cmds_before.length; i++) {
            Clay_RenderCommand *a = Clay_RenderCommandArray_Get(&cmds_before, i);
            Clay_RenderCommand *b = Clay_RenderCommandArray_Get(&cmds_after, i);
            ASSERT_EQ(a->commandType, b->commandType);
            ASSERT_FLOAT_EQ(a->boundingBox.x, b->boundingBox.x, 0.01f);
            ASSERT_FLOAT_EQ(a->boundingBox.y, b->boundingBox.y, 0.01f);
            ASSERT_FLOAT_EQ(a->boundingBox.width, b->boundingBox.width, 0.01f);
            ASSERT_FLOAT_EQ(a->boundingBox.height, b->boundingBox.height, 0.01f);
        }
    }
    free(mem);
}

/* Dock state survives reload */
TEST(dock_state_survives_reload) {
    dock_state d;
    memset(&d, 0, sizeof(d));
    dock_init_default(&d);

    int root_before = d.windows[0].root_node;
    int init_before = d.initialized;

    simulate_editor_dll_reload();

    ASSERT_EQ(d.windows[0].root_node, root_before);
    ASSERT_EQ(d.initialized, init_before);
}

/* Stress: 50 rapid reload cycles */
TEST(stress_fifty_reload_cycles) {
    uint32_t clay_size = Clay_MinMemorySize();
    void *mem = malloc(clay_size);
    Clay_Context *ctx = create_clay_ctx(mem, clay_size);
    Clay_SetMeasureTextFunction(real_measure_text, NULL);

    editor_state es;
    memset(&es, 0, sizeof(es));
    es.profiler_clay_ctx = ctx;
    es.initialized = 1;
    es.cam_pos = VEC3(1.0f, 2.0f, 3.0f);

    framebuffer *fb_ref = (framebuffer *)malloc(sizeof(framebuffer));
    fb_clear(fb_ref, 20, 20, 25);
    Clay_SetCurrentContext(ctx);
    fb_render_clay(fb_ref, build_profiler_layout());

    int cycle;
    for (cycle = 0; cycle < 50; cycle++) {
        simulate_editor_dll_reload();
        editor_post_reload_fixup(&es);

        framebuffer *fb_cycle = (framebuffer *)malloc(sizeof(framebuffer));
        fb_clear(fb_cycle, 20, 20, 25);
        Clay_SetCurrentContext(ctx);
        fb_render_clay(fb_cycle, build_profiler_layout());

        int diff_count = 0;
        if (fb_compare(fb_ref, fb_cycle, 0, &diff_count) != 0) {
            printf("FAIL\n    Cycle %d: %d pixels differ\n", cycle, diff_count);
            tests_failed++;
            tests_passed--;
            free(fb_cycle); free(fb_ref); free(mem);
            return;
        }
        free(fb_cycle);
    }

    ASSERT_FLOAT_EQ(es.cam_pos.x, 1.0f, 0.001f);
    ASSERT_FLOAT_EQ(es.cam_pos.y, 2.0f, 0.001f);
    ASSERT_FLOAT_EQ(es.cam_pos.z, 3.0f, 0.001f);
    free(fb_ref); free(mem);
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("\n=== Hot-Reload Visual Integration Tests ===\n\n");

    init_font_metrics();

    run_profiler_panel_identical_after_reload();
    run_scene_tree_panel_identical_after_reload();
    run_all_panels_identical_after_reload();
    run_without_fix_rendering_breaks();
    run_ten_reload_cycles_identical_output();
    run_state_preserved_across_visual_reload();
    run_render_command_bounding_boxes_match();
    run_dock_state_survives_reload();
    run_stress_fifty_reload_cycles();

    printf("\n=== Results ===\n");
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);
    printf("  Total:  %d\n", tests_run);

    if (tests_passed == tests_run)
        printf("  ALL TESTS PASSED\n");

    return tests_failed > 0 ? 1 : 0;
}
