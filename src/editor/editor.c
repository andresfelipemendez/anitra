#include <game.h>
#include <export.h>
#include <math3d.h>
#include "dock.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdbool.h>

#define CLAY_IMPLEMENTATION
#include "clay.h"

/* ── Profiler helpers (moved from externals.c — pure CPU, no GPU deps) ──── */
/* ── Profiler helpers (moved from externals.c — pure CPU, no GPU deps) ──── */

/* Memory profiler colors (one per allocation slot) */
static const Clay_Color profiler_colors[] = {
    {100, 160, 220, 255},  /* blue   */
    {220, 140,  80, 255},  /* orange */
    {100, 200, 120, 255},  /* green  */
    {200, 100, 180, 255},  /* pink   */
    {180, 180, 100, 255},  /* yellow */
    {130, 120, 220, 255},  /* purple */
    {100, 200, 200, 255},  /* teal   */
    {220, 110, 110, 255},  /* red    */
};
static const int profiler_color_count = (int)(sizeof(profiler_colors) / sizeof(profiler_colors[0]));

/* Format bytes as human-readable string (rotating static buffers).
   Static buffers reset to zero on hot-reload — fine, overwritten before read. */
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

/* Hovered record info — set by profiler_tree_arena, used by grid/block for highlighting */
typedef struct HoverInfo {
    const char *tag;
    uint32_t offset;
    uint32_t size;
    int found;
} HoverInfo;

/* Collapsed state for tree nodes (persists across frames within a DLL load) */
#define MAX_TREE_NODES 256
static int tree_collapsed[MAX_TREE_NODES]; /* 0 = expanded, 1 = collapsed */

/* Collect all leaf records (flattened) for block + grid views */
typedef struct FlatRecord {
    const char *tag;
    uint32_t    offset;
    uint32_t    size;
    int         color_idx;
} FlatRecord;
#define MAX_FLAT_RECORDS 256

/* Flatten arena into leaf records for block/grid views.
   If out is NULL, only advances color_id (used by tree collapse skip). */
static int profiler_flatten_arena(arena *a, uint32_t base_offset,
                                  FlatRecord *out, int count, int *color_id) {
    uint32_t i;
    for (i = 0; i < a->record_count && count < MAX_FLAT_RECORDS; i++) {
        arena_record *r = &a->records[i];
        if (r->child && r->child->record_count > 0) {
            count = profiler_flatten_arena(r->child, base_offset + r->offset + (uint32_t)sizeof(arena),
                                           out, count, color_id);
        } else if (r->child) {
            if (out) {
                out[count].tag = r->tag;
                out[count].offset = base_offset + r->offset;
                out[count].size = r->child->used > 0 ? r->child->used : r->size;
                out[count].color_idx = (*color_id);
            }
            (*color_id)++;
            count++;
        } else {
            if (out) {
                out[count].tag = r->tag;
                out[count].offset = base_offset + r->offset;
                out[count].size = r->size;
                out[count].color_idx = (*color_id);
            }
            (*color_id)++;
            count++;
        }
    }
    return count;
}

/* Count total records recursively (for stable row_id skipping on collapse) */
static int count_arena_records(arena *a) {
    uint32_t i;
    int n = (int)a->record_count;
    for (i = 0; i < a->record_count; i++)
        if (a->records[i].child) n += count_arena_records(a->records[i].child);
    return n;
}

/* Recursive: emit Clay tree rows for an arena and its children.
   color_id tracks flat-record-consistent color assignment:
     - leaf records and leaf sub-arenas: assign color and increment
     - parent sub-arenas (with children): neutral color, don't increment
   This keeps colors in sync with profiler_flatten_arena(). */
static void profiler_tree_arena(arena *a, int depth, int *row_id,
                                uint32_t base_offset, HoverInfo *hover,
                                int *color_id, int click) {
    uint32_t i;
    for (i = 0; i < a->record_count; i++) {
        arena_record *r = &a->records[i];
        int rid = *row_id;
        int is_parent = r->child && r->child->record_count > 0;
        int hovered;
        Clay_Color swatch;

        /* Color assignment matches profiler_flatten_arena logic:
           parent sub-arenas use neutral gray, leaves get sequential colors */
        if (is_parent) {
            swatch = (Clay_Color){120, 120, 130, 255};
        } else {
            swatch = profiler_colors[(*color_id) % profiler_color_count];
            (*color_id)++;
        }

        /* Build size string */
        {
            static char size_bufs[256][32];
            static char label_bufs[256][48];
            int bidx = rid % 256;
            if (r->child) {
                float child_pct = (r->child->capacity > 0)
                    ? (100.0f * r->child->used / r->child->capacity) : 0.0f;
                snprintf(size_bufs[bidx], sizeof(size_bufs[bidx]), "%s (%.0f%%)",
                    format_bytes(r->child->capacity), (double)child_pct);
            } else {
                snprintf(size_bufs[bidx], sizeof(size_bufs[bidx]), "%s", format_bytes(r->size));
            }
            /* Label: collapse arrow for parents, bullet for leaves */
            if (is_parent) {
                snprintf(label_bufs[bidx], sizeof(label_bufs[bidx]), "%s %s",
                    (rid < MAX_TREE_NODES && tree_collapsed[rid]) ? ">" : "v", r->tag);
            } else {
                snprintf(label_bufs[bidx], sizeof(label_bufs[bidx]), "%s", r->tag);
            }

            /* Two-column row: [left: indent+swatch+tag GROW] [right: size FIT] */
            CLAY(CLAY_IDI("TreeRow", (int32_t)rid), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIT({0}) },
                    .padding = { .left = (uint16_t)(4 + depth * 12), .right = 8, .top = 2, .bottom = 2 },
                    .childGap = 4,
                    .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER},
                    .layoutDirection = CLAY_LEFT_TO_RIGHT
                },
                .backgroundColor = Clay_Hovered() ? ((Clay_Color){60, 60, 70, 255}) : ((Clay_Color){0, 0, 0, 0})
            }) {
                hovered = Clay_Hovered();

                /* Toggle collapse on click */
                if (hovered && click && is_parent && rid < MAX_TREE_NODES)
                    tree_collapsed[rid] = !tree_collapsed[rid];

                /* Store hover info for grid/block highlight */
                if (hovered && hover) {
                    hover->tag = r->tag;
                    hover->offset = base_offset + r->offset;
                    hover->size = r->child ? r->child->capacity + (uint32_t)sizeof(arena) : r->size;
                    hover->found = 1;
                }

                /* Color swatch */
                CLAY(CLAY_IDI("TSwatch", (int32_t)rid), {
                    .layout = { .sizing = { CLAY_SIZING_FIXED(8), CLAY_SIZING_FIXED(8) } },
                    .backgroundColor = swatch,
                    .cornerRadius = CLAY_CORNER_RADIUS(2)
                }) {}

                /* Tag name — grows to push size to the right */
                CLAY(CLAY_IDI("TLabel", (int32_t)rid), {
                    .layout = { .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIT({0}) } }
                }) {
                    Clay_String tag_s = {false, (int32_t)strlen(label_bufs[bidx]), label_bufs[bidx]};
                    CLAY_TEXT(tag_s, CLAY_TEXT_CONFIG({
                        .textColor = hovered ? ((Clay_Color){255, 255, 255, 255})
                                             : ((Clay_Color){200, 200, 200, 255}),
                        .fontSize = 16
                    }));
                }

                /* Size — right-aligned */
                {
                    Clay_String sz_s = {false, (int32_t)strlen(size_bufs[bidx]), size_bufs[bidx]};
                    CLAY_TEXT(sz_s, CLAY_TEXT_CONFIG({.textColor = {140, 140, 150, 255}, .fontSize = 16}));
                }
            }
        }

        (*row_id)++;

        if (r->child && !(rid < MAX_TREE_NODES && tree_collapsed[rid])) {
            profiler_tree_arena(r->child, depth + 1, row_id,
                                base_offset + r->offset + (uint32_t)sizeof(arena),
                                hover, color_id, click);
        } else if (is_parent && rid < MAX_TREE_NODES && tree_collapsed[rid]) {
            /* Skip children — advance both row_id and color_id to keep
               them stable regardless of expand/collapse state */
            *row_id += count_arena_records(r->child);
            {
                int skip_color = *color_id;
                profiler_flatten_arena(r->child, 0, NULL, 0, &skip_color);
                *color_id = skip_color;
            }
        }
    }
}

/* Simple text measurement for Clay — uses pre-computed font advance table.
   No HarfBuzz, no kerning. Good enough for profiler UI (ASCII, single font). */
static Clay_Dimensions profiler_measure_text(Clay_StringSlice text,
                                              Clay_TextElementConfig *config,
                                              void *userData) {
    editor_state *e = (editor_state *)userData;
    float scale = (float)config->fontSize;
    float width = 0;
    int i;
    for (i = 0; i < text.length; i++) {
        int ch = (unsigned char)text.chars[i];
        if (ch < 128) width += e->font_advances[ch] * scale;
    }
    return (Clay_Dimensions){
        ceilf(width + 1.0f),
        ceilf(e->font_line_height * scale)
    };
}

/* ── Internal helpers ─────────────────────────────────────────── */

static Vec3 cam_forward(editor_state *e) {
    return VEC3(
        cosf(e->cam_pitch) * sinf(e->cam_yaw),
        sinf(e->cam_pitch),
        cosf(e->cam_pitch) * cosf(e->cam_yaw)
    );
}

static Vec3 world_to_screen(Vec3 pos, Mat4 vp, float w, float h) {
    float cx = vp.m[0]*pos.x + vp.m[4]*pos.y + vp.m[8]*pos.z  + vp.m[12];
    float cy = vp.m[1]*pos.x + vp.m[5]*pos.y + vp.m[9]*pos.z  + vp.m[13];
    float cw = vp.m[3]*pos.x + vp.m[7]*pos.y + vp.m[11]*pos.z + vp.m[15];
    if (cw < 0.001f) return VEC3(-9999, -9999, 0);
    {
        float ndx = cx / cw, ndy = cy / cw;
        return VEC3((ndx * 0.5f + 0.5f) * w, (1.0f - (ndy * 0.5f + 0.5f)) * h, 0);
    }
}

static float pt_seg_dist_sq(Vec3 p, Vec3 a, Vec3 b) {
    float dx = b.x - a.x, dy = b.y - a.y;
    float len_sq = dx*dx + dy*dy;
    float t, qx, qy;
    if (len_sq < 0.001f) { float ex = p.x-a.x, ey = p.y-a.y; return ex*ex+ey*ey; }
    t = ((p.x-a.x)*dx + (p.y-a.y)*dy) / len_sq;
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    qx = a.x + t*dx - p.x;
    qy = a.y + t*dy - p.y;
    return qx*qx + qy*qy;
}

static void add_line(editor_state *e, Vec3 a, Vec3 b, float r, float g, float bl) {
    int i;
    if (e->line_count >= EDITOR_MAX_LINES) return;
    i = e->line_count * 2;
    e->lines[i + 0].x = a.x; e->lines[i + 0].y = a.y; e->lines[i + 0].z = a.z;
    e->lines[i + 0].r = r;   e->lines[i + 0].g = g;   e->lines[i + 0].b = bl;
    e->lines[i + 1].x = b.x; e->lines[i + 1].y = b.y; e->lines[i + 1].z = b.z;
    e->lines[i + 1].r = r;   e->lines[i + 1].g = g;   e->lines[i + 1].b = bl;
    e->line_count++;
}

/* ── Line building ─────────────────────────────────────────────── */

static void build_lines(game_state *gs, editor_state *es) {
    editor_state *e = es;
    float gc = 0.3f;
    int i;
    Vec3 axis_dirs[3];
    float colors[3][3];

    e->line_count = 0;

    /* Ground grid: 21x21 on XZ plane at Y=0 */
    for (i = -10; i <= 10; i++) {
        float fi = (float)i;
        add_line(e, VEC3(fi, 0, -10), VEC3(fi, 0, 10), gc, gc, gc);
        add_line(e, VEC3(-10, 0, fi), VEC3(10, 0, fi), gc, gc, gc);
    }
    /* Axis highlights */
    add_line(e, VEC3(-10, 0, 0), VEC3(10, 0, 0), 0.8f, 0.2f, 0.2f);  /* X red */
    add_line(e, VEC3(0, 0, -10), VEC3(0, 0, 10), 0.2f, 0.2f, 0.8f);  /* Z blue */
    add_line(e, VEC3(0, 0, 0),   VEC3(0, 2, 0),  0.2f, 0.8f, 0.2f);  /* Y green */

    /* Game camera frustum gizmo */
    if (gs->mesh3d.visible) {
        Vec3 eye    = gs->mesh3d.camera_eye;
        Vec3 target = gs->mesh3d.camera_target;
        Vec3 cam_up = gs->mesh3d.camera_up;
        Vec3 fwd    = vec3_normalize(vec3_sub(target, eye));
        Vec3 right  = vec3_normalize(vec3_cross(fwd, cam_up));
        Vec3 up     = vec3_cross(right, fwd);

        float fov    = 60.0f * 3.14159265f / 180.0f;
        float aspect = (gs->width > 0 && gs->height > 0)
                       ? (float)gs->width / (float)gs->height : 4.0f / 3.0f;
        float near_d = 0.3f;
        float vis_d  = 3.0f;

        float nh = tanf(fov * 0.5f) * near_d;
        float nw = nh * aspect;
        float fh = tanf(fov * 0.5f) * vis_d;
        float fw = fh * aspect;

        Vec3 nc = vec3_add(eye, vec3_scale(fwd, near_d));
        Vec3 fc = vec3_add(eye, vec3_scale(fwd, vis_d));

        Vec3 n[4], f[4];
        float cr, cg2, cb;

        n[0] = vec3_add(vec3_add(nc, vec3_scale(right,  nw)), vec3_scale(up,  nh));
        n[1] = vec3_add(vec3_add(nc, vec3_scale(right, -nw)), vec3_scale(up,  nh));
        n[2] = vec3_add(vec3_add(nc, vec3_scale(right, -nw)), vec3_scale(up, -nh));
        n[3] = vec3_add(vec3_add(nc, vec3_scale(right,  nw)), vec3_scale(up, -nh));

        f[0] = vec3_add(vec3_add(fc, vec3_scale(right,  fw)), vec3_scale(up,  fh));
        f[1] = vec3_add(vec3_add(fc, vec3_scale(right, -fw)), vec3_scale(up,  fh));
        f[2] = vec3_add(vec3_add(fc, vec3_scale(right, -fw)), vec3_scale(up, -fh));
        f[3] = vec3_add(vec3_add(fc, vec3_scale(right,  fw)), vec3_scale(up, -fh));

        cr = 1.0f; cg2 = 1.0f; cb = 0.2f; /* yellow */
        for (i = 0; i < 4; i++) add_line(e, eye, f[i], cr, cg2, cb);
        for (i = 0; i < 4; i++) add_line(e, n[i], n[(i+1)%4], cr, cg2, cb);
        for (i = 0; i < 4; i++) add_line(e, f[i], f[(i+1)%4], cr, cg2, cb);
        add_line(e, eye, vec3_add(eye, vec3_scale(up, 0.3f)), 0.2f, 1.0f, 0.2f);

        /* Translate gizmo at game camera eye */
        {
            Vec3 giz = eye;
            float giz_dist = vec3_len(vec3_sub(giz, e->cam_pos));
            float giz_len  = giz_dist * 0.08f;
            if (giz_len < 0.1f) giz_len = 0.1f;

            axis_dirs[0] = VEC3(1,0,0); axis_dirs[1] = VEC3(0,1,0); axis_dirs[2] = VEC3(0,0,1);
            colors[0][0] = 1.0f; colors[0][1] = 0.2f; colors[0][2] = 0.2f;
            colors[1][0] = 0.2f; colors[1][1] = 1.0f; colors[1][2] = 0.2f;
            colors[2][0] = 0.2f; colors[2][1] = 0.2f; colors[2][2] = 1.0f;

            for (i = 0; i < 3; i++) {
                int ax = i + 1;
                int hl = (e->gizmo_active == ax || (e->gizmo_active == 0 && e->gizmo_hovered == ax));
                float cr2 = hl ? 1.0f : colors[i][0];
                float cg3 = hl ? 1.0f : colors[i][1];
                float cb2 = hl ? 0.2f : colors[i][2];
                Vec3 tip = vec3_add(giz, vec3_scale(axis_dirs[i], giz_len));
                float ah;
                int a1, a2;
                Vec3 perp1, perp2, back;

                add_line(e, giz, tip, cr2, cg3, cb2);

                /* Arrowhead */
                ah = giz_len * 0.15f;
                a1 = (i + 1) % 3; a2 = (i + 2) % 3;
                perp1 = axis_dirs[a1]; perp2 = axis_dirs[a2];
                back = vec3_sub(tip, vec3_scale(axis_dirs[i], ah));
                add_line(e, tip, vec3_add(back, vec3_scale(perp1,  ah * 0.5f)), cr2, cg3, cb2);
                add_line(e, tip, vec3_add(back, vec3_scale(perp1, -ah * 0.5f)), cr2, cg3, cb2);
                add_line(e, tip, vec3_add(back, vec3_scale(perp2,  ah * 0.5f)), cr2, cg3, cb2);
                add_line(e, tip, vec3_add(back, vec3_scale(perp2, -ah * 0.5f)), cr2, cg3, cb2);
            }
        }
    }
}

/* ── Camera input (polling-based) ──────────────────────────────── */

static void update_camera(game_state *gs, editor_state *es) {
    editor_state *e = es;
    const bool *keys;
    float dt, spd, dx, dy;
    Vec3 fwd, right, up;
    SDL_Window *focused;

    if (!e->open || !e->window) return;
    focused = SDL_GetKeyboardFocus();
    if (focused != (SDL_Window *)e->window) return;

    keys = SDL_GetKeyboardState(NULL);
    dt   = gs->dt;
    fwd  = cam_forward(e);
    right = vec3_normalize(vec3_cross(fwd, VEC3(0, 1, 0)));
    up    = VEC3(0, 1, 0);
    spd   = e->cam_speed * dt;

    if (keys[SDL_SCANCODE_W]) e->cam_pos = vec3_add(e->cam_pos, vec3_scale(fwd,   spd));
    if (keys[SDL_SCANCODE_S]) e->cam_pos = vec3_sub(e->cam_pos, vec3_scale(fwd,   spd));
    if (keys[SDL_SCANCODE_A]) e->cam_pos = vec3_sub(e->cam_pos, vec3_scale(right, spd));
    if (keys[SDL_SCANCODE_D]) e->cam_pos = vec3_add(e->cam_pos, vec3_scale(right, spd));
    if (keys[SDL_SCANCODE_E]) e->cam_pos = vec3_add(e->cam_pos, vec3_scale(up,    spd));
    if (keys[SDL_SCANCODE_Q]) e->cam_pos = vec3_sub(e->cam_pos, vec3_scale(up,    spd));

    if (e->cam_mouse_look) {
        SDL_GetRelativeMouseState(&dx, &dy);
        e->cam_yaw   -= dx * e->cam_sens;
        e->cam_pitch -= dy * e->cam_sens;
        if (e->cam_pitch >  1.55f) e->cam_pitch =  1.55f;
        if (e->cam_pitch < -1.55f) e->cam_pitch = -1.55f;
    }
}

/* ── Gizmo hover detection (polling-based) ─────────────────────── */

static void update_gizmo_hover(game_state *gs, editor_state *es) {
    editor_state *e = es;
    SDL_Window *mouse_win;
    int i;
    float fw2, fh2, ed_aspect, mx, my;
    Mat4 ed_proj, ed_view, vp;
    Vec3 ed_fwd, giz, mouse, center_s;
    float giz_dist, giz_len, best_dist;
    Vec3 axis_dirs[3];

    if (!e->open || !e->window || !gs->mesh3d.visible) {
        e->gizmo_hovered = 0;
        return;
    }
    if (e->gizmo_active != 0) return;

    mouse_win = SDL_GetMouseFocus();
    if (mouse_win != (SDL_Window *)e->window) {
        e->gizmo_hovered = 0;
        return;
    }

    /* Use panel dimensions instead of full window size */
    fw2 = e->panel_w; fh2 = e->panel_h;
    if (fw2 < 1 || fh2 < 1) return;

    ed_aspect = fw2 / fh2;
    ed_proj = mat4_perspective(60.0f * 3.14159265f / 180.0f, ed_aspect, 0.1f, 200.0f);
    ed_fwd  = cam_forward(e);
    ed_view = mat4_look_at(e->cam_pos, vec3_add(e->cam_pos, ed_fwd), VEC3(0, 1, 0));
    vp = mat4_mul(ed_proj, ed_view);

    /* Transform mouse from window-space to panel-local space */
    SDL_GetMouseState(&mx, &my);
    mx -= e->panel_x;
    my -= e->panel_y;
    mouse = VEC3(mx, my, 0);

    giz = gs->mesh3d.camera_eye;
    giz_dist = vec3_len(vec3_sub(giz, e->cam_pos));
    giz_len  = giz_dist * 0.08f;
    if (giz_len < 0.1f) giz_len = 0.1f;

    center_s = world_to_screen(giz, vp, fw2, fh2);
    axis_dirs[0] = VEC3(giz_len, 0, 0);
    axis_dirs[1] = VEC3(0, giz_len, 0);
    axis_dirs[2] = VEC3(0, 0, giz_len);

    e->gizmo_hovered = 0;
    best_dist = 12.0f * 12.0f;

    for (i = 0; i < 3; i++) {
        Vec3 tip_s = world_to_screen(vec3_add(giz, axis_dirs[i]), vp, fw2, fh2);
        float d = pt_seg_dist_sq(mouse, center_s, tip_s);
        if (d < best_dist) {
            best_dist = d;
            e->gizmo_hovered = i + 1;
        }
    }
}

/* ── Public API ────────────────────────────────────────────────── */

EXPORT void init_editor(game_state *gs, editor_state *es) {
    editor_state *e = es;
    if (e->initialized) return;

    e->cam_pos    = VEC3(0.0f, 3.0f, 8.0f);
    e->cam_yaw    = 3.14159265f;
    e->cam_pitch  = -0.3f;
    e->cam_speed  = 5.0f;
    e->cam_sens   = 0.003f;
    e->cam_mouse_look = 0;
    e->open = 1;
    e->gizmo_hovered = 0;
    e->gizmo_active  = 0;
    e->line_count = 0;
    e->initialized = 1;

    /* Create profiler Clay context (one-time, backed by editor_arena).
       profiler_clay_ctx in editor_state survives hot-reload. */
    if (!e->profiler_clay_ctx && es->editor_arena) {
        uint32_t clay_size = (uint32_t)Clay_MinMemorySize();
        arena *clay_sub = arena_alloc_subarena(es->editor_arena, clay_size, 16, "clay_editor");
        if (clay_sub) {
            Clay_Arena ca = Clay_CreateArenaWithCapacityAndMemory(clay_size, clay_sub->base);
            Clay_ErrorHandler err = {0};
            e->profiler_clay_ctx = Clay_Initialize(ca, (Clay_Dimensions){800, 600}, err);
            Clay_SetCurrentContext((Clay_Context *)e->profiler_clay_ctx);
            Clay_SetMeasureTextFunction(profiler_measure_text, e);
            es->clay_editor = e->profiler_clay_ctx;
        }
    }
}

/* ── Profiler Clay layout (produces render commands for externals GPU upload) ── */

static void profiler_layout(game_state *gs, editor_state *es) {
    editor_state *e = es;
    dock_state *d = (dock_state *)e->dock;
    Clay_Context *pctx = (Clay_Context *)e->profiler_clay_ctx;
    int prof_win_idx;
    int prof_node;
    int win_w, win_h;
    arena *a;
    float used_pct;
    static char title_buf[128];
    static FlatRecord flat[MAX_FLAT_RECORDS];
    int flat_color, flat_count;
    uint32_t hover_offset, hover_size; /* byte range of hovered record (for range match) */
    int have_hover;
    int click;    /* snapshot of prof_click — consumed after layout */
    Clay_RenderCommandArray commands;

    if (!pctx) return;

    /* Sync profiler Clay arena usage for display */
    {
        Clay_Context *pc = pctx;
        arena *clay_sub_arena = NULL;
        uint32_t ri;
        /* Find the clay_editor sub-arena by tag in editor_arena records */
        for (ri = 0; ri < es->editor_arena->record_count; ri++) {
            if (es->editor_arena->records[ri].child &&
                strcmp(es->editor_arena->records[ri].tag, "clay_editor") == 0) {
                clay_sub_arena = es->editor_arena->records[ri].child;
                break;
            }
        }
        if (clay_sub_arena)
            clay_sub_arena->used = (uint32_t)pc->internalArena.nextAllocation;
    }

    Clay_SetCurrentContext(pctx);

    /* Get profiler panel dimensions from dock node */
    prof_node = dock_find_leaf_for_panel_global(d, PANEL_PROFILER, &prof_win_idx);
    if (prof_node >= 0) {
        DockNode *pn = &d->nodes[prof_node];
        win_w = (int)pn->w;
        win_h = (int)(pn->h - DOCK_HEADER_HEIGHT);
        if (win_h < 1) win_h = 1;
    } else {
        win_w = 800;
        win_h = 600;
    }
    Clay_SetLayoutDimensions((Clay_Dimensions){(float)win_w, (float)win_h});

    /* Feed Clay pointer + scroll state so scroll containers and hover work */
    click = e->prof_click;   /* snapshot before layout — tree uses it for collapse toggle */
    {
        Clay_Vector2 mpos = {e->prof_mouse_x, e->prof_mouse_y};
        Clay_Vector2 sdelta = {0, e->prof_scroll_y};
        Clay_SetPointerState(mpos, (bool)e->prof_mouse_down);
        Clay_UpdateScrollContainers(true, sdelta, gs->dt);
        e->prof_scroll_y = 0; /* consumed */
    }

    Clay_BeginLayout();

    a = gs->root_arena;
    used_pct = (a->capacity > 0) ? (100.0f * a->used / a->capacity) : 0.0f;

    snprintf(title_buf, sizeof(title_buf), "Memory Profiler    %s / %s  (%.1f%%)",
        format_bytes(a->used), format_bytes(a->capacity), (double)used_pct);

    /* Flatten records for block+grid */
    flat_color = 0;
    flat_count = profiler_flatten_arena(a, 0, flat, 0, &flat_color);

    /* Root container */
    CLAY(CLAY_ID("PRoot"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_GROW({0}) },
            .padding = CLAY_PADDING_ALL(12),
            .childGap = 0,
            .layoutDirection = CLAY_TOP_TO_BOTTOM
        },
        .backgroundColor = {25, 25, 30, 255}
    }) {
        /* Title bar */
        CLAY(CLAY_ID("PTitleBar"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIT({0}) },
                .padding = { .left = 8, .right = 8, .top = 6, .bottom = 10 }
            }
        }) {
            Clay_String ts = {false, (int32_t)strlen(title_buf), title_buf};
            CLAY_TEXT(ts, CLAY_TEXT_CONFIG({.textColor = {220, 220, 220, 255}, .fontSize = 16}));
        }

        /* Three-panel row */
        CLAY(CLAY_ID("PPanels"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_GROW({0}) },
                .childGap = 8,
                .layoutDirection = CLAY_LEFT_TO_RIGHT
            }
        }) {
            /* ===== TREE PANEL (left, 35%) ===== */
            CLAY(CLAY_ID("PTree"), {
                .layout = {
                    .sizing = { CLAY_SIZING_PERCENT(0.35f), CLAY_SIZING_GROW({0}) },
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

                /* Scrollable tree content */
                CLAY(CLAY_ID("PTreeScroll"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_GROW({0}) },
                        .childGap = 2,
                        .layoutDirection = CLAY_TOP_TO_BOTTOM
                    },
                    .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() }
                }) {
                    /* Arena root row */
                    {
                        static char root_buf[64];
                        snprintf(root_buf, sizeof(root_buf), "Arena  %s", format_bytes(a->capacity));
                        {
                            Clay_String rs = {false, (int32_t)strlen(root_buf), root_buf};
                            CLAY(CLAY_ID("TRootRow"), {
                                .layout = {
                                    .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIT({0}) },
                                    .padding = { .left = 8, .right = 8, .top = 4, .bottom = 4 }
                                }
                            }) {
                                CLAY_TEXT(rs, CLAY_TEXT_CONFIG({.textColor = {220, 220, 220, 255}, .fontSize = 16}));
                            }
                        }
                    }

                    {
                        HoverInfo hi = {0};
                        int row_id = 0;
                        int tree_color = 0;
                        profiler_tree_arena(a, 1, &row_id, 0, &hi,
                                            &tree_color, click);
                        hover_offset = hi.offset;
                        hover_size = hi.size;
                        have_hover = hi.found;
                    }
                }
            }

            /* ===== BLOCK PANEL (center, 20%) ===== */
            CLAY(CLAY_ID("PBlock"), {
                .layout = {
                    .sizing = { CLAY_SIZING_PERCENT(0.20f), CLAY_SIZING_GROW({0}) },
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

                /* Column of proportional blocks */
                {
                    float block_total_h = (float)(win_h - 80);
                    int bi;
                    if (block_total_h < 100) block_total_h = 100;

                    for (bi = 0; bi < flat_count; bi++) {
                        float frac = (float)flat[bi].size / (float)a->capacity;
                        float bh = frac * block_total_h;
                        Clay_Color bcolor;
                        int blk_hovered;
                        static char block_bufs[MAX_FLAT_RECORDS][48];
                        if (bh < 2.0f) bh = 2.0f;
                        bcolor = profiler_colors[flat[bi].color_idx % profiler_color_count];
                        blk_hovered = have_hover &&
                            flat[bi].offset >= hover_offset &&
                            flat[bi].offset + flat[bi].size <= hover_offset + hover_size;
                        if (have_hover) {
                            if (blk_hovered) {
                                bcolor.r = (float)(bcolor.r + (255 - bcolor.r) * 0.5f);
                                bcolor.g = (float)(bcolor.g + (255 - bcolor.g) * 0.5f);
                                bcolor.b = (float)(bcolor.b + (255 - bcolor.b) * 0.5f);
                            } else {
                                bcolor.r = (float)(bcolor.r * 0.3f);
                                bcolor.g = (float)(bcolor.g * 0.3f);
                                bcolor.b = (float)(bcolor.b * 0.3f);
                            }
                        }
                        snprintf(block_bufs[bi], sizeof(block_bufs[bi]), "%s", flat[bi].tag);

                        CLAY(CLAY_IDI("BlkEntry", (int32_t)bi), {
                            .layout = {
                                .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIXED(bh) },
                                .padding = { .left = 4, .right = 4, .top = 1, .bottom = 1 },
                                .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}
                            },
                            .backgroundColor = bcolor,
                            .cornerRadius = CLAY_CORNER_RADIUS(2)
                        }) {
                            if (bh > 14) {
                                Clay_String bs = {false, (int32_t)strlen(block_bufs[bi]), block_bufs[bi]};
                                CLAY_TEXT(bs, CLAY_TEXT_CONFIG({.textColor = {0, 0, 0, 200}, .fontSize = 16}));
                            }
                        }
                    }

                    /* Free space block */
                    {
                        uint32_t free_bytes = a->capacity - a->used;
                        float frac = (float)free_bytes / (float)a->capacity;
                        float fh = frac * block_total_h;
                        static char free_buf[32];
                        if (fh < 2.0f) fh = 2.0f;
                        snprintf(free_buf, sizeof(free_buf), "FREE %s", format_bytes(free_bytes));

                        CLAY(CLAY_ID("BlkFree"), {
                            .layout = {
                                .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIXED(fh) },
                                .padding = { .left = 4, .right = 4, .top = 1, .bottom = 1 },
                                .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}
                            },
                            .backgroundColor = {50, 50, 55, 255},
                            .cornerRadius = CLAY_CORNER_RADIUS(2)
                        }) {
                            if (fh > 14) {
                                Clay_String fs = {false, (int32_t)strlen(free_buf), free_buf};
                                CLAY_TEXT(fs, CLAY_TEXT_CONFIG({.textColor = {120, 120, 120, 255}, .fontSize = 16}));
                            }
                        }
                    }
                }
            }

            /* ===== GRID PANEL (right) — single placeholder, pixels built below ===== */
            CLAY(CLAY_ID("PGrid"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_GROW({0}) },
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

                /* Placeholder rect — sized by Clay, filled by GPU texture in externals */
                CLAY(CLAY_ID("PGridTex"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_GROW({0}) }
                    },
                    .backgroundColor = {50, 50, 55, 255}
                }) {}

                /* Legend */
                CLAY(CLAY_ID("GLegend"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIT({0}) },
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

    commands = Clay_EndLayout();
    e->profiler_cmd_count = commands.length;
    e->profiler_cmd_array = commands.internalArray;
    e->prof_click = 0;  /* consumed — zeroed after layout used it */

    /* Export scroll data for scrollbar rendering */
    {
        Clay_ScrollContainerData sd = Clay_GetScrollContainerData(CLAY_ID("PTreeScroll"));
        if (sd.found && sd.scrollPosition) {
            e->prof_scroll_pos = sd.scrollPosition->y;
            e->prof_content_h = sd.contentDimensions.height;
            e->prof_container_h = sd.scrollContainerDimensions.height;
        }
        /* Export PTreeScroll bounding box for scrollbar track positioning */
        {
            Clay_ElementData ed = Clay_GetElementData(CLAY_ID("PTreeScroll"));
            if (ed.found) {
                e->prof_track_x = ed.boundingBox.x;
                e->prof_track_y = ed.boundingBox.y;
                e->prof_track_w = ed.boundingBox.width;
                e->prof_track_h = ed.boundingBox.height;
            }
        }
    }

    /* Export PGridTex bounding box + build pixel buffer for GPU texture */
    {
        Clay_ElementData gd = Clay_GetElementData(CLAY_ID("PGridTex"));
        if (gd.found) {
            e->prof_grid_x  = gd.boundingBox.x;
            e->prof_grid_y  = gd.boundingBox.y;
            e->prof_grid_bw = gd.boundingBox.width;
            e->prof_grid_bh = gd.boundingBox.height;
        }

        /* Build grid pixel buffer: each pixel = one 64KB arena cell */
        {
            uint32_t cell_size = 64 * 1024;
            uint32_t total_cells = (a->capacity + cell_size - 1) / cell_size;
            int grid_cols = 16;
            int grid_rows = ((int)total_cells + grid_cols - 1) / grid_cols;
            int row, col;

            if (grid_cols > PROF_GRID_MAX_COLS) grid_cols = PROF_GRID_MAX_COLS;
            if (grid_rows > PROF_GRID_MAX_ROWS) grid_rows = PROF_GRID_MAX_ROWS;

            e->prof_grid_w = grid_cols;
            e->prof_grid_h = grid_rows;

            for (row = 0; row < grid_rows; row++) {
                for (col = 0; col < grid_cols; col++) {
                    uint32_t cell_idx = (uint32_t)(row * grid_cols + col);
                    uint8_t cr = 50, cg = 50, cb = 55, ca = 255; /* free = dark gray */
                    int pi = (row * grid_cols + col) * 4;
                    int fi;

                    if (cell_idx < total_cells) {
                        uint32_t cell_start = cell_idx * cell_size;
                        uint32_t cell_end = cell_start + cell_size;
                        if (cell_end > a->capacity) cell_end = a->capacity;

                        for (fi = 0; fi < flat_count; fi++) {
                            uint32_t rec_start = flat[fi].offset;
                            uint32_t rec_end = flat[fi].offset + flat[fi].size;
                            if (rec_start < cell_end && rec_end > cell_start) {
                                Clay_Color cc = profiler_colors[flat[fi].color_idx % profiler_color_count];
                                int is_hovered = have_hover &&
                                    flat[fi].offset >= hover_offset &&
                                    flat[fi].offset + flat[fi].size <= hover_offset + hover_size;
                                if (have_hover) {
                                    if (is_hovered) {
                                        cr = (uint8_t)(cc.r + (255 - cc.r) * 0.5f);
                                        cg = (uint8_t)(cc.g + (255 - cc.g) * 0.5f);
                                        cb = (uint8_t)(cc.b + (255 - cc.b) * 0.5f);
                                    } else {
                                        cr = (uint8_t)(cc.r * 0.3f);
                                        cg = (uint8_t)(cc.g * 0.3f);
                                        cb = (uint8_t)(cc.b * 0.3f);
                                    }
                                } else {
                                    cr = (uint8_t)cc.r;
                                    cg = (uint8_t)cc.g;
                                    cb = (uint8_t)cc.b;
                                }
                                break;
                            }
                        }
                    }

                    e->prof_grid_pixels[pi + 0] = cr;
                    e->prof_grid_pixels[pi + 1] = cg;
                    e->prof_grid_pixels[pi + 2] = cb;
                    e->prof_grid_pixels[pi + 3] = ca;
                }
            }
        }
    }
}

EXPORT void update_editor(game_state *gs, editor_state *es) {
    editor_state *e = es;
    dock_state *d = (dock_state *)e->dock;

    if (!e->open) return;

    /* ── Dock layout — compute pixel rects for all windows ── */
    {
        int wi;
        for (wi = 0; wi < MAX_DOCK_WINDOWS; wi++) {
            int ww, wh;
            if (!d->windows[wi].in_use || !d->windows[wi].sdl_window) continue;
            SDL_GetWindowSizeInPixels((SDL_Window *)d->windows[wi].sdl_window, &ww, &wh);
            dock_layout(d, wi, ww, wh);
        }
    }

    /* ── Game panel size (for ortho projection in engine) ── */
    {
        int game_win_idx;
        int game_node = dock_find_leaf_for_panel_global(d, PANEL_GAME, &game_win_idx);
        if (game_node >= 0) {
            DockNode *gn = &d->nodes[game_node];
            gs->width = (int)gn->w;
            gs->height = (int)(gn->h - DOCK_HEADER_HEIGHT);
            if (gs->height < 1) gs->height = 1;
        } else {
            int dw2, dh2;
            SDL_GetWindowSizeInPixels((SDL_Window *)d->windows[0].sdl_window, &dw2, &dh2);
            gs->width = dw2;
            gs->height = dh2;
        }
    }

    /* ── Editor panel rect (for coordinate transforms) ── */
    {
        int ed_win_idx;
        int ed_node = dock_find_leaf_for_panel_global(d, PANEL_EDITOR, &ed_win_idx);
        if (ed_node >= 0) {
            DockNode *en = &d->nodes[ed_node];
            e->panel_x = en->x;
            e->panel_y = en->y + DOCK_HEADER_HEIGHT;
            e->panel_w = en->w;
            e->panel_h = en->h - DOCK_HEADER_HEIGHT;
            e->window = (ed_win_idx >= 0) ? d->windows[ed_win_idx].sdl_window : d->windows[0].sdl_window;
        } else {
            int dw3, dh3;
            SDL_GetWindowSizeInPixels((SDL_Window *)d->windows[0].sdl_window, &dw3, &dh3);
            e->panel_x = 0; e->panel_y = 0;
            e->panel_w = (float)dw3; e->panel_h = (float)dh3;
            e->window = d->windows[0].sdl_window;
        }
    }

    /* ── Profiler Clay layout (produces render commands for externals) ── */
    profiler_layout(gs, es);

    /* ── Camera, gizmo, lines (existing editor behavior) ── */
    update_camera(gs, es);
    update_gizmo_hover(gs, es);
    build_lines(gs, es);
}

EXPORT void destroy_editor(game_state *gs, editor_state *es) {
    (void)gs; (void)es;
}

EXPORT int editor_handle_event(game_state *gs, editor_state *es, void *event_ptr) {
    editor_state *e = es;
    SDL_Event *ev = (SDL_Event *)event_ptr;
    dock_state *d = (dock_state *)es->dock;
    DragState *drag = &d->drag;
    ResizeState *resize = &d->resize;
    SDL_Window *evwin;

    /* ── Dock: Divider resize — mouse down to start ── */
    if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        ev->button.button == SDL_BUTTON_LEFT &&
        drag->phase == DRAG_IDLE && !resize->active) {
        evwin = SDL_GetWindowFromEvent(ev);
        {
            int win_idx = dock_window_for_sdl(d, evwin);
            if (win_idx >= 0) {
                float density = SDL_GetWindowPixelDensity(evwin);
                float pmx = ev->button.x * density;
                float pmy = ev->button.y * density;
                int hit = dock_divider_at_point(d, d->windows[win_idx].root_node, pmx, pmy);
                if (hit >= 0) {
                    DockNode *sn = &d->nodes[hit];
                    resize->active = 1;
                    resize->node = hit;
                    resize->window = win_idx;
                    resize->initial_ratio = sn->ratio;
                    resize->grab_pos = (sn->type == DOCK_SPLIT_H) ? pmx : pmy;
                    return 1;
                }
            }
        }
    }

    /* ── Dock: Divider resize — mouse motion to update ratio ── */
    if (ev->type == SDL_EVENT_MOUSE_MOTION && resize->active) {
        SDL_Window *rwin = (SDL_Window *)d->windows[resize->window].sdl_window;
        float density = SDL_GetWindowPixelDensity(rwin);
        float pmx = ev->motion.x * density;
        float pmy = ev->motion.y * density;
        DockNode *sn = &d->nodes[resize->node];
        float mouse_pos = (sn->type == DOCK_SPLIT_H) ? pmx : pmy;
        float extent    = (sn->type == DOCK_SPLIT_H) ? sn->w : sn->h;
        float origin    = (sn->type == DOCK_SPLIT_H) ? sn->x : sn->y;
        float new_ratio;
        if (extent < 1.0f) extent = 1.0f;
        new_ratio = (mouse_pos - origin) / extent;
        /* Clamp so neither child collapses below minimum size */
        {
            float min_frac = (float)DOCK_MIN_PANEL_SIZE / extent;
            if (new_ratio < min_frac) new_ratio = min_frac;
            if (new_ratio > 1.0f - min_frac) new_ratio = 1.0f - min_frac;
        }
        sn->ratio = new_ratio;
        return 1;
    }

    /* ── Dock: Divider resize — mouse up to finish ── */
    if (ev->type == SDL_EVENT_MOUSE_BUTTON_UP &&
        ev->button.button == SDL_BUTTON_LEFT &&
        resize->active) {
        resize->active = 0;
        return 1;
    }

    /* ── Dock: Left mouse button down — tab switch or drag start ── */
    if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        ev->button.button == SDL_BUTTON_LEFT &&
        drag->phase == DRAG_IDLE && !resize->active) {
        evwin = SDL_GetWindowFromEvent(ev);
        {
            int win_idx = dock_window_for_sdl(d, evwin);
            if (win_idx >= 0) {
                float density = SDL_GetWindowPixelDensity(evwin);
                float mx = ev->button.x * density;  /* logical → pixel */
                float my = ev->button.y * density;
                int hit_node = -1;
                PanelId hit_panel = PANEL_GAME;
                int hit_tab_idx = -1;
                if (header_hit_test_node(d, d->windows[win_idx].root_node,
                                         mx, my, &hit_node, &hit_panel, &hit_tab_idx)) {
                    DockNode *hn = &d->nodes[hit_node];
                    if (hit_tab_idx >= 0 && hit_tab_idx != hn->active_tab) {
                        /* Clicked an inactive tab — switch to it, no drag */
                        hn->active_tab = hit_tab_idx;
                        return 1;
                    }
                    /* Clicked the active tab — start drag.
                       Store grab position in logical coords (mx/my are pixel here,
                       so divide back) for consistent threshold check. */
                    drag->phase = DRAG_PENDING;
                    drag->panel = hit_panel;
                    drag->source_node = hit_node;
                    drag->source_window = win_idx;
                    drag->grab_x = ev->button.x;
                    drag->grab_y = ev->button.y;
                    return 1;
                }
            }
        }
    }

    /* ── Dock: Mouse motion — threshold, hover tracking, tear-off detection ── */
    if (ev->type == SDL_EVENT_MOUSE_MOTION && drag->phase != DRAG_IDLE) {
        float mx = ev->motion.x;
        float my = ev->motion.y;

        if (drag->phase == DRAG_PENDING) {
            float dx = mx - drag->grab_x;
            float dy = my - drag->grab_y;
            if (dx * dx + dy * dy > DOCK_DRAG_THRESHOLD * DOCK_DRAG_THRESHOLD) {
                drag->phase = DRAG_ACTIVE;
            }
        }

        if (drag->phase == DRAG_ACTIVE) {
            SDL_Window *src_win = (SDL_Window *)d->windows[drag->source_window].sdl_window;
            int ww, wh;
            SDL_GetWindowSize(src_win, &ww, &wh); /* logical coords for bounds check */
            if (mx < 0 || my < 0 || mx >= ww || my >= wh) {
                /* Mouse left source window — tear-off or redock as tab */
                float gx, gy;
                int found_other = -1;
                int dwi;
                SDL_GetGlobalMouseState(&gx, &gy);

                for (dwi = 0; dwi < MAX_DOCK_WINDOWS; dwi++) {
                    int owx, owy, oww, owh;
                    SDL_Window *ow;
                    if (dwi == drag->source_window) continue;
                    if (!d->windows[dwi].in_use || !d->windows[dwi].sdl_window) continue;
                    ow = (SDL_Window *)d->windows[dwi].sdl_window;
                    SDL_GetWindowPosition(ow, &owx, &owy);
                    SDL_GetWindowSize(ow, &oww, &owh);
                    if (gx >= owx && gx < owx + oww && gy >= owy && gy < owy + owh) {
                        found_other = dwi;
                        break;
                    }
                }

                if (found_other >= 0) {
                    d->cmd_redock = 1;
                    d->cmd_redock_target = found_other;
                } else {
                    d->cmd_tear_off = 1;
                    d->cmd_screen_x = gx;
                    d->cmd_screen_y = gy;
                }
                drag->hover_node = -1;
                drag->hover_zone = DROP_NONE;
                drag->phase = DRAG_IDLE;
            } else {
                /* Mouse inside source window — track hover for drop zones.
                   Convert logical mouse coords to pixel space to match
                   dock_layout which uses SDL_GetWindowSizeInPixels. */
                float density = SDL_GetWindowPixelDensity(src_win);
                float pmx = mx * density;
                float pmy = my * density;
                int root = d->windows[drag->source_window].root_node;
                int hover = dock_node_at_point(d, root, pmx, pmy);
                if (hover >= 0) {
                    DockNode *hn = &d->nodes[hover];
                    DropZone zone = dock_drop_zone(hn, pmx, pmy);
                    drag->hover_node = hover;
                    drag->hover_window = drag->source_window;
                    drag->hover_zone = zone;
                } else {
                    drag->hover_node = -1;
                    drag->hover_zone = DROP_NONE;
                }
            }
        }
        return 1;
    }

    /* ── Dock: Mouse button up — execute drop (split/tab) or cancel ── */
    if (ev->type == SDL_EVENT_MOUSE_BUTTON_UP &&
        ev->button.button == SDL_BUTTON_LEFT &&
        drag->phase != DRAG_IDLE) {
        if (drag->phase == DRAG_ACTIVE && drag->hover_node >= 0 &&
            drag->hover_zone != DROP_NONE) {
            int tgt = drag->hover_node;
            int win = drag->hover_window;
            DropZone zone = drag->hover_zone;
            PanelId panel = drag->panel;
            int src_node = drag->source_node;
            int src_win = drag->source_window;

            if (zone == DROP_CENTER) {
                /* Drop as tab into target node */
                DockNode *tn = &d->nodes[tgt];
                if (tn->panel_count < MAX_TABS_PER_NODE) {
                    tn->panels[tn->panel_count] = panel;
                    tn->panel_count++;
                    tn->active_tab = tn->panel_count - 1;
                }
            } else {
                /* Drop as split (LEFT/RIGHT/TOP/BOTTOM) */
                dock_split_at(d, win, tgt, panel, zone);
            }

            /* Remove panel from source and collapse empty nodes */
            dock_remove_panel(d, src_node, panel);
            {
                int new_root = dock_collapse_empty(d, d->windows[src_win].root_node);
                d->windows[src_win].root_node = new_root;
            }

            /* If source window became empty, signal externals to destroy it */
            if (d->windows[src_win].root_node < 0 && src_win != 0) {
                d->cmd_cleanup_windows = 1;
            }
        }
        drag->hover_node = -1;
        drag->hover_zone = DROP_NONE;
        drag->phase = DRAG_IDLE;
        return 1;
    }

    /* ── Dock: Cursor feedback for divider hover ── */
    if (ev->type == SDL_EVENT_MOUSE_MOTION &&
        drag->phase == DRAG_IDLE && !resize->active) {
        evwin = SDL_GetWindowFromEvent(ev);
        {
            int win_idx = dock_window_for_sdl(d, evwin);
            if (win_idx >= 0) {
                /* Cached cursors: survive within a single DLL load.
                   3 lightweight objects — negligible even if leaked on hot-reload. */
                static SDL_Cursor *cur_default = NULL;
                static SDL_Cursor *cur_sizewe  = NULL;
                static SDL_Cursor *cur_sizens  = NULL;
                float density = SDL_GetWindowPixelDensity(evwin);
                float pmx = ev->motion.x * density;
                float pmy = ev->motion.y * density;
                int hit = dock_divider_at_point(d, d->windows[win_idx].root_node, pmx, pmy);
                if (!cur_default) cur_default = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
                if (!cur_sizewe)  cur_sizewe  = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_EW_RESIZE);
                if (!cur_sizens)  cur_sizens  = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NS_RESIZE);
                if (hit >= 0) {
                    DockNode *sn = &d->nodes[hit];
                    SDL_SetCursor(sn->type == DOCK_SPLIT_H ? cur_sizewe : cur_sizens);
                } else {
                    SDL_SetCursor(cur_default);
                }
            }
        }
        /* Don't return — let other handlers see the motion event */
    }

    /* ── Editor-specific events below (require open panel) ── */
    if (!e->open || !e->window) return 0;

    /* Right-click mouse look toggle */
    if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        ev->button.button == SDL_BUTTON_RIGHT) {
        evwin = SDL_GetWindowFromEvent(ev);
        if (evwin == (SDL_Window *)e->window) {
            e->cam_mouse_look = 1;
            SDL_SetWindowRelativeMouseMode((SDL_Window *)e->window, 1);
        }
    }
    if (ev->type == SDL_EVENT_MOUSE_BUTTON_UP &&
        ev->button.button == SDL_BUTTON_RIGHT) {
        if (e->cam_mouse_look) {
            e->cam_mouse_look = 0;
            SDL_SetWindowRelativeMouseMode((SDL_Window *)e->window, 0);
        }
    }
    if (ev->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        evwin = SDL_GetWindowFromEvent(ev);
        if (evwin == (SDL_Window *)e->window && e->cam_mouse_look) {
            e->cam_mouse_look = 0;
            SDL_SetWindowRelativeMouseMode((SDL_Window *)e->window, 0);
        }
    }

    /* Gizmo: left-click to start drag */
    if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        ev->button.button == SDL_BUTTON_LEFT) {
        evwin = SDL_GetWindowFromEvent(ev);
        if (evwin == (SDL_Window *)e->window && e->gizmo_hovered != 0) {
            float fw2, fh2, ed_aspect, giz_dist, giz_len, sdx, sdy, slen;
            Mat4 ed_proj, ed_view, vp;
            Vec3 ed_fwd, giz, world_axis, center_s, tip_s;

            e->gizmo_active = e->gizmo_hovered;
            e->gizmo_drag_start_eye    = gs->mesh3d.camera_eye;
            e->gizmo_drag_start_target = gs->mesh3d.camera_target;
            e->gizmo_drag_accum = 0;

            fw2 = e->panel_w; fh2 = e->panel_h;
            ed_aspect = fw2 / fh2;
            ed_proj = mat4_perspective(60.0f * 3.14159265f / 180.0f, ed_aspect, 0.1f, 200.0f);
            ed_fwd  = cam_forward(e);
            ed_view = mat4_look_at(e->cam_pos, vec3_add(e->cam_pos, ed_fwd), VEC3(0, 1, 0));
            vp = mat4_mul(ed_proj, ed_view);

            giz = gs->mesh3d.camera_eye;
            giz_dist = vec3_len(vec3_sub(giz, e->cam_pos));
            giz_len  = giz_dist * 0.08f;
            if (giz_len < 0.1f) giz_len = 0.1f;

            world_axis = (e->gizmo_active == 1) ? VEC3(1, 0, 0) :
                         (e->gizmo_active == 2) ? VEC3(0, 1, 0) : VEC3(0, 0, 1);

            center_s = world_to_screen(giz, vp, fw2, fh2);
            tip_s    = world_to_screen(vec3_add(giz, vec3_scale(world_axis, giz_len)), vp, fw2, fh2);
            sdx = tip_s.x - center_s.x;
            sdy = tip_s.y - center_s.y;
            slen = sqrtf(sdx * sdx + sdy * sdy);
            if (slen > 0.001f) {
                e->gizmo_screen_axis = VEC3(sdx / slen, sdy / slen, 0);
                e->gizmo_world_per_pixel = giz_len / slen;
            } else {
                e->gizmo_screen_axis = VEC3(1, 0, 0);
                e->gizmo_world_per_pixel = 0.01f;
            }
        }
    }

    /* Gizmo: mouse motion during drag */
    if (ev->type == SDL_EVENT_MOUSE_MOTION && e->gizmo_active != 0) {
        float dot = ev->motion.xrel * e->gizmo_screen_axis.x
                  + ev->motion.yrel * e->gizmo_screen_axis.y;
        Vec3 world_axis, delta;
        e->gizmo_drag_accum += dot;

        world_axis = (e->gizmo_active == 1) ? VEC3(1, 0, 0) :
                     (e->gizmo_active == 2) ? VEC3(0, 1, 0) : VEC3(0, 0, 1);
        delta = vec3_scale(world_axis, e->gizmo_drag_accum * e->gizmo_world_per_pixel);
        gs->mesh3d.camera_eye    = vec3_add(e->gizmo_drag_start_eye, delta);
        gs->mesh3d.camera_target = vec3_add(e->gizmo_drag_start_target, delta);
    }

    /* Gizmo: left-button up ends drag */
    if (ev->type == SDL_EVENT_MOUSE_BUTTON_UP &&
        ev->button.button == SDL_BUTTON_LEFT) {
        e->gizmo_active = 0;
    }

    /* ── Profiler input: track mouse position + scroll for Clay scroll containers ── */
    if (ev->type == SDL_EVENT_MOUSE_MOTION) {
        evwin = SDL_GetWindowFromEvent(ev);
        {
            int pwin_idx;
            int pnode = dock_find_leaf_for_panel_global(d, PANEL_PROFILER, &pwin_idx);
            if (pnode >= 0 && pwin_idx >= 0 &&
                evwin == (SDL_Window *)d->windows[pwin_idx].sdl_window) {
                DockNode *pn = &d->nodes[pnode];
                float density = SDL_GetWindowPixelDensity(evwin);
                e->prof_mouse_x = ev->motion.x * density - pn->x;
                e->prof_mouse_y = ev->motion.y * density - pn->y - DOCK_HEADER_HEIGHT;
            }
        }
    }
    if (ev->type == SDL_EVENT_MOUSE_WHEEL) {
        evwin = SDL_GetWindowFromEvent(ev);
        {
            int pwin_idx;
            int pnode = dock_find_leaf_for_panel_global(d, PANEL_PROFILER, &pwin_idx);
            if (pnode >= 0 && pwin_idx >= 0 &&
                evwin == (SDL_Window *)d->windows[pwin_idx].sdl_window) {
                /* Accumulate — consumed and cleared by profiler_layout each frame.
                   Clay internally multiplies by 10, so 3.0 → ~30px per tick. */
                e->prof_scroll_y += ev->wheel.y * 3.0f;
            }
        }
    }
    if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev->button.button == SDL_BUTTON_LEFT) {
        e->prof_mouse_down = 1;
        e->prof_click = 1;
    }
    if (ev->type == SDL_EVENT_MOUSE_BUTTON_UP && ev->button.button == SDL_BUTTON_LEFT)
        e->prof_mouse_down = 0;

    return 0;
}
