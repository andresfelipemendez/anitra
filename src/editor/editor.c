#include <game.h>
#include <export.h>
#include <math3d.h>
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>

#define CLAY_IMPLEMENTATION
#include "clay.h"

#define PROFILER_MAX_FLAT_RECORDS 65536

enum {
    GIZMO_NONE = 0,
    GIZMO_TRANSLATE_X = 1,
    GIZMO_TRANSLATE_Y = 2,
    GIZMO_TRANSLATE_Z = 3,
    GIZMO_CAPSULE_RADIUS_POS_X = 4,
    GIZMO_CAPSULE_RADIUS_NEG_X = 5,
    GIZMO_CAPSULE_RADIUS_POS_Z = 6,
    GIZMO_CAPSULE_RADIUS_NEG_Z = 7,
    GIZMO_CAPSULE_HEIGHT_TOP = 8,
    GIZMO_CAPSULE_HEIGHT_BOTTOM = 9
};

enum {
    EDITOR_TOOLBAR_ACTION_NONE = 0,
    EDITOR_TOOLBAR_ACTION_PERSPECTIVE,
    EDITOR_TOOLBAR_ACTION_ORTHOGRAPHIC,
    EDITOR_TOOLBAR_ACTION_FRONT,
    EDITOR_TOOLBAR_ACTION_SIDE,
    EDITOR_TOOLBAR_ACTION_TOP,
    EDITOR_TOOLBAR_ACTION_PLAY_MODE
};

enum {
    EDITOR_VIEW_PRESET_FRONT = 0,
    EDITOR_VIEW_PRESET_SIDE,
    EDITOR_VIEW_PRESET_TOP
};

enum {
    MENU_NONE = -1,
    MENU_FILE = 0
};

enum {
    FILE_MENU_ACTION_OPEN_PROJECT = 0,
    FILE_MENU_ACTION_NEW_PROJECT,
    FILE_MENU_ACTION_COUNT
};

enum {
    SCENE_TREE_DROP_NONE = 0,
    SCENE_TREE_DROP_NESTED,
    SCENE_TREE_DROP_SIBLING_BEFORE,
    SCENE_TREE_DROP_SIBLING_AFTER
};

#define EDITOR_VIEWBAR_HEIGHT          42.0f
#define EDITOR_VIEWBAR_MARGIN_X        10.0f
#define EDITOR_VIEWBAR_MARGIN_Y         8.0f
#define EDITOR_VIEWBAR_ROW_HEIGHT      30.0f
#define EDITOR_VIEWBAR_ROW_PADDING_X    8.0f
#define EDITOR_VIEWBAR_BUTTON_HEIGHT   22.0f
#define EDITOR_VIEWBAR_TOGGLE_WIDTH   112.0f
#define EDITOR_VIEWBAR_VIEW_WIDTH      68.0f
#define EDITOR_VIEWBAR_PLAY_WIDTH      74.0f
#define EDITOR_VIEWBAR_BUTTON_GAP       6.0f
#define EDITOR_VIEWBAR_GROUP_GAP       14.0f

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

/* Collect all leaf records (flattened) for block + grid views */
typedef struct FlatRecord {
    const char *tag;
    uint32_t    offset;
    uint32_t    size;
    int         color_idx;
} FlatRecord;

/* Flatten arena into leaf records for block/grid views.
   If out is NULL, only advances color_id (used by tree collapse skip). */
static int profiler_flatten_arena(arena *a, uint32_t base_offset,
                                  FlatRecord *out, int count, int *color_id) {
    uint32_t i;
    for (i = 0; i < a->record_count; i++) {
        arena_record *r = &a->records[i];
        if (r->child && r->child->record_count > 0) {
            count = profiler_flatten_arena(r->child, base_offset + r->offset + (uint32_t)sizeof(arena),
                                           out, count, color_id);
        } else if (r->child) {
            if (out && count < PROFILER_MAX_FLAT_RECORDS) {
                out[count].tag = r->tag;
                out[count].offset = base_offset + r->offset;
                out[count].size = r->child->used > 0 ? r->child->used : r->size;
                out[count].color_idx = (*color_id);
                count++;
            } else if (!out) {
                count++;
            }
            (*color_id)++;
        } else {
            if (out && count < PROFILER_MAX_FLAT_RECORDS) {
                out[count].tag = r->tag;
                out[count].offset = base_offset + r->offset;
                out[count].size = r->size;
                out[count].color_idx = (*color_id);
                count++;
            } else if (!out) {
                count++;
            }
            (*color_id)++;
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
                                int *color_id, int click, int *collapsed_nodes) {
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
            static char size_bufs[PROFILER_TREE_MAX_NODES][32];
            static char label_bufs[PROFILER_TREE_MAX_NODES][48];
            int bidx = rid % PROFILER_TREE_MAX_NODES;
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
                    (rid < PROFILER_TREE_MAX_NODES && collapsed_nodes[rid]) ? ">" : "v", r->tag);
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
                if (hovered && click && is_parent && rid < PROFILER_TREE_MAX_NODES)
                    collapsed_nodes[rid] = !collapsed_nodes[rid];

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

        if (r->child && !(rid < PROFILER_TREE_MAX_NODES && collapsed_nodes[rid])) {
            profiler_tree_arena(r->child, depth + 1, row_id,
                                base_offset + r->offset + (uint32_t)sizeof(arena),
                                hover, color_id, click, collapsed_nodes);
        } else if (is_parent && rid < PROFILER_TREE_MAX_NODES && collapsed_nodes[rid]) {
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

static int find_parent_component(const game_state *gs, int entity_index, int *out_parent_index) {
    int i;
    if (!gs->parent_components) return 0;
    for (i = 0; i < gs->parent_component_count; i++) {
        parent_component *pc = &gs->parent_components[i];
        if (pc->entity_index == entity_index) {
            if (out_parent_index) *out_parent_index = pc->parent_entity_index;
            return 1;
        }
    }
    return 0;
}

static int has_parent_transform_component(const game_state *gs, int entity_index, int *out_parent_index) {
    int i;
    if (!gs->parent_transform_components) return 0;
    for (i = 0; i < gs->parent_transform_component_count; i++) {
        parent_transform_component *pt = &gs->parent_transform_components[i];
        if (pt->entity_index == entity_index) {
            if (out_parent_index) *out_parent_index = pt->parent_entity_index;
            return 1;
        }
    }
    return 0;
}

static int has_parent_rotation_component(const game_state *gs, int entity_index) {
    int i;
    if (!gs->parent_rotation_components) return 0;
    for (i = 0; i < gs->parent_rotation_component_count; i++) {
        parent_rotation_component *pr = &gs->parent_rotation_components[i];
        if (pr->entity_index == entity_index) return 1;
    }
    return 0;
}

static int has_mesh_component(const game_state *gs, int entity_index, int *out_visible) {
    int i;
    if (!gs->mesh_components) return 0;
    for (i = 0; i < gs->mesh_component_count; i++) {
        mesh_component *mc = &gs->mesh_components[i];
        if (mc->entity_index == entity_index) {
            if (out_visible) *out_visible = mc->visible;
            return 1;
        }
    }
    return 0;
}

static int has_animation_component(const game_state *gs, int entity_index,
                                   int *out_playing, int *out_clip, float *out_time, float *out_speed) {
    int i;
    if (!gs->animation_components) return 0;
    for (i = 0; i < gs->animation_component_count; i++) {
        animation_component *ac = &gs->animation_components[i];
        if (ac->entity_index == entity_index) {
            if (out_playing) *out_playing = ac->playing;
            if (out_clip) *out_clip = ac->active_clip;
            if (out_time) *out_time = ac->anim_time;
            if (out_speed) *out_speed = ac->speed;
            return 1;
        }
    }
    return 0;
}

static int has_transform_component(const game_state *gs, int entity_index, Vec3 *out_position) {
    int i;
    if (!gs->transform_components) return 0;
    for (i = 0; i < gs->transform_component_count; i++) {
        transform_component *tc = &gs->transform_components[i];
        if (tc->entity_index == entity_index) {
            if (out_position) *out_position = tc->position;
            return 1;
        }
    }
    return 0;
}

static transform_component *find_transform_component_mut(game_state *gs, int entity_index) {
    int i;
    if (!gs || !gs->transform_components) return NULL;
    for (i = 0; i < gs->transform_component_count; i++) {
        transform_component *tc = &gs->transform_components[i];
        if (tc->entity_index == entity_index) return tc;
    }
    return NULL;
}

static capsule_collider_component *find_capsule_collider_component_mut(game_state *gs, int entity_index) {
    int i;
    if (!gs || !gs->capsule_collider_components) return NULL;
    for (i = 0; i < gs->capsule_collider_component_count; i++) {
        capsule_collider_component *cc = &gs->capsule_collider_components[i];
        if (cc->entity_index == entity_index) return cc;
    }
    return NULL;
}

static int has_rotation_component(const game_state *gs, int entity_index, float *out_rotation_y_deg) {
    int i;
    if (!gs->rotation_components) return 0;
    for (i = 0; i < gs->rotation_component_count; i++) {
        rotation_component *rc = &gs->rotation_components[i];
        if (rc->entity_index == entity_index) {
            if (out_rotation_y_deg) *out_rotation_y_deg = rc->rotation_y_deg;
            return 1;
        }
    }
    return 0;
}

static int has_scale_component(const game_state *gs, int entity_index, Vec3 *out_scale) {
    int i;
    if (!gs->scale_components) return 0;
    for (i = 0; i < gs->scale_component_count; i++) {
        scale_component *sc = &gs->scale_components[i];
        if (sc->entity_index == entity_index) {
            if (out_scale) *out_scale = sc->scale;
            return 1;
        }
    }
    return 0;
}

static int has_velocity_component(const game_state *gs, int entity_index, vec2 *out_velocity) {
    int i;
    if (!gs->velocity_components) return 0;
    for (i = 0; i < gs->velocity_component_count; i++) {
        velocity_component *vc = &gs->velocity_components[i];
        if (vc->entity_index == entity_index) {
            if (out_velocity) *out_velocity = vc->velocity;
            return 1;
        }
    }
    return 0;
}

static int has_health_component(const game_state *gs, int entity_index, float *out_health, float *out_max) {
    int i;
    if (!gs->health_components) return 0;
    for (i = 0; i < gs->health_component_count; i++) {
        health_component *hc = &gs->health_components[i];
        if (hc->entity_index == entity_index) {
            if (out_health) *out_health = hc->health;
            if (out_max) *out_max = hc->max_health;
            return 1;
        }
    }
    return 0;
}

static int has_box_collider_component(const game_state *gs, int entity_index, rect *out_rect) {
    int i;
    if (!gs->box_collider_components) return 0;
    for (i = 0; i < gs->box_collider_component_count; i++) {
        box_collider_component *cc = &gs->box_collider_components[i];
        if (cc->entity_index == entity_index) {
            if (out_rect) *out_rect = cc->rect;
            return 1;
        }
    }
    return 0;
}

static int has_capsule_collider_component(const game_state *gs, int entity_index,
                                          float *out_radius, float *out_half_height,
                                          rect *out_aabb) {
    int i;
    if (!gs->capsule_collider_components) return 0;
    for (i = 0; i < gs->capsule_collider_component_count; i++) {
        capsule_collider_component *cc = &gs->capsule_collider_components[i];
        if (cc->entity_index == entity_index) {
            if (out_radius) *out_radius = cc->radius;
            if (out_half_height) *out_half_height = cc->half_height;
            if (out_aabb) *out_aabb = cc->aabb;
            return 1;
        }
    }
    return 0;
}

static rect capsule_aabb_rect(float radius, float half_height) {
    float rr = radius > 0.0f ? radius : 0.5f;
    float hh = half_height > 0.0f ? half_height : rr;
    rect r;
    r.x = 0.0f;
    r.y = 0.0f;
    r.w = rr * 2.0f;
    r.h = (hh + rr) * 2.0f;
    return r;
}

static int has_any_collider_component(const game_state *gs, int entity_index,
                                      rect *out_rect, int *out_is_capsule,
                                      float *out_radius, float *out_half_height) {
    rect box;
    rect capsule_aabb;
    float radius = 0.0f, half_height = 0.0f;
    if (has_capsule_collider_component(gs, entity_index, &radius, &half_height, &capsule_aabb)) {
        if (out_rect) *out_rect = capsule_aabb;
        if (out_is_capsule) *out_is_capsule = 1;
        if (out_radius) *out_radius = radius;
        if (out_half_height) *out_half_height = half_height;
        return 1;
    }
    if (has_box_collider_component(gs, entity_index, &box)) {
        if (out_rect) *out_rect = box;
        if (out_is_capsule) *out_is_capsule = 0;
        if (out_radius) *out_radius = 0.0f;
        if (out_half_height) *out_half_height = 0.0f;
        return 1;
    }
    return 0;
}

static int has_camera_component(const game_state *gs, int entity_index,
                                float *out_fov, float *out_near, float *out_far,
                                Vec3 *out_target, Vec3 *out_up) {
    int i;
    if (!gs->camera_components) return 0;
    for (i = 0; i < gs->camera_component_count; i++) {
        camera_component *cc = &gs->camera_components[i];
        if (cc->entity_index == entity_index) {
            if (out_fov) *out_fov = cc->fov_deg;
            if (out_near) *out_near = cc->near_plane;
            if (out_far) *out_far = cc->far_plane;
            if (out_target) *out_target = cc->target;
            if (out_up) *out_up = cc->up;
            return 1;
        }
    }
    return 0;
}

static int has_rigid_body_component(const game_state *gs, int entity_index, int *out_use_gravity) {
    int i;
    if (!gs->rigid_body_components) return 0;
    for (i = 0; i < gs->rigid_body_component_count; i++) {
        rigid_body_component *rb = &gs->rigid_body_components[i];
        if (rb->entity_index == entity_index) {
            if (out_use_gravity) *out_use_gravity = rb->use_gravity ? 1 : 0;
            return 1;
        }
    }
    return 0;
}

static int has_character_controller_component(const game_state *gs, int entity_index,
                                              float *out_move_speed, float *out_jump_speed) {
    int i;
    if (!gs->character_controller_components) return 0;
    for (i = 0; i < gs->character_controller_component_count; i++) {
        character_controller_component *cc = &gs->character_controller_components[i];
        if (cc->entity_index == entity_index) {
            if (out_move_speed) *out_move_speed = cc->move_speed;
            if (out_jump_speed) *out_jump_speed = cc->jump_speed;
            return 1;
        }
    }
    return 0;
}

static const mesh_component *find_mesh_component_read(const game_state *gs, int entity_index) {
    int i;
    if (!gs || !gs->mesh_components) return NULL;
    for (i = 0; i < gs->mesh_component_count; i++) {
        const mesh_component *mc = &gs->mesh_components[i];
        if (mc->entity_index == entity_index) return mc;
    }
    return NULL;
}

static const scene_model_asset *find_scene_model_asset_read(const game_state *gs, int asset_index) {
    if (!gs || asset_index < 0 || asset_index >= gs->scene_model_asset_count) return NULL;
    return &gs->scene_model_assets[asset_index];
}

static int string_starts_with(const char *s, const char *prefix) {
    int i;
    if (!s || !prefix) return 0;
    for (i = 0; prefix[i]; i++) {
        if (s[i] != prefix[i]) return 0;
    }
    return 1;
}

static void toml_write_escaped_string(FILE *fp, const char *text) {
    const unsigned char *p;
    if (!fp) return;
    if (!text) text = "";
    fputc('"', fp);
    p = (const unsigned char *)text;
    while (*p) {
        unsigned char c = *p++;
        if (c == '\\') {
            fputs("\\\\", fp);
        } else if (c == '"') {
            fputs("\\\"", fp);
        } else if (c == '\n') {
            fputs("\\n", fp);
        } else if (c == '\r') {
            fputs("\\r", fp);
        } else if (c == '\t') {
            fputs("\\t", fp);
        } else {
            fputc((int)c, fp);
        }
    }
    fputc('"', fp);
}

static void toml_write_assets_table(FILE *fp,
                                    const char *section_name,
                                    const char keys[][64],
                                    const char paths[][256],
                                    int count) {
    int i;
    if (!fp || !section_name || count <= 0) return;
    fprintf(fp, "[assets.%s]\n", section_name);
    for (i = 0; i < count; i++) {
        if (!keys[i][0] || !paths[i][0]) continue;
        fprintf(fp, "%-12s = ", keys[i]);
        toml_write_escaped_string(fp, paths[i]);
        fputc('\n', fp);
    }
    fputc('\n', fp);
}

static const char *scene_entity_name_for_save(const game_state *gs,
                                              int entity_index,
                                              char *fallback,
                                              int fallback_size) {
    if (gs &&
        entity_index >= 0 &&
        entity_index < gs->project.scene_entity_count &&
        gs->project.scene_entity_names[entity_index][0]) {
        return gs->project.scene_entity_names[entity_index];
    }

    if (!fallback || fallback_size <= 0) return "entity";
    snprintf(fallback, (size_t)fallback_size, "entity_%d", entity_index);
    return fallback;
}

static const char *mesh_model_key_for_save(const game_state *gs,
                                           int entity_index,
                                           char *fallback,
                                           int fallback_size) {
    const project_data *project;
    const mesh_component *mc;
    const scene_model_asset *asset;
    int i;
    if (!gs) return NULL;

    project = &gs->project;
    if (entity_index >= 0 &&
        entity_index < project->scene_entity_count &&
        project->scene_components[entity_index].has_mesh &&
        project->scene_components[entity_index].mesh_model[0]) {
        return project->scene_components[entity_index].mesh_model;
    }

    mc = find_mesh_component_read(gs, entity_index);
    if (!mc) return NULL;
    asset = find_scene_model_asset_read(gs, mc->model_asset_index);
    if (!asset) return NULL;

    if (asset->key[0] && !string_starts_with(asset->key, "asset_")) {
        return asset->key;
    }

    for (i = 0; i < project->model_count; i++) {
        if (strcmp(project->model_paths[i], asset->path) == 0) {
            return project->model_keys[i];
        }
    }
    for (i = 0; i < project->dungeon_piece_count; i++) {
        if (strcmp(project->dungeon_piece_paths[i], asset->path) == 0) {
            return project->dungeon_piece_keys[i];
        }
    }

    if (asset->key[0]) return asset->key;
    if (asset->path[0]) return asset->path;
    if (!fallback || fallback_size <= 0) return "model";
    snprintf(fallback, (size_t)fallback_size, "model_%d", entity_index);
    return fallback;
}

static const char *animation_asset_key_for_save(const game_state *gs,
                                                int entity_index,
                                                char *fallback,
                                                int fallback_size) {
    const project_data *project;
    const mesh_component *mc;
    const scene_model_asset *asset;
    int i;
    if (!gs) return NULL;

    project = &gs->project;
    if (entity_index >= 0 &&
        entity_index < project->scene_entity_count &&
        project->scene_components[entity_index].has_animation &&
        project->scene_components[entity_index].animation_asset[0]) {
        return project->scene_components[entity_index].animation_asset;
    }

    mc = find_mesh_component_read(gs, entity_index);
    if (mc) {
        asset = find_scene_model_asset_read(gs, mc->model_asset_index);
        if (asset && asset->animation_path[0]) {
            for (i = 0; i < project->animation_count; i++) {
                if (strcmp(project->animation_paths[i], asset->animation_path) == 0) {
                    return project->animation_keys[i];
                }
            }
        }
    }

    if (project->animation_count > 0 && project->animation_keys[0][0]) {
        return project->animation_keys[0];
    }

    if (!fallback || fallback_size <= 0) return NULL;
    fallback[0] = '\0';
    return fallback;
}

static int scene_entity_count_for_save(const game_state *gs) {
    int count = 0;
    if (!gs) return 0;
    if (gs->scene_entities && gs->scene_entity_count > 0) {
        count = gs->scene_entity_count;
    } else if (gs->project.scene_entity_count > 0) {
        count = gs->project.scene_entity_count;
    }
    if (count < 0) count = 0;
    if (count > PROJECT_SCENE_MAX_ENTITIES) count = PROJECT_SCENE_MAX_ENTITIES;
    return count;
}

static int editor_save_scene_to_toml(game_state *gs, const char *path) {
    FILE *fp;
    const project_data *project;
    int scene_count;
    int i;
    int count;

    if (!gs || !path || !path[0] || !gs->project_loaded) return 0;

    project = &gs->project;
    scene_count = scene_entity_count_for_save(gs);
    fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "editor save failed for '%s': %s\n", path, strerror(errno));
        return 0;
    }

    fprintf(fp, "# Saved by Anitra editor\n\n");

    fprintf(fp, "[project]\n");
    fprintf(fp, "name = ");
    toml_write_escaped_string(fp, project->name[0] ? project->name : "Anitra Scene");
    fputc('\n', fp);
    fprintf(fp, "version = %d\n\n", project->version > 0 ? project->version : 1);

    toml_write_assets_table(fp, "models", project->model_keys, project->model_paths, project->model_count);
    toml_write_assets_table(fp, "animations", project->animation_keys, project->animation_paths, project->animation_count);
    toml_write_assets_table(fp, "dungeon_pieces", project->dungeon_piece_keys, project->dungeon_piece_paths, project->dungeon_piece_count);
    toml_write_assets_table(fp, "sprites", project->sprite_keys, project->sprite_paths, project->sprite_count);

    if (project->has_camera) {
        fprintf(fp, "[camera]\n");
        fprintf(fp, "eye    = [%.4f, %.4f, %.4f]\n",
                project->camera.eye[0], project->camera.eye[1], project->camera.eye[2]);
        fprintf(fp, "target = [%.4f, %.4f, %.4f]\n",
                project->camera.target[0], project->camera.target[1], project->camera.target[2]);
        fprintf(fp, "up     = [%.4f, %.4f, %.4f]\n",
                project->camera.up[0], project->camera.up[1], project->camera.up[2]);
        fprintf(fp, "fov    = %.4f\n\n", project->camera.fov);
    }

    if (project->has_lighting) {
        fprintf(fp, "[lighting]\n");
        fprintf(fp, "ambient = [%.4f, %.4f, %.4f]\n\n",
                project->lighting.ambient[0],
                project->lighting.ambient[1],
                project->lighting.ambient[2]);
        for (i = 0; i < project->lighting.point_light_count; i++) {
            const project_point_light *pl = &project->lighting.point_lights[i];
            fprintf(fp, "[[lighting.point_lights]]\n");
            fprintf(fp, "position  = [%.4f, %.4f, %.4f]\n",
                    pl->position[0], pl->position[1], pl->position[2]);
            fprintf(fp, "color     = [%.4f, %.4f, %.4f]\n",
                    pl->color[0], pl->color[1], pl->color[2]);
            fprintf(fp, "intensity = %.4f\n", pl->intensity);
            fprintf(fp, "radius    = %.4f\n\n", pl->radius);
        }
    }

    fprintf(fp, "[entities]\n");
    fprintf(fp, "list = [\n");
    for (i = 0; i < scene_count; i++) {
        char name_buf[64];
        const char *entity_name = scene_entity_name_for_save(gs, i, name_buf, (int)sizeof(name_buf));
        fprintf(fp, "  ");
        toml_write_escaped_string(fp, entity_name);
        if (i + 1 < scene_count) fputc(',', fp);
        fputc('\n', fp);
    }
    fprintf(fp, "]\n\n");

    count = 0;
    for (i = 0; i < scene_count; i++) if (has_transform_component(gs, i, NULL)) count++;
    if (count > 0) {
        fprintf(fp, "[transforms]\n");
        for (i = 0; i < scene_count; i++) {
            Vec3 pos;
            if (!has_transform_component(gs, i, &pos)) continue;
            fprintf(fp, "\"%d\" = { position = [%.4f, %.4f, %.4f] }\n", i, pos.x, pos.y, pos.z);
        }
        fputc('\n', fp);
    }

    count = 0;
    for (i = 0; i < scene_count; i++) if (has_rotation_component(gs, i, NULL)) count++;
    if (count > 0) {
        fprintf(fp, "[rotations]\n");
        for (i = 0; i < scene_count; i++) {
            float y = 0.0f;
            if (!has_rotation_component(gs, i, &y)) continue;
            fprintf(fp, "\"%d\" = { y = %.4f }\n", i, y);
        }
        fputc('\n', fp);
    }

    count = 0;
    for (i = 0; i < scene_count; i++) if (has_scale_component(gs, i, NULL)) count++;
    if (count > 0) {
        fprintf(fp, "[scales]\n");
        for (i = 0; i < scene_count; i++) {
            Vec3 sc;
            if (!has_scale_component(gs, i, &sc)) continue;
            fprintf(fp, "\"%d\" = { scale = [%.4f, %.4f, %.4f] }\n", i, sc.x, sc.y, sc.z);
        }
        fputc('\n', fp);
    }

    count = 0;
    for (i = 0; i < scene_count; i++) if (has_parent_transform_component(gs, i, NULL)) count++;
    if (count > 0) {
        fprintf(fp, "[parent_transform]\n");
        for (i = 0; i < scene_count; i++) {
            int parent = -1;
            if (!has_parent_transform_component(gs, i, &parent)) continue;
            fprintf(fp, "\"%d\" = { parent = %d }\n", i, parent);
        }
        fputc('\n', fp);
    }

    count = 0;
    for (i = 0; i < scene_count; i++) if (has_parent_rotation_component(gs, i)) count++;
    if (count > 0) {
        fprintf(fp, "[parent_rotation]\n");
        for (i = 0; i < scene_count; i++) {
            if (!has_parent_rotation_component(gs, i)) continue;
            fprintf(fp, "\"%d\" = { enabled = true }\n", i);
        }
        fputc('\n', fp);
    }

    count = 0;
    for (i = 0; i < scene_count; i++) if (has_mesh_component(gs, i, NULL)) count++;
    if (count > 0) {
        fprintf(fp, "[meshes]\n");
        for (i = 0; i < scene_count; i++) {
            int visible = 1;
            const char *model_key;
            char model_buf[96];
            if (!has_mesh_component(gs, i, &visible)) continue;
            model_key = mesh_model_key_for_save(gs, i, model_buf, (int)sizeof(model_buf));
            if (!model_key || !model_key[0]) continue;
            fprintf(fp, "\"%d\" = { model = ", i);
            toml_write_escaped_string(fp, model_key);
            fprintf(fp, ", visible = %s }\n", visible ? "true" : "false");
        }
        fputc('\n', fp);
    }

    count = 0;
    for (i = 0; i < scene_count; i++) if (has_animation_component(gs, i, NULL, NULL, NULL, NULL)) count++;
    if (count > 0) {
        fprintf(fp, "[animations]\n");
        for (i = 0; i < scene_count; i++) {
            int playing = 0;
            int clip = 0;
            float speed = 1.0f;
            const char *asset_key;
            char asset_buf[96];
            if (!has_animation_component(gs, i, &playing, &clip, NULL, &speed)) continue;
            asset_key = animation_asset_key_for_save(gs, i, asset_buf, (int)sizeof(asset_buf));
            fprintf(fp, "\"%d\" = { ", i);
            if (asset_key && asset_key[0]) {
                fprintf(fp, "asset = ");
                toml_write_escaped_string(fp, asset_key);
                fprintf(fp, ", ");
            }
            fprintf(fp, "playing = %s, clip = %d, speed = %.4f }\n",
                    playing ? "true" : "false", clip, speed);
        }
        fputc('\n', fp);
    }

    count = 0;
    for (i = 0; i < scene_count; i++) if (has_velocity_component(gs, i, NULL)) count++;
    if (count > 0) {
        fprintf(fp, "[velocities]\n");
        for (i = 0; i < scene_count; i++) {
            vec2 vel;
            if (!has_velocity_component(gs, i, &vel)) continue;
            fprintf(fp, "\"%d\" = { value = [%.4f, %.4f] }\n", i, vel.x, vel.y);
        }
        fputc('\n', fp);
    }

    count = 0;
    for (i = 0; i < scene_count; i++) if (has_rigid_body_component(gs, i, NULL)) count++;
    if (count > 0) {
        fprintf(fp, "[rigid_bodies]\n");
        for (i = 0; i < scene_count; i++) {
            int use_gravity = 1;
            if (!has_rigid_body_component(gs, i, &use_gravity)) continue;
            fprintf(fp, "\"%d\" = { use_gravity = %s }\n", i, use_gravity ? "true" : "false");
        }
        fputc('\n', fp);
    }

    count = 0;
    for (i = 0; i < scene_count; i++) if (has_character_controller_component(gs, i, NULL, NULL)) count++;
    if (count > 0) {
        fprintf(fp, "[character_controllers]\n");
        for (i = 0; i < scene_count; i++) {
            float move_speed = 5.0f;
            float jump_speed = 8.5f;
            if (!has_character_controller_component(gs, i, &move_speed, &jump_speed)) continue;
            fprintf(fp, "\"%d\" = { move_speed = %.4f, jump_speed = %.4f }\n", i, move_speed, jump_speed);
        }
        fputc('\n', fp);
    }

    count = 0;
    for (i = 0; i < scene_count; i++) if (has_health_component(gs, i, NULL, NULL)) count++;
    if (count > 0) {
        fprintf(fp, "[health]\n");
        for (i = 0; i < scene_count; i++) {
            float current = 0.0f, max = 0.0f;
            if (!has_health_component(gs, i, &current, &max)) continue;
            fprintf(fp, "\"%d\" = { current = %.4f, max = %.4f }\n", i, current, max);
        }
        fputc('\n', fp);
    }

    count = 0;
    for (i = 0; i < scene_count; i++) if (has_box_collider_component(gs, i, NULL)) count++;
    if (count > 0) {
        fprintf(fp, "[box_colliders]\n");
        for (i = 0; i < scene_count; i++) {
            rect box;
            float hx, hy, hz;
            if (!has_box_collider_component(gs, i, &box)) continue;
            hx = fabsf(box.w) * 0.5f;
            hz = fabsf(box.h) * 0.5f;
            hy = 0.5f;
            if (i >= 0 &&
                i < project->scene_entity_count &&
                project->scene_components[i].has_box_collider &&
                project->scene_components[i].box_collider_half_extents[1] > 0.0f) {
                hy = project->scene_components[i].box_collider_half_extents[1];
            }
            fprintf(fp, "\"%d\" = { type = \"box\", half_extents = [%.4f, %.4f, %.4f] }\n", i, hx, hy, hz);
        }
        fputc('\n', fp);
    }

    count = 0;
    for (i = 0; i < scene_count; i++) if (has_capsule_collider_component(gs, i, NULL, NULL, NULL)) count++;
    if (count > 0) {
        fprintf(fp, "[capsule_colliders]\n");
        for (i = 0; i < scene_count; i++) {
            float radius = 0.0f;
            float half_height = 0.0f;
            if (!has_capsule_collider_component(gs, i, &radius, &half_height, NULL)) continue;
            fprintf(fp, "\"%d\" = { radius = %.4f, half_height = %.4f }\n", i, radius, half_height);
        }
        fputc('\n', fp);
    }

    count = 0;
    for (i = 0; i < scene_count; i++) if (has_camera_component(gs, i, NULL, NULL, NULL, NULL, NULL)) count++;
    if (count > 0) {
        fprintf(fp, "[cameras]\n");
        for (i = 0; i < scene_count; i++) {
            float fov = 60.0f, near_plane = 0.1f, far_plane = 100.0f;
            Vec3 target = VEC3(0.0f, 0.0f, 0.0f);
            Vec3 up = VEC3(0.0f, 1.0f, 0.0f);
            if (!has_camera_component(gs, i, &fov, &near_plane, &far_plane, &target, &up)) continue;
            fprintf(fp, "\"%d\" = { fov = %.4f, near = %.4f, far = %.4f, target = [%.4f, %.4f, %.4f], up = [%.4f, %.4f, %.4f] }\n",
                    i, fov, near_plane, far_plane, target.x, target.y, target.z, up.x, up.y, up.z);
        }
        fputc('\n', fp);
    }

    if (fclose(fp) != 0) {
        fprintf(stderr, "editor save failed closing '%s': %s\n", path, strerror(errno));
        return 0;
    }

    return 1;
}

static void build_scene_parent_lookup(const game_state *gs, int entity_count, int *parent_of) {
    int i;
    for (i = 0; i < entity_count; i++) parent_of[i] = -1;
    if (gs->parent_components) {
        for (i = 0; i < gs->parent_component_count; i++) {
            parent_component *pc = &gs->parent_components[i];
            if (pc->entity_index < 0 || pc->entity_index >= entity_count) continue;
            parent_of[pc->entity_index] = pc->parent_entity_index;
        }
    }
    if (!gs->parent_transform_components) return;

    for (i = 0; i < gs->parent_transform_component_count; i++) {
        parent_transform_component *pt = &gs->parent_transform_components[i];
        if (pt->entity_index < 0 || pt->entity_index >= entity_count) continue;
        if (pt->parent_entity_index < 0 || pt->parent_entity_index >= entity_count) continue;
        if (parent_of[pt->entity_index] >= 0) continue;
        parent_of[pt->entity_index] = pt->parent_entity_index;
    }
}

static void remove_parent_transform_component(game_state *gs, int entity_index) {
    int read_i;
    int write_i = 0;
    if (!gs || !gs->parent_transform_components) return;

    for (read_i = 0; read_i < gs->parent_transform_component_count; read_i++) {
        parent_transform_component pt = gs->parent_transform_components[read_i];
        if (pt.entity_index == entity_index) continue;
        gs->parent_transform_components[write_i++] = pt;
    }
    gs->parent_transform_component_count = write_i;
}

static void set_parent_transform_component(game_state *gs, int entity_index, int parent_entity_index) {
    int i;
    if (!gs) return;
    if (entity_index < 0 || entity_index >= gs->scene_entity_count) return;
    if (parent_entity_index < 0 || parent_entity_index >= gs->scene_entity_count) {
        remove_parent_transform_component(gs, entity_index);
        return;
    }
    if (entity_index == parent_entity_index) return;

    remove_parent_transform_component(gs, entity_index);
    if (!gs->parent_transform_components) return;
    if (gs->parent_transform_component_count >= gs->parent_transform_component_capacity) return;

    i = gs->parent_transform_component_count++;
    gs->parent_transform_components[i].entity_index = entity_index;
    gs->parent_transform_components[i].parent_entity_index = parent_entity_index;
}

static int scene_tree_parent_assignment_is_valid(int child_entity, int parent_entity,
                                                 const int *parent_of, int entity_count) {
    int current;
    int guard = 0;

    if (!parent_of || entity_count <= 0) return 0;
    if (child_entity < 0 || child_entity >= entity_count) return 0;
    if (parent_entity < -1 || parent_entity >= entity_count) return 0;
    if (parent_entity == -1) return 1;
    if (child_entity == parent_entity) return 0;

    current = parent_entity;
    while (current >= 0 && current < entity_count && guard < entity_count) {
        if (current == child_entity) return 0;
        current = parent_of[current];
        guard++;
    }

    return 1;
}

static void scene_tree_emit_entity_row(const game_state *gs, editor_state *e,
                                       int entity_index, int depth, int click,
                                       const int *parent_of, int entity_count) {
    static char row_bufs[SCENE_TREE_MAX_ENTITIES][64];
    int bi = entity_index % SCENE_TREE_MAX_ENTITIES;
    int selected = (e->scene_selected_entity == entity_index);
    int dragging = e->scene_tree_drag_active && e->scene_tree_mouse_down;
    int sibling_parent = -1;
    int nested_valid = scene_tree_parent_assignment_is_valid(e->scene_tree_drag_entity, entity_index,
                                                              parent_of, entity_count);
    int sibling_valid;
    const char *name = NULL;

    if (entity_index >= 0 && entity_index < entity_count) {
        sibling_parent = parent_of[entity_index];
        if (sibling_parent < 0 || sibling_parent >= entity_count) sibling_parent = -1;
    }
    sibling_valid = scene_tree_parent_assignment_is_valid(e->scene_tree_drag_entity, sibling_parent,
                                                          parent_of, entity_count);

    if (gs && entity_index >= 0 && entity_index < gs->project.scene_entity_count) {
        if (gs->project.scene_entity_names[entity_index][0]) {
            name = gs->project.scene_entity_names[entity_index];
        }
    }

    if (name) {
        snprintf(row_bufs[bi], sizeof(row_bufs[bi]), "%s", name);
    } else {
        snprintf(row_bufs[bi], sizeof(row_bufs[bi]), "Entity %d", entity_index);
    }

    CLAY(CLAY_IDI("STEntityRow", (int32_t)entity_index), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIT({0}) },
            .childGap = 0,
            .layoutDirection = CLAY_TOP_TO_BOTTOM
        }
    }) {
        CLAY(CLAY_IDI("STEntityRowTop", (int32_t)entity_index), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIXED(2) }
            },
            .backgroundColor = (dragging && entity_index != e->scene_tree_drag_entity && Clay_Hovered())
                                   ? (sibling_valid ? ((Clay_Color){102, 170, 238, 255})
                                                    : ((Clay_Color){214, 98, 98, 255}))
                                   : ((Clay_Color){0, 0, 0, 0})
        }) {
            int hovered = Clay_Hovered();
            if (hovered && click) {
                e->scene_selected_entity = entity_index;
                e->scene_tree_drag_active = 1;
                e->scene_tree_drag_entity = entity_index;
                e->scene_tree_drop_target = -1;
                e->scene_tree_drop_mode = SCENE_TREE_DROP_NONE;
            }
            if (hovered && dragging && entity_index != e->scene_tree_drag_entity) {
                e->scene_tree_drop_target = entity_index;
                e->scene_tree_drop_mode = SCENE_TREE_DROP_SIBLING_BEFORE;
            }
        }

        CLAY(CLAY_IDI("STEntityRowBody", (int32_t)entity_index), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIT({0}) },
                .padding = { .left = (uint16_t)(8 + depth * 16), .right = 8, .top = 5, .bottom = 5 }
            },
            .backgroundColor = (dragging && entity_index != e->scene_tree_drag_entity && Clay_Hovered())
                                   ? (nested_valid ? ((Clay_Color){64, 94, 136, 255})
                                                   : ((Clay_Color){120, 60, 60, 255}))
                                   : ((dragging && entity_index == e->scene_tree_drag_entity)
                                          ? ((Clay_Color){74, 74, 74, 255})
                                          : (selected ? ((Clay_Color){52, 62, 84, 255})
                                                      : (Clay_Hovered() ? ((Clay_Color){48, 56, 72, 255})
                                                                        : ((Clay_Color){0, 0, 0, 0})))),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            int hovered = Clay_Hovered();
            if (hovered && click) {
                e->scene_selected_entity = entity_index;
                e->scene_tree_drag_active = 1;
                e->scene_tree_drag_entity = entity_index;
                e->scene_tree_drop_target = -1;
                e->scene_tree_drop_mode = SCENE_TREE_DROP_NONE;
            }
            if (hovered && dragging && entity_index != e->scene_tree_drag_entity) {
                e->scene_tree_drop_target = entity_index;
                e->scene_tree_drop_mode = SCENE_TREE_DROP_NESTED;
            }

            Clay_String cs = {false, (int32_t)strlen(row_bufs[bi]), row_bufs[bi]};
            CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {224, 230, 240, 255}, .fontSize = 16}));
        }

        CLAY(CLAY_IDI("STEntityRowBottom", (int32_t)entity_index), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIXED(2) }
            },
            .backgroundColor = (dragging && entity_index != e->scene_tree_drag_entity && Clay_Hovered())
                                   ? (sibling_valid ? ((Clay_Color){102, 170, 238, 255})
                                                    : ((Clay_Color){214, 98, 98, 255}))
                                   : ((Clay_Color){0, 0, 0, 0})
        }) {
            int hovered = Clay_Hovered();
            if (hovered && click) {
                e->scene_selected_entity = entity_index;
                e->scene_tree_drag_active = 1;
                e->scene_tree_drag_entity = entity_index;
                e->scene_tree_drop_target = -1;
                e->scene_tree_drop_mode = SCENE_TREE_DROP_NONE;
            }
            if (hovered && dragging && entity_index != e->scene_tree_drag_entity) {
                e->scene_tree_drop_target = entity_index;
                e->scene_tree_drop_mode = SCENE_TREE_DROP_SIBLING_AFTER;
            }
        }
    }
}

static void scene_tree_emit_entity_recursive(const game_state *gs, editor_state *e,
                                             int entity_index, int depth, int click,
                                             const int *parent_of, int entity_count,
                                             int *visited) {
    int child;
    if (entity_index < 0 || entity_index >= entity_count) return;
    if (visited[entity_index]) return;

    visited[entity_index] = 1;
    scene_tree_emit_entity_row(gs, e, entity_index, depth, click, parent_of, entity_count);

    for (child = 0; child < entity_count; child++) {
        if (parent_of[child] == entity_index && child != entity_index) {
            scene_tree_emit_entity_recursive(gs, e, child, depth + 1, click,
                                             parent_of, entity_count, visited);
        }
    }
}

static int panel_event_hit(dock_state *d, PanelId panel, SDL_Window *evwin,
                           float mx, float my, float *out_lx, float *out_ly) {
    int pwin_idx;
    int pnode = dock_find_leaf_for_panel_global(d, panel, &pwin_idx);
    if (pnode < 0 || pwin_idx < 0) return 0;
    if (evwin != (SDL_Window *)d->windows[pwin_idx].sdl_window) return 0;
    {
        DockNode *pn = &d->nodes[pnode];
        float lx = mx - pn->x;
        float ly = my - pn->y - DOCK_HEADER_HEIGHT;
        float pw = pn->w;
        float ph = pn->h - DOCK_HEADER_HEIGHT;
        if (ph < 1) ph = 1;
        if (lx < 0 || ly < 0 || lx >= pw || ly >= ph) return 0;
        if (out_lx) *out_lx = lx;
        if (out_ly) *out_ly = ly;
    }
    return 1;
}

/* ── Internal helpers ─────────────────────────────────────────── */

static Vec3 cam_forward(editor_state *e) {
    return VEC3(
        cosf(e->cam_pitch) * sinf(e->cam_yaw),
        sinf(e->cam_pitch),
        cosf(e->cam_pitch) * cosf(e->cam_yaw)
    );
}

static float editor_ortho_size(const editor_state *e) {
    float sz = (e && e->cam_ortho_size > 0.05f) ? e->cam_ortho_size : 12.0f;
    if (sz < 0.5f) sz = 0.5f;
    return sz;
}

static Mat4 editor_projection_matrix(const editor_state *e, float panel_w, float panel_h) {
    float safe_h = panel_h > 1.0f ? panel_h : 1.0f;
    float aspect = panel_w / safe_h;
    if (aspect < 0.01f) aspect = 0.01f;

    if (e && e->cam_projection_mode == EDITOR_CAMERA_ORTHOGRAPHIC) {
        float half_h = editor_ortho_size(e) * 0.5f;
        float half_w = half_h * aspect;
        return mat4_orthographic(-half_w, half_w, -half_h, half_h, 0.1f, 200.0f);
    }

    return mat4_perspective(60.0f * 3.14159265f / 180.0f, aspect, 0.1f, 200.0f);
}

static int point_in_rect(float px, float py, float x, float y, float w, float h) {
    return px >= x && py >= y && px < (x + w) && py < (y + h);
}

static int editor_is_play_mode(const game_state *gs) {
    return gs && gs->editor_play_mode;
}

static int menu_bar_contains_point(dock_state *d, SDL_Window *evwin, float mx, float my) {
    int win_w = 0;
    if (!d || !d->windows[0].in_use || !d->windows[0].sdl_window) return 0;
    if (evwin != (SDL_Window *)d->windows[0].sdl_window) return 0;
    SDL_GetWindowSize((SDL_Window *)d->windows[0].sdl_window, &win_w, NULL);
    return point_in_rect(mx, my, 0.0f, 0.0f, (float)win_w, (float)MENU_BAR_HEIGHT);
}

static int file_menu_dropdown_contains_point(float mx, float my) {
    const float dropdown_x = 6.0f;
    const float dropdown_y = 24.0f;
    const float dropdown_w = 190.0f;
    const float dropdown_h = 4.0f + (FILE_MENU_ACTION_COUNT * 24.0f) + ((FILE_MENU_ACTION_COUNT - 1) * 2.0f) + 4.0f;
    return point_in_rect(mx, my, dropdown_x, dropdown_y, dropdown_w, dropdown_h);
}

static void editor_dispatch_file_menu_action(game_state *gs, int action) {
    (void)gs;
    switch (action) {
    case FILE_MENU_ACTION_OPEN_PROJECT:
        fprintf(stderr, "Menu action: Open Project (not implemented yet)\n");
        break;
    case FILE_MENU_ACTION_NEW_PROJECT:
        fprintf(stderr, "Menu action: New Project (not implemented yet)\n");
        break;
    default:
        break;
    }
}

static int editor_toolbar_contains_point(const editor_state *e, float lx, float ly) {
    if (!e) return 0;
    if (lx < 0.0f || lx >= e->panel_w) return 0;
    return ly >= 0.0f && ly < EDITOR_VIEWBAR_HEIGHT;
}

static int editor_toolbar_button_hit(const editor_state *e, float lx, float ly) {
    float btn_y = EDITOR_VIEWBAR_MARGIN_Y + (EDITOR_VIEWBAR_ROW_HEIGHT - EDITOR_VIEWBAR_BUTTON_HEIGHT) * 0.5f;
    float btn_x = EDITOR_VIEWBAR_MARGIN_X + EDITOR_VIEWBAR_ROW_PADDING_X;
    (void)e;

    if (point_in_rect(lx, ly, btn_x, btn_y, EDITOR_VIEWBAR_TOGGLE_WIDTH, EDITOR_VIEWBAR_BUTTON_HEIGHT))
        return EDITOR_TOOLBAR_ACTION_PERSPECTIVE;
    btn_x += EDITOR_VIEWBAR_TOGGLE_WIDTH + EDITOR_VIEWBAR_BUTTON_GAP;

    if (point_in_rect(lx, ly, btn_x, btn_y, EDITOR_VIEWBAR_TOGGLE_WIDTH, EDITOR_VIEWBAR_BUTTON_HEIGHT))
        return EDITOR_TOOLBAR_ACTION_ORTHOGRAPHIC;
    btn_x += EDITOR_VIEWBAR_TOGGLE_WIDTH + EDITOR_VIEWBAR_GROUP_GAP;

    if (point_in_rect(lx, ly, btn_x, btn_y, EDITOR_VIEWBAR_VIEW_WIDTH, EDITOR_VIEWBAR_BUTTON_HEIGHT))
        return EDITOR_TOOLBAR_ACTION_FRONT;
    btn_x += EDITOR_VIEWBAR_VIEW_WIDTH + EDITOR_VIEWBAR_BUTTON_GAP;

    if (point_in_rect(lx, ly, btn_x, btn_y, EDITOR_VIEWBAR_VIEW_WIDTH, EDITOR_VIEWBAR_BUTTON_HEIGHT))
        return EDITOR_TOOLBAR_ACTION_SIDE;
    btn_x += EDITOR_VIEWBAR_VIEW_WIDTH + EDITOR_VIEWBAR_BUTTON_GAP;

    if (point_in_rect(lx, ly, btn_x, btn_y, EDITOR_VIEWBAR_VIEW_WIDTH, EDITOR_VIEWBAR_BUTTON_HEIGHT))
        return EDITOR_TOOLBAR_ACTION_TOP;
    btn_x += EDITOR_VIEWBAR_VIEW_WIDTH + EDITOR_VIEWBAR_GROUP_GAP;

    if (point_in_rect(lx, ly, btn_x, btn_y, EDITOR_VIEWBAR_PLAY_WIDTH, EDITOR_VIEWBAR_BUTTON_HEIGHT))
        return EDITOR_TOOLBAR_ACTION_PLAY_MODE;

    return EDITOR_TOOLBAR_ACTION_NONE;
}

static void editor_begin_mouse_look(editor_state *e, uint8_t button) {
    float flush_dx, flush_dy;
    if (!e || !e->window || e->cam_mouse_look) return;
    e->cam_mouse_look = 1;
    e->cam_mouse_button = button;
    SDL_SetWindowRelativeMouseMode((SDL_Window *)e->window, 1);
    /* Clear any accumulated relative delta so drag starts from click point. */
    SDL_GetRelativeMouseState(&flush_dx, &flush_dy);
}

static void editor_end_mouse_look(editor_state *e) {
    if (!e || !e->window || !e->cam_mouse_look) return;
    e->cam_mouse_look = 0;
    e->cam_mouse_button = 0;
    SDL_SetWindowRelativeMouseMode((SDL_Window *)e->window, 0);
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

static Vec3 editor_transform_point(Mat4 m, Vec3 p);
static float editor_gizmo_axis_len(const editor_state *e, Vec3 origin);
static float editor_handle_draw_size(const editor_state *e, Vec3 origin);

typedef struct capsule_handle_info {
    int id;
    Vec3 position;
    Vec3 axis_world;
    float axis_local_scale;
} capsule_handle_info;

static void editor_world_basis_axes(Mat4 world, Vec3 out_axes[3], float out_scales[3]) {
    Vec3 raw_axes[3];
    int i;
    raw_axes[0] = VEC3(world.m[0], world.m[1], world.m[2]);
    raw_axes[1] = VEC3(world.m[4], world.m[5], world.m[6]);
    raw_axes[2] = VEC3(world.m[8], world.m[9], world.m[10]);
    for (i = 0; i < 3; i++) {
        float scale = vec3_len(raw_axes[i]);
        if (scale < 0.001f) {
            scale = 1.0f;
            out_axes[i] = (i == 0) ? VEC3(1.0f, 0.0f, 0.0f)
                                    : ((i == 1) ? VEC3(0.0f, 1.0f, 0.0f)
                                                : VEC3(0.0f, 0.0f, 1.0f));
        } else {
            out_axes[i] = vec3_scale(raw_axes[i], 1.0f / scale);
        }
        if (out_scales) out_scales[i] = scale;
    }
}

static void setup_drag_screen_axis(editor_state *e, Mat4 vp, float fw2, float fh2,
                                   Vec3 drag_origin, Vec3 axis_world, float axis_len) {
    Vec3 center_s = world_to_screen(drag_origin, vp, fw2, fh2);
    Vec3 tip_s = world_to_screen(vec3_add(drag_origin, vec3_scale(axis_world, axis_len)), vp, fw2, fh2);
    float sdx = tip_s.x - center_s.x;
    float sdy = tip_s.y - center_s.y;
    float slen = sqrtf(sdx * sdx + sdy * sdy);
    if (slen > 0.001f) {
        e->gizmo_screen_axis = VEC3(sdx / slen, sdy / slen, 0);
        e->gizmo_world_per_pixel = axis_len / slen;
    } else {
        e->gizmo_screen_axis = VEC3(1.0f, 0.0f, 0.0f);
        e->gizmo_world_per_pixel = 0.01f;
    }
}

static int build_capsule_edit_handles(Mat4 world, float radius, float half_height,
                                      capsule_handle_info out_handles[6]) {
    Vec3 axes[3];
    float scales[3];
    float rr = radius > 0.05f ? radius : 0.3f;
    float hh = half_height > 0.0f ? half_height : rr;
    float top_y = hh + rr;
    editor_world_basis_axes(world, axes, scales);

    out_handles[0].id = GIZMO_CAPSULE_RADIUS_POS_X;
    out_handles[0].position = editor_transform_point(world, VEC3(rr, 0.0f, 0.0f));
    out_handles[0].axis_world = axes[0];
    out_handles[0].axis_local_scale = scales[0];

    out_handles[1].id = GIZMO_CAPSULE_RADIUS_NEG_X;
    out_handles[1].position = editor_transform_point(world, VEC3(-rr, 0.0f, 0.0f));
    out_handles[1].axis_world = vec3_scale(axes[0], -1.0f);
    out_handles[1].axis_local_scale = scales[0];

    out_handles[2].id = GIZMO_CAPSULE_RADIUS_POS_Z;
    out_handles[2].position = editor_transform_point(world, VEC3(0.0f, 0.0f, rr));
    out_handles[2].axis_world = axes[2];
    out_handles[2].axis_local_scale = scales[2];

    out_handles[3].id = GIZMO_CAPSULE_RADIUS_NEG_Z;
    out_handles[3].position = editor_transform_point(world, VEC3(0.0f, 0.0f, -rr));
    out_handles[3].axis_world = vec3_scale(axes[2], -1.0f);
    out_handles[3].axis_local_scale = scales[2];

    out_handles[4].id = GIZMO_CAPSULE_HEIGHT_TOP;
    out_handles[4].position = editor_transform_point(world, VEC3(0.0f, top_y, 0.0f));
    out_handles[4].axis_world = axes[1];
    out_handles[4].axis_local_scale = scales[1];

    out_handles[5].id = GIZMO_CAPSULE_HEIGHT_BOTTOM;
    out_handles[5].position = editor_transform_point(world, VEC3(0.0f, -top_y, 0.0f));
    out_handles[5].axis_world = vec3_scale(axes[1], -1.0f);
    out_handles[5].axis_local_scale = scales[1];

    return 6;
}

static void draw_capsule_handle_cross(editor_state *e, Vec3 center, Vec3 axis_a, Vec3 axis_b,
                                      float half_size, float r, float g, float b) {
    add_line(e,
             vec3_sub(center, vec3_scale(axis_a, half_size)),
             vec3_add(center, vec3_scale(axis_a, half_size)),
             r, g, b);
    add_line(e,
             vec3_sub(center, vec3_scale(axis_b, half_size)),
             vec3_add(center, vec3_scale(axis_b, half_size)),
             r, g, b);
}

static void draw_selected_entity_capsule_handles(editor_state *e, Mat4 world, float radius, float half_height) {
    capsule_handle_info handles[6];
    Vec3 axes[3];
    int i;
    float base_size;
    editor_world_basis_axes(world, axes, NULL);
    build_capsule_edit_handles(world, radius, half_height, handles);

    base_size = editor_handle_draw_size(e, VEC3(world.m[12], world.m[13], world.m[14]));

    for (i = 0; i < 6; i++) {
        float r = 0.35f, g = 0.92f, b = 1.0f;
        int id = handles[i].id;
        if (e->gizmo_hovered == id || e->gizmo_active == id) {
            r = 1.0f; g = 0.95f; b = 0.25f;
        }

        if (id == GIZMO_CAPSULE_HEIGHT_TOP || id == GIZMO_CAPSULE_HEIGHT_BOTTOM) {
            draw_capsule_handle_cross(e, handles[i].position, axes[0], axes[2], base_size, r, g, b);
        } else {
            draw_capsule_handle_cross(e, handles[i].position, axes[1], handles[i].axis_world, base_size, r, g, b);
        }
    }
}

static Quat editor_quat_from_y_deg(float degrees) {
    float half = (degrees * 3.14159265f / 180.0f) * 0.5f;
    return QUAT(0.0f, sinf(half), 0.0f, cosf(half));
}

static Vec3 editor_normalize_scale(Vec3 s) {
    if (s.x == 0.0f) s.x = 1.0f;
    if (s.y == 0.0f) s.y = 1.0f;
    if (s.z == 0.0f) s.z = 1.0f;
    return s;
}

static Mat4 editor_local_matrix_for_entity(const game_state *gs, int entity_index) {
    Vec3 position = VEC3(0.0f, 0.0f, 0.0f);
    float rotation_y = 0.0f;
    Vec3 scale = VEC3(1.0f, 1.0f, 1.0f);
    has_transform_component(gs, entity_index, &position);
    has_rotation_component(gs, entity_index, &rotation_y);
    if (has_scale_component(gs, entity_index, &scale)) {
        scale = editor_normalize_scale(scale);
    }
    return mat4_from_trs(position, editor_quat_from_y_deg(rotation_y), scale);
}

static Mat4 editor_world_matrix_for_entity(const game_state *gs, int entity_index) {
    int chain[SCENE_TREE_MAX_ENTITIES];
    int chain_count = 0;
    int current = entity_index;
    int guard = 0;
    Mat4 world = mat4_identity();

    if (!gs || !gs->scene_entities) return world;
    if (entity_index < 0 || entity_index >= gs->scene_entity_count) return world;

    while (guard < SCENE_TREE_MAX_ENTITIES &&
           current >= 0 && current < gs->scene_entity_count) {
        int parent = -1;
        chain[chain_count++] = current;
        if (!has_parent_transform_component(gs, current, &parent)) break;
        if (parent == current) break;
        current = parent;
        guard++;
    }

    while (chain_count > 0) {
        int idx = chain[--chain_count];
        world = mat4_mul(world, editor_local_matrix_for_entity(gs, idx));
    }

    return world;
}

static Vec3 editor_transform_point(Mat4 m, Vec3 p) {
    return VEC3(
        m.m[0] * p.x + m.m[4] * p.y + m.m[8]  * p.z + m.m[12],
        m.m[1] * p.x + m.m[5] * p.y + m.m[9]  * p.z + m.m[13],
        m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z + m.m[14]
    );
}

static float editor_gizmo_axis_len(const editor_state *e, Vec3 origin) {
    float len;
    if (!e) return 0.15f;
    if (e->cam_projection_mode == EDITOR_CAMERA_ORTHOGRAPHIC) {
        len = editor_ortho_size(e) * 0.08f;
    } else {
        float dist = vec3_len(vec3_sub(origin, e->cam_pos));
        len = dist * 0.08f;
    }
    if (len < 0.15f) len = 0.15f;
    return len;
}

static float editor_handle_draw_size(const editor_state *e, Vec3 origin) {
    float size;
    if (!e) return 0.04f;
    if (e->cam_projection_mode == EDITOR_CAMERA_ORTHOGRAPHIC) {
        size = editor_ortho_size(e) * 0.012f;
    } else {
        float dist = vec3_len(vec3_sub(origin, e->cam_pos));
        size = dist * 0.012f;
    }
    if (size < 0.04f) size = 0.04f;
    if (size > 0.18f) size = 0.18f;
    return size;
}

static Vec3 editor_camera_focus_point(const game_state *gs, const editor_state *e) {
    int selected;
    Mat4 world;
    if (!gs || !e || !gs->scene_entities) return VEC3(0.0f, 0.0f, 0.0f);
    selected = e->scene_selected_entity;
    if (selected < 0 || selected >= gs->scene_entity_count) return VEC3(0.0f, 0.0f, 0.0f);
    if (!has_transform_component(gs, selected, NULL)) return VEC3(0.0f, 0.0f, 0.0f);
    world = editor_world_matrix_for_entity(gs, selected);
    return VEC3(world.m[12], world.m[13], world.m[14]);
}

static void editor_snap_camera_to_preset(const game_state *gs, editor_state *e, int preset) {
    Vec3 focus;
    Vec3 forward;
    float yaw = e ? e->cam_yaw : 0.0f;
    float pitch = e ? e->cam_pitch : 0.0f;
    float dist;

    if (!e) return;

    focus = editor_camera_focus_point(gs, e);
    dist = vec3_len(vec3_sub(e->cam_pos, focus));
    if (dist < 2.0f) dist = 8.0f;

    if (preset == EDITOR_VIEW_PRESET_FRONT) {
        yaw = 3.14159265f;
        pitch = 0.0f;
    } else if (preset == EDITOR_VIEW_PRESET_SIDE) {
        yaw = -1.57079633f;
        pitch = 0.0f;
    } else {
        yaw = 3.14159265f;
        pitch = -1.55f;
    }

    e->cam_yaw = yaw;
    e->cam_pitch = pitch;
    forward = cam_forward(e);
    e->cam_pos = vec3_sub(focus, vec3_scale(forward, dist));
}

static void editor_set_play_mode(game_state *gs, editor_state *e, int enabled) {
    int i;
    int play_mode = enabled ? 1 : 0;
    if (!gs) return;
    if (gs->editor_play_mode == play_mode) return;

    gs->editor_play_mode = play_mode;
    gs->input.horizontal = 0.0f;
    gs->input.vertical = 0.0f;
    gs->input.input_mask = 0;

    if (!play_mode) {
        if (e && e->cam_mouse_look) {
            editor_end_mouse_look(e);
        }
        for (i = 0; i < gs->animation_component_count; i++) {
            gs->animation_components[i].playing = 0;
        }
        return;
    }

    gs->mesh3d.anim_time = 0.0f;
    for (i = 0; i < gs->animation_component_count; i++) {
        animation_component *ac = &gs->animation_components[i];
        ac->playing = 1;
        ac->anim_time = 0.0f;
        if (ac->speed <= 0.0f) ac->speed = 1.0f;
    }
}

static void editor_apply_toolbar_action(game_state *gs, editor_state *e, int action) {
    if (!e) return;

    switch (action) {
    case EDITOR_TOOLBAR_ACTION_PERSPECTIVE:
        e->cam_projection_mode = EDITOR_CAMERA_PERSPECTIVE;
        break;
    case EDITOR_TOOLBAR_ACTION_ORTHOGRAPHIC:
        if (e->cam_projection_mode != EDITOR_CAMERA_ORTHOGRAPHIC) {
            Vec3 focus = editor_camera_focus_point(gs, e);
            float dist = vec3_len(vec3_sub(e->cam_pos, focus));
            if (dist < 2.0f) dist = 2.0f;
            e->cam_ortho_size = fmaxf(2.0f, fminf(80.0f, dist * 1.2f));
        }
        e->cam_projection_mode = EDITOR_CAMERA_ORTHOGRAPHIC;
        break;
    case EDITOR_TOOLBAR_ACTION_FRONT:
        editor_snap_camera_to_preset(gs, e, EDITOR_VIEW_PRESET_FRONT);
        break;
    case EDITOR_TOOLBAR_ACTION_SIDE:
        editor_snap_camera_to_preset(gs, e, EDITOR_VIEW_PRESET_SIDE);
        break;
    case EDITOR_TOOLBAR_ACTION_TOP:
        editor_snap_camera_to_preset(gs, e, EDITOR_VIEW_PRESET_TOP);
        break;
    case EDITOR_TOOLBAR_ACTION_PLAY_MODE:
        editor_set_play_mode(gs, e, !editor_is_play_mode(gs));
        break;
    default:
        break;
    }
}

static void draw_selected_entity_bounds(editor_state *e, Mat4 world, float hx, float hy, float hz) {
    Vec3 corners[8];
    corners[0] = editor_transform_point(world, VEC3(-hx, -hy, -hz));
    corners[1] = editor_transform_point(world, VEC3( hx, -hy, -hz));
    corners[2] = editor_transform_point(world, VEC3( hx, -hy,  hz));
    corners[3] = editor_transform_point(world, VEC3(-hx, -hy,  hz));
    corners[4] = editor_transform_point(world, VEC3(-hx,  hy, -hz));
    corners[5] = editor_transform_point(world, VEC3( hx,  hy, -hz));
    corners[6] = editor_transform_point(world, VEC3( hx,  hy,  hz));
    corners[7] = editor_transform_point(world, VEC3(-hx,  hy,  hz));

    add_line(e, corners[0], corners[1], 0.95f, 0.82f, 0.25f);
    add_line(e, corners[1], corners[2], 0.95f, 0.82f, 0.25f);
    add_line(e, corners[2], corners[3], 0.95f, 0.82f, 0.25f);
    add_line(e, corners[3], corners[0], 0.95f, 0.82f, 0.25f);

    add_line(e, corners[4], corners[5], 0.95f, 0.82f, 0.25f);
    add_line(e, corners[5], corners[6], 0.95f, 0.82f, 0.25f);
    add_line(e, corners[6], corners[7], 0.95f, 0.82f, 0.25f);
    add_line(e, corners[7], corners[4], 0.95f, 0.82f, 0.25f);

    add_line(e, corners[0], corners[4], 0.95f, 0.82f, 0.25f);
    add_line(e, corners[1], corners[5], 0.95f, 0.82f, 0.25f);
    add_line(e, corners[2], corners[6], 0.95f, 0.82f, 0.25f);
    add_line(e, corners[3], corners[7], 0.95f, 0.82f, 0.25f);
}

static void draw_selected_entity_capsule(editor_state *e, Mat4 world, float radius, float half_height) {
    int i;
    const int segments = 20;
    float rr = radius > 0.05f ? radius : 0.3f;
    float hh = half_height > 0.0f ? half_height : rr;
    Vec3 top_center = editor_transform_point(world, VEC3(0.0f, hh, 0.0f));
    Vec3 bottom_center = editor_transform_point(world, VEC3(0.0f, -hh, 0.0f));
    Vec3 prev_top = top_center;
    Vec3 prev_bottom = bottom_center;

    for (i = 0; i <= segments; i++) {
        float t = (float)i / (float)segments;
        float a = t * 2.0f * 3.14159265f;
        Vec3 local_top = VEC3(cosf(a) * rr, hh, sinf(a) * rr);
        Vec3 local_bottom = VEC3(cosf(a) * rr, -hh, sinf(a) * rr);
        Vec3 ptop = editor_transform_point(world, local_top);
        Vec3 pbottom = editor_transform_point(world, local_bottom);
        if (i > 0) {
            add_line(e, prev_top, ptop, 0.95f, 0.82f, 0.25f);
            add_line(e, prev_bottom, pbottom, 0.95f, 0.82f, 0.25f);
        }
        prev_top = ptop;
        prev_bottom = pbottom;
    }

    for (i = 1; i <= segments; i++) {
        float t0 = (float)(i - 1) / (float)segments;
        float t1 = (float)i / (float)segments;
        float a0 = t0 * 3.14159265f;
        float a1 = t1 * 3.14159265f;

        Vec3 top_x0 = editor_transform_point(world, VEC3(cosf(a0) * rr, hh + sinf(a0) * rr, 0.0f));
        Vec3 top_x1 = editor_transform_point(world, VEC3(cosf(a1) * rr, hh + sinf(a1) * rr, 0.0f));
        Vec3 top_z0 = editor_transform_point(world, VEC3(0.0f, hh + sinf(a0) * rr, cosf(a0) * rr));
        Vec3 top_z1 = editor_transform_point(world, VEC3(0.0f, hh + sinf(a1) * rr, cosf(a1) * rr));
        Vec3 bot_x0 = editor_transform_point(world, VEC3(cosf(a0) * rr, -hh - sinf(a0) * rr, 0.0f));
        Vec3 bot_x1 = editor_transform_point(world, VEC3(cosf(a1) * rr, -hh - sinf(a1) * rr, 0.0f));
        Vec3 bot_z0 = editor_transform_point(world, VEC3(0.0f, -hh - sinf(a0) * rr, cosf(a0) * rr));
        Vec3 bot_z1 = editor_transform_point(world, VEC3(0.0f, -hh - sinf(a1) * rr, cosf(a1) * rr));

        add_line(e, top_x0, top_x1, 0.95f, 0.82f, 0.25f);
        add_line(e, top_z0, top_z1, 0.95f, 0.82f, 0.25f);
        add_line(e, bot_x0, bot_x1, 0.95f, 0.82f, 0.25f);
        add_line(e, bot_z0, bot_z1, 0.95f, 0.82f, 0.25f);
    }

    add_line(e, editor_transform_point(world, VEC3(rr, hh, 0.0f)),
             editor_transform_point(world, VEC3(rr, -hh, 0.0f)), 0.95f, 0.82f, 0.25f);
    add_line(e, editor_transform_point(world, VEC3(-rr, hh, 0.0f)),
             editor_transform_point(world, VEC3(-rr, -hh, 0.0f)), 0.95f, 0.82f, 0.25f);
    add_line(e, editor_transform_point(world, VEC3(0.0f, hh, rr)),
             editor_transform_point(world, VEC3(0.0f, -hh, rr)), 0.95f, 0.82f, 0.25f);
    add_line(e, editor_transform_point(world, VEC3(0.0f, hh, -rr)),
             editor_transform_point(world, VEC3(0.0f, -hh, -rr)), 0.95f, 0.82f, 0.25f);
}

static void draw_selected_entity_gizmo(editor_state *e, Mat4 world) {
    Vec3 origin = VEC3(world.m[12], world.m[13], world.m[14]);
    Vec3 x_axis = vec3_normalize(VEC3(world.m[0], world.m[1], world.m[2]));
    Vec3 y_axis = vec3_normalize(VEC3(world.m[4], world.m[5], world.m[6]));
    Vec3 z_axis = vec3_normalize(VEC3(world.m[8], world.m[9], world.m[10]));
    float len = editor_gizmo_axis_len(e, origin);

    if (vec3_len(x_axis) < 0.001f) x_axis = VEC3(1.0f, 0.0f, 0.0f);
    if (vec3_len(y_axis) < 0.001f) y_axis = VEC3(0.0f, 1.0f, 0.0f);
    if (vec3_len(z_axis) < 0.001f) z_axis = VEC3(0.0f, 0.0f, 1.0f);

    add_line(e, origin, vec3_add(origin, vec3_scale(x_axis, len)), 1.0f, 0.2f, 0.2f);
    add_line(e, origin, vec3_add(origin, vec3_scale(y_axis, len)), 0.2f, 1.0f, 0.2f);
    add_line(e, origin, vec3_add(origin, vec3_scale(z_axis, len)), 0.2f, 0.4f, 1.0f);
}

static int selected_entity_gizmo_basis(const game_state *gs, const editor_state *e,
                                       Vec3 *out_origin, Vec3 axes[3], float *out_len) {
    int selected;
    Mat4 world;
    if (!gs || !e || !gs->scene_entities) return 0;
    selected = e->scene_selected_entity;
    if (selected < 0 || selected >= gs->scene_entity_count) return 0;
    if (!has_transform_component(gs, selected, NULL)) return 0;

    world = editor_world_matrix_for_entity(gs, selected);
    *out_origin = VEC3(world.m[12], world.m[13], world.m[14]);
    axes[0] = vec3_normalize(VEC3(world.m[0], world.m[1], world.m[2]));
    axes[1] = vec3_normalize(VEC3(world.m[4], world.m[5], world.m[6]));
    axes[2] = vec3_normalize(VEC3(world.m[8], world.m[9], world.m[10]));
    if (vec3_len(axes[0]) < 0.001f) axes[0] = VEC3(1.0f, 0.0f, 0.0f);
    if (vec3_len(axes[1]) < 0.001f) axes[1] = VEC3(0.0f, 1.0f, 0.0f);
    if (vec3_len(axes[2]) < 0.001f) axes[2] = VEC3(0.0f, 0.0f, 1.0f);
    *out_len = editor_gizmo_axis_len(e, *out_origin);
    return 1;
}

/* ── Line building ─────────────────────────────────────────────── */

static void build_lines(game_state *gs, editor_state *es) {
    editor_state *e = es;
    float gc = 0.3f;
    int i;
    int selected = -1;

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

    selected = e->scene_selected_entity;
    if (gs->scene_entities &&
        selected >= 0 && selected < gs->scene_entity_count) {
        Vec3 tpos = VEC3(0.0f, 0.0f, 0.0f);
        Vec3 tscale = VEC3(1.0f, 1.0f, 1.0f);
        rect crect = {0.0f, 0.0f, 0.0f, 0.0f};
        int collider_is_capsule = 0;
        float capsule_radius = 0.0f;
        float capsule_half_height = 0.0f;
        int mesh_visible = 0;
        int has_transform = has_transform_component(gs, selected, &tpos);
        int has_rotation = has_rotation_component(gs, selected, NULL);
        int has_scale = has_scale_component(gs, selected, &tscale);
        int has_collider = has_any_collider_component(gs, selected, &crect,
                                                      &collider_is_capsule,
                                                      &capsule_radius,
                                                      &capsule_half_height);
        int has_mesh = has_mesh_component(gs, selected, &mesh_visible);
        int has_any_visual = has_transform || has_rotation || has_scale || has_collider || has_mesh;

        if (has_any_visual) {
            Mat4 world = editor_world_matrix_for_entity(gs, selected);
            float hx = 0.5f;
            float hy = 0.5f;
            float hz = 0.5f;

            if (has_scale) {
                hx = fmaxf(0.05f, hx * fabsf(tscale.x));
                hy = fmaxf(0.05f, hy * fabsf(tscale.y));
                hz = fmaxf(0.05f, hz * fabsf(tscale.z));
            }
            if (has_collider) {
                float chx = fmaxf(0.05f, fabsf(crect.w) * 0.5f);
                float chz = fmaxf(0.05f, fabsf(crect.h) * 0.5f);
                hx = fmaxf(hx, chx);
                hz = fmaxf(hz, chz);
                hy = fmaxf(hy, fmaxf(0.2f, fmaxf(chx, chz)));
            }

            if (has_collider && collider_is_capsule) {
                draw_selected_entity_capsule(e, world, capsule_radius, capsule_half_height);
                draw_selected_entity_capsule_handles(e, world, capsule_radius, capsule_half_height);
            } else {
                draw_selected_entity_bounds(e, world, hx, hy, hz);
            }

            if (has_transform || has_rotation || has_scale) {
                draw_selected_entity_gizmo(e, world);
            }
        }
    }

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
    float fw2, fh2, mx, my;
    Mat4 ed_proj, ed_view, vp;
    Vec3 ed_fwd, giz, mouse, center_s;
    float giz_len, best_dist;
    Vec3 axis_dirs[3];
    int selected;

    if (!e->open || !e->window) {
        e->gizmo_hovered = GIZMO_NONE;
        return;
    }
    if (e->gizmo_active != GIZMO_NONE) return;

    mouse_win = SDL_GetMouseFocus();
    if (mouse_win != (SDL_Window *)e->window) {
        e->gizmo_hovered = GIZMO_NONE;
        return;
    }

    /* Use panel dimensions instead of full window size */
    fw2 = e->panel_w; fh2 = e->panel_h;
    if (fw2 < 1 || fh2 < 1) return;

    ed_proj = editor_projection_matrix(e, fw2, fh2);
    ed_fwd  = cam_forward(e);
    ed_view = mat4_look_at(e->cam_pos, vec3_add(e->cam_pos, ed_fwd), VEC3(0, 1, 0));
    vp = mat4_mul(ed_proj, ed_view);

    /* Transform mouse from window-space to panel-local space */
    SDL_GetMouseState(&mx, &my);
    mx -= e->panel_x;
    my -= e->panel_y;
    if (editor_toolbar_contains_point(e, mx, my)) {
        e->gizmo_hovered = GIZMO_NONE;
        return;
    }
    mouse = VEC3(mx, my, 0);

    if (!selected_entity_gizmo_basis(gs, e, &giz, axis_dirs, &giz_len)) {
        e->gizmo_hovered = GIZMO_NONE;
        return;
    }

    center_s = world_to_screen(giz, vp, fw2, fh2);
    selected = e->scene_selected_entity;

    e->gizmo_hovered = GIZMO_NONE;
    best_dist = 14.0f * 14.0f;

    for (i = 0; i < 3; i++) {
        Vec3 tip_s = world_to_screen(vec3_add(giz, vec3_scale(axis_dirs[i], giz_len)), vp, fw2, fh2);
        float d = pt_seg_dist_sq(mouse, center_s, tip_s);
        if (d < best_dist) {
            best_dist = d;
            e->gizmo_hovered = i + 1;
        }
    }

    if (selected >= 0 && selected < gs->scene_entity_count) {
        float capsule_radius = 0.0f;
        float capsule_half_height = 0.0f;
        if (has_capsule_collider_component(gs, selected, &capsule_radius, &capsule_half_height, NULL)) {
            Mat4 world = editor_world_matrix_for_entity(gs, selected);
            capsule_handle_info handles[6];
            int handle_count = build_capsule_edit_handles(world, capsule_radius, capsule_half_height, handles);
            for (i = 0; i < handle_count; i++) {
                Vec3 hs = world_to_screen(handles[i].position, vp, fw2, fh2);
                float dx = mouse.x - hs.x;
                float dy = mouse.y - hs.y;
                float d = dx * dx + dy * dy;
                if (d < best_dist) {
                    best_dist = d;
                    e->gizmo_hovered = handles[i].id;
                }
            }
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
    e->cam_projection_mode = EDITOR_CAMERA_PERSPECTIVE;
    e->cam_ortho_size = 12.0f;
    e->cam_mouse_look = 0;
    e->cam_mouse_button = 0;
    e->open = 1;
    e->gizmo_hovered = GIZMO_NONE;
    e->gizmo_active  = GIZMO_NONE;
    e->gizmo_entity_index = -1;
    e->gizmo_drag_mode = 0;
    e->gizmo_drag_start_capsule_radius = 0.0f;
    e->gizmo_drag_start_capsule_half_height = 0.0f;
    e->gizmo_drag_axis_local_scale = 1.0f;
    e->line_count = 0;
    e->menu_open = -1;
    e->menu_hover = -1;
    e->scene_selected_entity = 0;
    e->scene_tree_drag_active = 0;
    e->scene_tree_drag_entity = -1;
    e->scene_tree_drop_target = -1;
    e->scene_tree_drop_mode = SCENE_TREE_DROP_NONE;
    e->project_browser_mouse_x = -10000.0f;
    e->project_browser_mouse_y = -10000.0f;
    e->project_browser_scroll_y = 0.0f;
    e->project_browser_mouse_down = 0;
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

    if (!e->scene_tree_clay_ctx && es->editor_arena) {
        uint32_t clay_size = (uint32_t)Clay_MinMemorySize();
        arena *clay_sub = arena_alloc_subarena(es->editor_arena, clay_size, 16, "clay_scene_tree");
        if (clay_sub) {
            Clay_Arena ca = Clay_CreateArenaWithCapacityAndMemory(clay_size, clay_sub->base);
            Clay_ErrorHandler err = {0};
            e->scene_tree_clay_ctx = Clay_Initialize(ca, (Clay_Dimensions){800, 600}, err);
            Clay_SetCurrentContext((Clay_Context *)e->scene_tree_clay_ctx);
            Clay_SetMeasureTextFunction(profiler_measure_text, e);
        }
    }

    if (!e->inspector_clay_ctx && es->editor_arena) {
        uint32_t clay_size = (uint32_t)Clay_MinMemorySize();
        arena *clay_sub = arena_alloc_subarena(es->editor_arena, clay_size, 16, "clay_inspector");
        if (clay_sub) {
            Clay_Arena ca = Clay_CreateArenaWithCapacityAndMemory(clay_size, clay_sub->base);
            Clay_ErrorHandler err = {0};
            e->inspector_clay_ctx = Clay_Initialize(ca, (Clay_Dimensions){420, 700}, err);
            Clay_SetCurrentContext((Clay_Context *)e->inspector_clay_ctx);
            Clay_SetMeasureTextFunction(profiler_measure_text, e);
        }
    }

    if (!e->project_browser_clay_ctx && es->editor_arena) {
        uint32_t clay_size = (uint32_t)Clay_MinMemorySize();
        arena *clay_sub = arena_alloc_subarena(es->editor_arena, clay_size, 16, "clay_project_browser");
        if (clay_sub) {
            Clay_Arena ca = Clay_CreateArenaWithCapacityAndMemory(clay_size, clay_sub->base);
            Clay_ErrorHandler err = {0};
            e->project_browser_clay_ctx = Clay_Initialize(ca, (Clay_Dimensions){520, 720}, err);
            Clay_SetCurrentContext((Clay_Context *)e->project_browser_clay_ctx);
            Clay_SetMeasureTextFunction(profiler_measure_text, e);
        }
    }

    if (!e->menu_bar_clay_ctx && es->editor_arena) {
        uint32_t clay_size = (uint32_t)Clay_MinMemorySize();
        arena *clay_sub = arena_alloc_subarena(es->editor_arena, clay_size, 16, "clay_menu_bar");
        if (clay_sub) {
            Clay_Arena ca = Clay_CreateArenaWithCapacityAndMemory(clay_size, clay_sub->base);
            Clay_ErrorHandler err = {0};
            e->menu_bar_clay_ctx = Clay_Initialize(ca, (Clay_Dimensions){1600, MENU_BAR_HEIGHT}, err);
            Clay_SetCurrentContext((Clay_Context *)e->menu_bar_clay_ctx);
            Clay_SetMeasureTextFunction(profiler_measure_text, e);
        }
    }

    if (!e->editor_toolbar_clay_ctx && es->editor_arena) {
        uint32_t clay_size = (uint32_t)Clay_MinMemorySize();
        arena *clay_sub = arena_alloc_subarena(es->editor_arena, clay_size, 16, "clay_editor_toolbar");
        if (clay_sub) {
            Clay_Arena ca = Clay_CreateArenaWithCapacityAndMemory(clay_size, clay_sub->base);
            Clay_ErrorHandler err = {0};
            e->editor_toolbar_clay_ctx = Clay_Initialize(ca, (Clay_Dimensions){800, 200}, err);
            Clay_SetCurrentContext((Clay_Context *)e->editor_toolbar_clay_ctx);
            Clay_SetMeasureTextFunction(profiler_measure_text, e);
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
    static FlatRecord flat[PROFILER_MAX_FLAT_RECORDS];
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

    /* Flatten records for grid */
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

        /* Two-panel row */
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
                    CLAY(CLAY_ID("PTreeContent"), {
                        .layout = {
                            .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIT({0}) },
                            .childGap = 2,
                            .layoutDirection = CLAY_TOP_TO_BOTTOM
                        }
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
                                                &tree_color, click, e->profiler_tree_collapsed);
                            hover_offset = hi.offset;
                            hover_size = hi.size;
                            have_hover = hi.found;
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

static const char *project_browser_path_basename(const char *path) {
    const char *slash;
    const char *backslash;
    const char *last;
    if (!path || !path[0]) return "";
    slash = strrchr(path, '/');
    backslash = strrchr(path, '\\');
    last = slash;
    if (backslash && (!last || backslash > last)) last = backslash;
    return last ? (last + 1) : path;
}

static void project_browser_emit_asset_row(int row_id,
                                           const char *key,
                                           const char *path,
                                           Clay_Color swatch) {
    const char *display_key = (key && key[0]) ? key : "(unnamed)";
    const char *display_path = (path && path[0]) ? path : "(missing path)";
    const char *file_name = project_browser_path_basename(display_path);
    Clay_String text;

    CLAY(CLAY_IDI("PBAssetRow", row_id), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIT({0}) },
            .padding = CLAY_PADDING_ALL(8),
            .childGap = 10,
            .childAlignment = { CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER },
            .layoutDirection = CLAY_LEFT_TO_RIGHT
        },
        .backgroundColor = Clay_Hovered() ? ((Clay_Color){52, 60, 78, 255})
                                          : ((Clay_Color){40, 48, 64, 255}),
        .cornerRadius = CLAY_CORNER_RADIUS(4)
    }) {
        CLAY(CLAY_IDI("PBAssetSwatch", row_id), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(38), CLAY_SIZING_FIXED(38) }
            },
            .backgroundColor = swatch,
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {}

        CLAY(CLAY_IDI("PBAssetMeta", row_id), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIT({0}) },
                .childGap = 2,
                .layoutDirection = CLAY_TOP_TO_BOTTOM
            }
        }) {
            text = (Clay_String){false, (int32_t)strlen(display_key), display_key};
            CLAY_TEXT(text, CLAY_TEXT_CONFIG({.textColor = {228, 234, 242, 255}, .fontSize = 16}));

            text = (Clay_String){false, (int32_t)strlen(file_name), file_name};
            CLAY_TEXT(text, CLAY_TEXT_CONFIG({.textColor = {190, 200, 220, 255}, .fontSize = 15}));

            text = (Clay_String){false, (int32_t)strlen(display_path), display_path};
            CLAY_TEXT(text, CLAY_TEXT_CONFIG({.textColor = {150, 162, 186, 255}, .fontSize = 14}));
        }
    }
}

static void project_browser_emit_asset_group(const char *title,
                                             const char (*keys)[64],
                                             const char (*paths)[256],
                                             int count,
                                             Clay_Color swatch,
                                             int *row_id) {
    int i;
    int group_id;
    Clay_String header = { false, (int32_t)strlen(title), title };
    if (!row_id) return;
    group_id = *row_id;
    (*row_id)++;

    CLAY(CLAY_IDI("PBGroup", group_id), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIT({0}) },
            .childGap = 6,
            .layoutDirection = CLAY_TOP_TO_BOTTOM
        }
    }) {
        CLAY_TEXT(header, CLAY_TEXT_CONFIG({.textColor = {182, 208, 244, 255}, .fontSize = 16}));

        if (count <= 0) {
            Clay_String empty = CLAY_STRING("(none)");
            CLAY_TEXT(empty, CLAY_TEXT_CONFIG({.textColor = {130, 142, 166, 255}, .fontSize = 15}));
        } else {
            for (i = 0; i < count; i++) {
                project_browser_emit_asset_row(*row_id, keys[i], paths[i], swatch);
                (*row_id)++;
            }
        }
    }
}

static void scene_tree_layout(game_state *gs, editor_state *es) {
    editor_state *e = es;
    dock_state *d = (dock_state *)e->dock;
    Clay_Context *ctx = (Clay_Context *)e->scene_tree_clay_ctx;
    int scene_win_idx;
    int scene_node;
    int win_w, win_h;
    int click;
    int entity_count;
    int parent_of[SCENE_TREE_MAX_ENTITIES];
    int visited[SCENE_TREE_MAX_ENTITIES];
    static char title_buf[128];
    Clay_RenderCommandArray commands;

    if (!ctx) {
        e->scene_tree_cmd_count = 0;
        e->scene_tree_cmd_array = NULL;
        return;
    }

    Clay_SetCurrentContext(ctx);

    scene_node = dock_find_leaf_for_panel_global(d, PANEL_SCENE_TREE, &scene_win_idx);
    if (scene_node >= 0) {
        DockNode *sn = &d->nodes[scene_node];
        win_w = (int)sn->w;
        win_h = (int)(sn->h - DOCK_HEADER_HEIGHT);
        if (win_h < 1) win_h = 1;
    } else {
        win_w = 600;
        win_h = 700;
    }
    Clay_SetLayoutDimensions((Clay_Dimensions){(float)win_w, (float)win_h});

    click = e->scene_tree_click;
    if (e->scene_tree_drag_active && e->scene_tree_mouse_down) {
        e->scene_tree_drop_target = -1;
        e->scene_tree_drop_mode = SCENE_TREE_DROP_NONE;
    }
    {
        Clay_Vector2 mpos = {e->scene_tree_mouse_x, e->scene_tree_mouse_y};
        Clay_Vector2 sdelta = {0, e->scene_tree_scroll_y};
        Clay_SetPointerState(mpos, (bool)e->scene_tree_mouse_down);
        Clay_UpdateScrollContainers(true, sdelta, gs->dt);
        e->scene_tree_scroll_y = 0;
    }

    Clay_BeginLayout();

    entity_count = gs->scene_entities ? gs->scene_entity_count : 0;
    snprintf(title_buf, sizeof(title_buf), "Scene Tree  (%d entities)", entity_count);
    if (entity_count > SCENE_TREE_MAX_ENTITIES) entity_count = SCENE_TREE_MAX_ENTITIES;
    build_scene_parent_lookup(gs, entity_count, parent_of);
    memset(visited, 0, (size_t)entity_count * sizeof(int));

    CLAY(CLAY_ID("STRoot"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_GROW({0}) },
            .padding = CLAY_PADDING_ALL(12),
            .childGap = 8,
            .layoutDirection = CLAY_TOP_TO_BOTTOM
        },
        .backgroundColor = {24, 28, 36, 255}
    }) {
        Clay_String ts = {false, (int32_t)strlen(title_buf), title_buf};
        CLAY_TEXT(ts, CLAY_TEXT_CONFIG({.textColor = {220, 225, 235, 255}, .fontSize = 16}));

        CLAY(CLAY_ID("STList"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_GROW({0}) },
                .childGap = 8,
                .layoutDirection = CLAY_TOP_TO_BOTTOM
            },
            .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() }
        }) {
            CLAY(CLAY_ID("STListContent"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIT({0}) },
                    .childGap = 8,
                    .layoutDirection = CLAY_TOP_TO_BOTTOM
                }
            }) {
                int i;
                if (entity_count <= 0) {
                    Clay_String cs = CLAY_STRING("(empty)");
                    CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {150, 160, 180, 255}, .fontSize = 16}));
                } else {
                    for (i = 0; i < entity_count; i++) {
                        int p = parent_of[i];
                        int is_root = (p < 0 || p >= entity_count || p == i);
                        if (is_root) {
                            scene_tree_emit_entity_recursive(gs, e, i, 0, click,
                                                             parent_of, entity_count, visited);
                        }
                    }
                    for (i = 0; i < entity_count; i++) {
                        if (!visited[i]) {
                            scene_tree_emit_entity_recursive(gs, e, i, 0, click,
                                                             parent_of, entity_count, visited);
                        }
                    }
                }
            }
        }
    }

    commands = Clay_EndLayout();
    e->scene_tree_cmd_count = commands.length;
    e->scene_tree_cmd_array = commands.internalArray;

    if (e->scene_tree_drag_active && !e->scene_tree_mouse_down) {
        int child = e->scene_tree_drag_entity;
        int target = e->scene_tree_drop_target;
        int mode = e->scene_tree_drop_mode;
        int parent = -2;

        if (mode == SCENE_TREE_DROP_NESTED && target >= 0 && target < entity_count) {
            parent = target;
        } else if ((mode == SCENE_TREE_DROP_SIBLING_BEFORE || mode == SCENE_TREE_DROP_SIBLING_AFTER) &&
                   target >= 0 && target < entity_count) {
            parent = parent_of[target];
            if (parent < 0 || parent >= entity_count) parent = -1;
        }

        if (parent >= -1 && scene_tree_parent_assignment_is_valid(child, parent, parent_of, entity_count)) {
            set_parent_transform_component(gs, child, parent);
            e->scene_selected_entity = child;
        }
        e->scene_tree_drag_active = 0;
        e->scene_tree_drag_entity = -1;
        e->scene_tree_drop_target = -1;
        e->scene_tree_drop_mode = SCENE_TREE_DROP_NONE;
    }

    e->scene_tree_click = 0;
}

static void project_browser_layout(game_state *gs, editor_state *es) {
    editor_state *e = es;
    dock_state *d = (dock_state *)e->dock;
    Clay_Context *ctx = (Clay_Context *)e->project_browser_clay_ctx;
    int browser_win_idx;
    int browser_node;
    int win_w, win_h;
    int row_id = 0;
    int total_assets;
    static char title_buf[128];
    Clay_RenderCommandArray commands;

    if (!ctx) {
        e->project_browser_cmd_count = 0;
        e->project_browser_cmd_array = NULL;
        return;
    }

    Clay_SetCurrentContext(ctx);
    browser_node = dock_find_leaf_for_panel_global(d, PANEL_ASSETS, &browser_win_idx);
    if (browser_node < 0) {
        e->project_browser_cmd_count = 0;
        e->project_browser_cmd_array = NULL;
        return;
    }
    {
        DockNode *n = &d->nodes[browser_node];
        win_w = (int)n->w;
        win_h = (int)(n->h - DOCK_HEADER_HEIGHT);
        if (win_h < 1) win_h = 1;
    }
    Clay_SetLayoutDimensions((Clay_Dimensions){(float)win_w, (float)win_h});

    {
        Clay_Vector2 mpos = {e->project_browser_mouse_x, e->project_browser_mouse_y};
        Clay_Vector2 sdelta = {0, e->project_browser_scroll_y};
        Clay_SetPointerState(mpos, (bool)e->project_browser_mouse_down);
        Clay_UpdateScrollContainers(true, sdelta, gs->dt);
        e->project_browser_scroll_y = 0.0f;
    }

    total_assets = gs->project.model_count +
                   gs->project.animation_count +
                   gs->project.dungeon_piece_count +
                   gs->project.sprite_count;
    snprintf(title_buf, sizeof(title_buf), "Project Browser  (%d assets)", total_assets);

    Clay_BeginLayout();
    CLAY(CLAY_ID("PBRoot"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_GROW({0}) },
            .padding = CLAY_PADDING_ALL(12),
            .childGap = 8,
            .layoutDirection = CLAY_TOP_TO_BOTTOM
        },
        .backgroundColor = {24, 28, 36, 255}
    }) {
        Clay_String title = { false, (int32_t)strlen(title_buf), title_buf };
        CLAY_TEXT(title, CLAY_TEXT_CONFIG({.textColor = {220, 226, 236, 255}, .fontSize = 16}));

        CLAY(CLAY_ID("PBList"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_GROW({0}) },
                .childGap = 10,
                .layoutDirection = CLAY_TOP_TO_BOTTOM
            },
            .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() }
        }) {
            CLAY(CLAY_ID("PBListContent"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIT({0}) },
                    .childGap = 12,
                    .layoutDirection = CLAY_TOP_TO_BOTTOM
                }
            }) {
                project_browser_emit_asset_group("Models",
                                                 gs->project.model_keys,
                                                 gs->project.model_paths,
                                                 gs->project.model_count,
                                                 (Clay_Color){94, 138, 216, 255},
                                                 &row_id);
                project_browser_emit_asset_group("Animations",
                                                 gs->project.animation_keys,
                                                 gs->project.animation_paths,
                                                 gs->project.animation_count,
                                                 (Clay_Color){198, 138, 84, 255},
                                                 &row_id);
                project_browser_emit_asset_group("Dungeon Pieces",
                                                 gs->project.dungeon_piece_keys,
                                                 gs->project.dungeon_piece_paths,
                                                 gs->project.dungeon_piece_count,
                                                 (Clay_Color){116, 172, 118, 255},
                                                 &row_id);
                project_browser_emit_asset_group("Sprites",
                                                 gs->project.sprite_keys,
                                                 gs->project.sprite_paths,
                                                 gs->project.sprite_count,
                                                 (Clay_Color){166, 132, 204, 255},
                                                 &row_id);
            }
        }
    }

    commands = Clay_EndLayout();
    e->project_browser_cmd_count = commands.length;
    e->project_browser_cmd_array = commands.internalArray;
}

static void inspector_layout(game_state *gs, editor_state *es) {
    editor_state *e = es;
    dock_state *d = (dock_state *)e->dock;
    Clay_Context *ctx = (Clay_Context *)e->inspector_clay_ctx;
    int insp_win_idx;
    int insp_node;
    int win_w, win_h;
    int selected;
    static char title_buf[96];
    static char line_bufs[32][160];
    Clay_RenderCommandArray commands;

    if (!ctx) {
        e->inspector_cmd_count = 0;
        e->inspector_cmd_array = NULL;
        return;
    }

    Clay_SetCurrentContext(ctx);
    insp_node = dock_find_leaf_for_panel_global(d, PANEL_INSPECTOR, &insp_win_idx);
    if (insp_node < 0) {
        e->inspector_cmd_count = 0;
        e->inspector_cmd_array = NULL;
        return;
    }
    {
        DockNode *n = &d->nodes[insp_node];
        win_w = (int)n->w;
        win_h = (int)(n->h - DOCK_HEADER_HEIGHT);
        if (win_h < 1) win_h = 1;
    }
    Clay_SetLayoutDimensions((Clay_Dimensions){(float)win_w, (float)win_h});

    selected = e->scene_selected_entity;
    if (!gs->scene_entities) selected = -1;
    if (selected < 0 || selected >= gs->scene_entity_count) selected = -1;

    snprintf(title_buf, sizeof(title_buf), "Inspector");
    Clay_BeginLayout();

    CLAY(CLAY_ID("INRoot"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_GROW({0}) },
            .padding = CLAY_PADDING_ALL(12),
            .childGap = 8,
            .layoutDirection = CLAY_TOP_TO_BOTTOM
        },
        .backgroundColor = {24, 28, 36, 255}
    }) {
        Clay_String ts = {false, (int32_t)strlen(title_buf), title_buf};
        CLAY_TEXT(ts, CLAY_TEXT_CONFIG({.textColor = {220, 225, 235, 255}, .fontSize = 16}));

        CLAY(CLAY_ID("INBody"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_GROW({0}) },
                .padding = CLAY_PADDING_ALL(10),
                .childGap = 6,
                .layoutDirection = CLAY_TOP_TO_BOTTOM
            },
            .backgroundColor = {36, 42, 56, 255},
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            if (selected < 0) {
                Clay_String cs = CLAY_STRING("No entity selected.");
                CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {170, 180, 198, 255}, .fontSize = 16}));
            } else {
                int line_i = 0;
                int parent_idx = -1;
                int parent_transform_idx = -1;
                Vec3 tpos = VEC3(0.0f, 0.0f, 0.0f);
                float ry = 0.0f;
                Vec3 tscale = VEC3(1.0f, 1.0f, 1.0f);
                vec2 vvel = {0.0f, 0.0f};
                float hcur = 0.0f, hmax = 0.0f;
                rect crect = {0.0f, 0.0f, 0.0f, 0.0f};
                int collider_is_capsule = 0;
                float capsule_radius = 0.0f;
                float capsule_half_height = 0.0f;
                int mesh_visible = 0;
                int anim_playing = 0;
                int anim_clip = 0;
                float anim_time = 0.0f;
                float anim_speed = 1.0f;
                float cam_fov = 0.0f, cam_near = 0.0f, cam_far = 0.0f;
                Vec3 cam_target = VEC3(0.0f, 0.0f, 0.0f);
                Vec3 cam_up = VEC3(0.0f, 1.0f, 0.0f);
                int has_transform = has_transform_component(gs, selected, &tpos);
                int has_rotation = has_rotation_component(gs, selected, &ry);
                int has_scale = has_scale_component(gs, selected, &tscale);
                int has_velocity = has_velocity_component(gs, selected, &vvel);
                float cc_move_speed = 0.0f;
                float cc_jump_speed = 0.0f;
                int has_character_controller = has_character_controller_component(gs, selected,
                                                                                 &cc_move_speed,
                                                                                 &cc_jump_speed);
                int has_health = has_health_component(gs, selected, &hcur, &hmax);
                int has_collider = has_any_collider_component(gs, selected, &crect,
                                                              &collider_is_capsule,
                                                              &capsule_radius,
                                                              &capsule_half_height);
                int has_mesh = has_mesh_component(gs, selected, &mesh_visible);
                int has_animation = has_animation_component(gs, selected,
                                                            &anim_playing, &anim_clip, &anim_time, &anim_speed);
                int has_camera = has_camera_component(gs, selected,
                                                      &cam_fov, &cam_near, &cam_far,
                                                      &cam_target, &cam_up);
                int has_parent = find_parent_component(gs, selected, &parent_idx);
                int has_parent_transform = has_parent_transform_component(gs, selected, &parent_transform_idx);
                int has_parent_rotation = has_parent_rotation_component(gs, selected);
                int has_components = has_transform || has_rotation || has_scale ||
                                     has_velocity || has_character_controller ||
                                     has_health || has_collider ||
                                     has_mesh || has_animation || has_camera || has_parent ||
                                     has_parent_transform || has_parent_rotation;

                snprintf(line_bufs[line_i], sizeof(line_bufs[line_i]), "Entity %d", selected);
                {
                    Clay_String cs = {false, (int32_t)strlen(line_bufs[line_i]), line_bufs[line_i]};
                    CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {240, 240, 240, 255}, .fontSize = 16}));
                }
                line_i++;

                {
                    Clay_String cs = CLAY_STRING("Components");
                    CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {190, 200, 218, 255}, .fontSize = 16}));
                }
                if (!has_components) {
                    Clay_String cs = CLAY_STRING("- (none)");
                    CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {150, 160, 180, 255}, .fontSize = 16}));
                }

                if (has_transform) {
                    snprintf(line_bufs[line_i], sizeof(line_bufs[line_i]),
                             "- Transform (position=%.2f, %.2f, %.2f)",
                             tpos.x, tpos.y, tpos.z);
                    {
                        Clay_String cs = {false, (int32_t)strlen(line_bufs[line_i]), line_bufs[line_i]};
                        CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {205, 215, 232, 255}, .fontSize = 16}));
                    }
                    line_i++;
                }
                if (has_rotation) {
                    snprintf(line_bufs[line_i], sizeof(line_bufs[line_i]),
                             "- Rotation (y=%.2f deg)", ry);
                    {
                        Clay_String cs = {false, (int32_t)strlen(line_bufs[line_i]), line_bufs[line_i]};
                        CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {205, 215, 232, 255}, .fontSize = 16}));
                    }
                    line_i++;
                }
                if (has_scale) {
                    snprintf(line_bufs[line_i], sizeof(line_bufs[line_i]),
                             "- Scale (%.2f, %.2f, %.2f)",
                             tscale.x, tscale.y, tscale.z);
                    {
                        Clay_String cs = {false, (int32_t)strlen(line_bufs[line_i]), line_bufs[line_i]};
                        CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {205, 215, 232, 255}, .fontSize = 16}));
                    }
                    line_i++;
                }
                if (has_velocity) {
                    snprintf(line_bufs[line_i], sizeof(line_bufs[line_i]),
                             "- Velocity (%.2f, %.2f)", vvel.x, vvel.y);
                    {
                        Clay_String cs = {false, (int32_t)strlen(line_bufs[line_i]), line_bufs[line_i]};
                        CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {205, 215, 232, 255}, .fontSize = 16}));
                    }
                    line_i++;
                }
                if (has_character_controller) {
                    snprintf(line_bufs[line_i], sizeof(line_bufs[line_i]),
                             "- Character Controller (move=%.2f jump=%.2f)",
                             cc_move_speed, cc_jump_speed);
                    {
                        Clay_String cs = {false, (int32_t)strlen(line_bufs[line_i]), line_bufs[line_i]};
                        CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {205, 215, 232, 255}, .fontSize = 16}));
                    }
                    line_i++;
                }
                if (has_health) {
                    snprintf(line_bufs[line_i], sizeof(line_bufs[line_i]),
                             "- Health (%.1f / %.1f)", hcur, hmax);
                    {
                        Clay_String cs = {false, (int32_t)strlen(line_bufs[line_i]), line_bufs[line_i]};
                        CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {205, 215, 232, 255}, .fontSize = 16}));
                    }
                    line_i++;
                }
                if (has_collider) {
                    if (collider_is_capsule) {
                        snprintf(line_bufs[line_i], sizeof(line_bufs[line_i]),
                                 "- Capsule Collider (r=%.2f h=%.2f)",
                                 capsule_radius, capsule_half_height);
                    } else {
                        snprintf(line_bufs[line_i], sizeof(line_bufs[line_i]),
                                 "- Box Collider (%.2f, %.2f, %.2f, %.2f)",
                                 crect.x, crect.y, crect.w, crect.h);
                    }
                    {
                        Clay_String cs = {false, (int32_t)strlen(line_bufs[line_i]), line_bufs[line_i]};
                        CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {205, 215, 232, 255}, .fontSize = 16}));
                    }
                    line_i++;
                }
                if (has_mesh) {
                    snprintf(line_bufs[line_i], sizeof(line_bufs[line_i]),
                             "- Mesh (visible=%d)", mesh_visible);
                    {
                        Clay_String cs = {false, (int32_t)strlen(line_bufs[line_i]), line_bufs[line_i]};
                        CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {190, 220, 255, 255}, .fontSize = 16}));
                    }
                    line_i++;
                }
                if (has_animation) {
                    uint32_t total_clips = gs->mesh3d.clip_count;
                    if (total_clips > 0) {
                        snprintf(line_bufs[line_i], sizeof(line_bufs[line_i]),
                                 "- Animation (playing=%d clip=%d/%u time=%.2f speed=%.2f)",
                                 anim_playing, anim_clip, (unsigned int)total_clips, anim_time, anim_speed);
                    } else {
                        snprintf(line_bufs[line_i], sizeof(line_bufs[line_i]),
                                 "- Animation (playing=%d clip=%d time=%.2f speed=%.2f)",
                                 anim_playing, anim_clip, anim_time, anim_speed);
                    }
                    {
                        Clay_String cs = {false, (int32_t)strlen(line_bufs[line_i]), line_bufs[line_i]};
                        CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {164, 198, 236, 255}, .fontSize = 16}));
                    }
                    line_i++;
                }
                if (has_camera) {
                    snprintf(line_bufs[line_i], sizeof(line_bufs[line_i]),
                             "- Camera (fov=%.1f near=%.2f far=%.1f)", cam_fov, cam_near, cam_far);
                    {
                        Clay_String cs = {false, (int32_t)strlen(line_bufs[line_i]), line_bufs[line_i]};
                        CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {190, 220, 255, 255}, .fontSize = 16}));
                    }
                    line_i++;

                    snprintf(line_bufs[line_i], sizeof(line_bufs[line_i]),
                             "  target=(%.2f, %.2f, %.2f) up=(%.2f, %.2f, %.2f)",
                             cam_target.x, cam_target.y, cam_target.z,
                             cam_up.x, cam_up.y, cam_up.z);
                    {
                        Clay_String cs = {false, (int32_t)strlen(line_bufs[line_i]), line_bufs[line_i]};
                        CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {164, 198, 236, 255}, .fontSize = 16}));
                    }
                    line_i++;
                }
                if (has_parent) {
                    snprintf(line_bufs[line_i], sizeof(line_bufs[line_i]),
                             "- Parent (entity %d)", parent_idx);
                    {
                        Clay_String cs = {false, (int32_t)strlen(line_bufs[line_i]), line_bufs[line_i]};
                        CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {170, 210, 255, 255}, .fontSize = 16}));
                    }
                    line_i++;
                }
                if (has_parent_transform) {
                    snprintf(line_bufs[line_i], sizeof(line_bufs[line_i]),
                             "- Parent Transform (entity %d)", parent_transform_idx);
                    {
                        Clay_String cs = {false, (int32_t)strlen(line_bufs[line_i]), line_bufs[line_i]};
                        CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {170, 255, 190, 255}, .fontSize = 16}));
                    }
                    line_i++;
                }
                if (has_parent_rotation) {
                    Clay_String cs = CLAY_STRING("- Parent Rotation");
                    CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {255, 210, 170, 255}, .fontSize = 16}));
                }
            }
        }
    }

    commands = Clay_EndLayout();
    e->inspector_cmd_count = commands.length;
    e->inspector_cmd_array = commands.internalArray;
}

static const char *file_menu_action_labels[FILE_MENU_ACTION_COUNT] = {
    "Open Project",
    "New Project"
};

static void menu_bar_layout(game_state *gs, editor_state *es) {
    editor_state *e = es;
    dock_state *d = (dock_state *)e->dock;
    Clay_Context *ctx = (Clay_Context *)e->menu_bar_clay_ctx;
    Clay_RenderCommandArray commands;
    int win_w = 1600;
    int win_h = 900;
    int play_mode = editor_is_play_mode(gs);
    int menu_click_handled = 0;

    if (!ctx || !d || !d->windows[0].in_use || !d->windows[0].sdl_window) {
        e->menu_bar_cmd_count = 0;
        e->menu_bar_cmd_array = NULL;
        e->menu_click = 0;
        return;
    }

    SDL_GetWindowSize((SDL_Window *)d->windows[0].sdl_window, &win_w, &win_h);
    if (win_w < 1) win_w = 1;
    if (win_h < 1) win_h = 1;

    Clay_SetCurrentContext(ctx);
    Clay_SetLayoutDimensions((Clay_Dimensions){(float)win_w, (float)MENU_BAR_HEIGHT});
    Clay_SetPointerState((Clay_Vector2){e->menu_mouse_x, e->menu_mouse_y}, (bool)e->menu_click);
    Clay_BeginLayout();

    CLAY(CLAY_ID("MenuBarRoot"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIXED(MENU_BAR_HEIGHT) },
            .padding = { .left = 6, .right = 8, .top = 2, .bottom = 2 },
            .childGap = 6,
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
            .childAlignment = { CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER }
        },
        .backgroundColor = {24, 28, 36, 255}
    }) {
        CLAY(CLAY_ID("MenuFile"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIT({0}), CLAY_SIZING_FIXED(22) },
                .padding = { .left = 10, .right = 10, .top = 2, .bottom = 2 },
                .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER }
            },
            .backgroundColor = (e->menu_open == MENU_FILE || Clay_Hovered())
                             ? ((Clay_Color){56, 66, 84, 240})
                             : ((Clay_Color){0, 0, 0, 0}),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            int hovered = Clay_Hovered();
            if (hovered && e->menu_click) {
                e->menu_open = (e->menu_open == MENU_FILE) ? MENU_NONE : MENU_FILE;
                e->menu_hover = -1;
                menu_click_handled = 1;
            }

            CLAY_TEXT(CLAY_STRING("File"), CLAY_TEXT_CONFIG({
                .textColor = {224, 230, 240, 255},
                .fontSize = 16
            }));

            if (e->menu_open == MENU_FILE) {
                int i;
                CLAY(CLAY_ID("FileDropdown"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(190), CLAY_SIZING_FIT({0}) },
                        .padding = { .left = 4, .right = 4, .top = 4, .bottom = 4 },
                        .childGap = 2,
                        .layoutDirection = CLAY_TOP_TO_BOTTOM
                    },
                    .floating = {
                        .attachTo = CLAY_ATTACH_TO_PARENT,
                        .attachPoints = {
                            .element = CLAY_ATTACH_POINT_LEFT_TOP,
                            .parent = CLAY_ATTACH_POINT_LEFT_BOTTOM
                        }
                    },
                    .backgroundColor = {38, 44, 58, 255},
                    .cornerRadius = CLAY_CORNER_RADIUS(4)
                }) {
                    for (i = 0; i < FILE_MENU_ACTION_COUNT; i++) {
                        CLAY(CLAY_IDI("FileMenuItem", i), {
                            .layout = {
                                .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIXED(24) },
                                .padding = { .left = 8, .right = 8, .top = 4, .bottom = 4 },
                                .childAlignment = { CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER }
                            },
                            .backgroundColor = Clay_Hovered()
                                             ? ((Clay_Color){68, 86, 120, 255})
                                             : ((Clay_Color){0, 0, 0, 0}),
                            .cornerRadius = CLAY_CORNER_RADIUS(3)
                        }) {
                            int item_hovered = Clay_Hovered();
                            if (item_hovered) e->menu_hover = i;
                            if (item_hovered && e->menu_click) {
                                editor_dispatch_file_menu_action(gs, i);
                                e->menu_open = MENU_NONE;
                                e->menu_hover = -1;
                                menu_click_handled = 1;
                            }

                            {
                                Clay_String item_label = {
                                    false,
                                    (int32_t)strlen(file_menu_action_labels[i]),
                                    file_menu_action_labels[i]
                                };
                                CLAY_TEXT(item_label, CLAY_TEXT_CONFIG({
                                    .textColor = {226, 232, 244, 255},
                                    .fontSize = 16
                                }));
                            }
                        }
                    }
                }
            }
        }

        CLAY(CLAY_ID("MenuFill"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIXED(1) }
            }
        }) {}

        CLAY(CLAY_ID("MenuPlayMode"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(76), CLAY_SIZING_FIXED(22) },
                .padding = { .left = 10, .right = 10, .top = 2, .bottom = 2 },
                .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER }
            },
            .backgroundColor = play_mode
                             ? ((Clay_Color){146, 72, 72, 240})
                             : ((Clay_Color){68, 122, 76, 240}),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            if (Clay_Hovered() && e->menu_click) {
                editor_set_play_mode(gs, e, !play_mode);
                play_mode = editor_is_play_mode(gs);
                menu_click_handled = 1;
            }
            {
                const char *label = play_mode ? "Stop" : "Play";
                Clay_String text = { false, (int32_t)strlen(label), label };
                CLAY_TEXT(text, CLAY_TEXT_CONFIG({
                    .textColor = {238, 242, 248, 255},
                    .fontSize = 16
                }));
            }
        }
    }

    commands = Clay_EndLayout();
    if (e->menu_click && e->menu_open == MENU_FILE && !menu_click_handled) {
        if (!point_in_rect(e->menu_mouse_x, e->menu_mouse_y, 0.0f, 0.0f, (float)win_w, (float)MENU_BAR_HEIGHT) &&
            !file_menu_dropdown_contains_point(e->menu_mouse_x, e->menu_mouse_y)) {
            e->menu_open = MENU_NONE;
            e->menu_hover = -1;
        }
    }
    e->menu_bar_cmd_count = commands.length;
    e->menu_bar_cmd_array = commands.internalArray;
    e->menu_click = 0;
}

static void editor_toolbar_layout(game_state *gs, editor_state *es) {
    editor_state *e = es;
    Clay_Context *ctx = (Clay_Context *)e->editor_toolbar_clay_ctx;
    Clay_RenderCommandArray commands;
    int perspective_active;
    int play_active;

    (void)gs;

    if (!ctx || e->panel_w < 1.0f || e->panel_h < 1.0f) {
        e->editor_toolbar_cmd_count = 0;
        e->editor_toolbar_cmd_array = NULL;
        return;
    }

    perspective_active = (e->cam_projection_mode != EDITOR_CAMERA_ORTHOGRAPHIC);
    play_active = editor_is_play_mode(gs);

    Clay_SetCurrentContext(ctx);
    Clay_SetLayoutDimensions((Clay_Dimensions){e->panel_w, e->panel_h});
    Clay_BeginLayout();

    CLAY(CLAY_ID("EDToolbarRoot"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_GROW({0}) },
            .padding = {
                .left = (uint16_t)EDITOR_VIEWBAR_MARGIN_X,
                .right = (uint16_t)EDITOR_VIEWBAR_MARGIN_X,
                .top = (uint16_t)EDITOR_VIEWBAR_MARGIN_Y,
                .bottom = 0
            },
            .layoutDirection = CLAY_TOP_TO_BOTTOM
        }
    }) {
        CLAY(CLAY_ID("EDToolbarRow"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIXED(EDITOR_VIEWBAR_ROW_HEIGHT) },
                .padding = {
                    .left = (uint16_t)EDITOR_VIEWBAR_ROW_PADDING_X,
                    .right = (uint16_t)EDITOR_VIEWBAR_ROW_PADDING_X,
                    .top = (uint16_t)((EDITOR_VIEWBAR_ROW_HEIGHT - EDITOR_VIEWBAR_BUTTON_HEIGHT) * 0.5f),
                    .bottom = (uint16_t)((EDITOR_VIEWBAR_ROW_HEIGHT - EDITOR_VIEWBAR_BUTTON_HEIGHT) * 0.5f)
                },
                .childGap = (uint16_t)EDITOR_VIEWBAR_GROUP_GAP,
                .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER},
                .layoutDirection = CLAY_LEFT_TO_RIGHT
            },
            .backgroundColor = {28, 34, 44, 224},
            .cornerRadius = CLAY_CORNER_RADIUS(6)
        }) {
            CLAY(CLAY_ID("EDToolbarProjectionGroup"), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIT({0}), CLAY_SIZING_FIT({0}) },
                    .childGap = (uint16_t)EDITOR_VIEWBAR_BUTTON_GAP,
                    .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER},
                    .layoutDirection = CLAY_LEFT_TO_RIGHT
                }
            }) {
                CLAY(CLAY_ID("EDToolbarPerspective"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(EDITOR_VIEWBAR_TOGGLE_WIDTH),
                                    CLAY_SIZING_FIXED(EDITOR_VIEWBAR_BUTTON_HEIGHT) },
                        .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}
                    },
                    .backgroundColor = perspective_active
                                       ? ((Clay_Color){80, 120, 176, 230})
                                       : ((Clay_Color){56, 66, 84, 220}),
                    .cornerRadius = CLAY_CORNER_RADIUS(4)
                }) {
                    Clay_String cs = CLAY_STRING("Perspective");
                    CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {236, 240, 248, 255}, .fontSize = 16}));
                }

                CLAY(CLAY_ID("EDToolbarOrthographic"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(EDITOR_VIEWBAR_TOGGLE_WIDTH),
                                    CLAY_SIZING_FIXED(EDITOR_VIEWBAR_BUTTON_HEIGHT) },
                        .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}
                    },
                    .backgroundColor = perspective_active
                                       ? ((Clay_Color){56, 66, 84, 220})
                                       : ((Clay_Color){80, 120, 176, 230}),
                    .cornerRadius = CLAY_CORNER_RADIUS(4)
                }) {
                    Clay_String cs = CLAY_STRING("Orthographic");
                    CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {236, 240, 248, 255}, .fontSize = 16}));
                }
            }

            CLAY(CLAY_ID("EDToolbarViewGroup"), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIT({0}), CLAY_SIZING_FIT({0}) },
                    .childGap = (uint16_t)EDITOR_VIEWBAR_BUTTON_GAP,
                    .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER},
                    .layoutDirection = CLAY_LEFT_TO_RIGHT
                }
            }) {
                CLAY(CLAY_ID("EDToolbarFront"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(EDITOR_VIEWBAR_VIEW_WIDTH),
                                    CLAY_SIZING_FIXED(EDITOR_VIEWBAR_BUTTON_HEIGHT) },
                        .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}
                    },
                    .backgroundColor = {56, 66, 84, 220},
                    .cornerRadius = CLAY_CORNER_RADIUS(4)
                }) {
                    Clay_String cs = CLAY_STRING("Front");
                    CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {236, 240, 248, 255}, .fontSize = 16}));
                }

                CLAY(CLAY_ID("EDToolbarSide"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(EDITOR_VIEWBAR_VIEW_WIDTH),
                                    CLAY_SIZING_FIXED(EDITOR_VIEWBAR_BUTTON_HEIGHT) },
                        .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}
                    },
                    .backgroundColor = {56, 66, 84, 220},
                    .cornerRadius = CLAY_CORNER_RADIUS(4)
                }) {
                    Clay_String cs = CLAY_STRING("Side");
                    CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {236, 240, 248, 255}, .fontSize = 16}));
                }

                CLAY(CLAY_ID("EDToolbarTop"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(EDITOR_VIEWBAR_VIEW_WIDTH),
                                    CLAY_SIZING_FIXED(EDITOR_VIEWBAR_BUTTON_HEIGHT) },
                        .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}
                    },
                    .backgroundColor = {56, 66, 84, 220},
                    .cornerRadius = CLAY_CORNER_RADIUS(4)
                }) {
                    Clay_String cs = CLAY_STRING("Top");
                    CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {236, 240, 248, 255}, .fontSize = 16}));
                }
            }

           
        }
    }

    commands = Clay_EndLayout();
    e->editor_toolbar_cmd_count = commands.length;
    e->editor_toolbar_cmd_array = commands.internalArray;
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
            SDL_GetWindowSize((SDL_Window *)d->windows[wi].sdl_window, &ww, &wh);
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
            SDL_GetWindowSize((SDL_Window *)d->windows[0].sdl_window, &dw2, &dh2);
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
            SDL_GetWindowSize((SDL_Window *)d->windows[0].sdl_window, &dw3, &dh3);
            e->panel_x = 0; e->panel_y = 0;
            e->panel_w = (float)dw3; e->panel_h = (float)dh3;
            e->window = d->windows[0].sdl_window;
        }
    }

    /* ── Profiler Clay layout (produces render commands for externals) ── */
    {
        int scene_count = gs->scene_entities ? gs->scene_entity_count : 0;
        if (e->scene_selected_entity >= scene_count)
            e->scene_selected_entity = scene_count - 1;
        if (e->scene_selected_entity < 0 && scene_count > 0)
            e->scene_selected_entity = 0;
    }
    profiler_layout(gs, es);
    scene_tree_layout(gs, es);
    project_browser_layout(gs, es);
    inspector_layout(gs, es);
    menu_bar_layout(gs, es);
    editor_toolbar_layout(gs, es);

    /* ── Camera, gizmo, lines (existing editor behavior) ── */
    if (!editor_is_play_mode(gs)) {
        update_camera(gs, es);
        update_gizmo_hover(gs, es);
    } else {
        if (e->cam_mouse_look) editor_end_mouse_look(e);
        e->gizmo_hovered = GIZMO_NONE;
        e->gizmo_active = GIZMO_NONE;
        e->gizmo_entity_index = -1;
    }
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

    /* ── Menu bar input (main window top strip) ── */
    if (ev->type == SDL_EVENT_MOUSE_MOTION) {
        evwin = SDL_GetWindowFromEvent(ev);
        if (d->windows[0].in_use &&
            evwin == (SDL_Window *)d->windows[0].sdl_window) {
            e->menu_mouse_x = ev->motion.x;
            e->menu_mouse_y = ev->motion.y;
            if (menu_bar_contains_point(d, evwin, ev->motion.x, ev->motion.y)) {
                return 1;
            }
        }
    }

    if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        ev->button.button == SDL_BUTTON_LEFT) {
        evwin = SDL_GetWindowFromEvent(ev);
        if (d->windows[0].in_use &&
            evwin == (SDL_Window *)d->windows[0].sdl_window) {
            e->menu_mouse_x = ev->button.x;
            e->menu_mouse_y = ev->button.y;
            if (menu_bar_contains_point(d, evwin, ev->button.x, ev->button.y) ||
                e->menu_open != MENU_NONE) {
                e->menu_click = 1;
                return 1;
            }
        }
    }

    if (ev->type == SDL_EVENT_KEY_DOWN &&
        !ev->key.repeat &&
        ev->key.key == SDLK_ESCAPE &&
        e->menu_open != MENU_NONE) {
        e->menu_open = MENU_NONE;
        e->menu_hover = -1;
        return 1;
    }

    /* ── Dock: Divider resize — mouse down to start ── */
    if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        ev->button.button == SDL_BUTTON_LEFT &&
        drag->phase == DRAG_IDLE && !resize->active) {
        evwin = SDL_GetWindowFromEvent(ev);
        {
            int win_idx = dock_window_for_sdl(d, evwin);
            if (win_idx >= 0) {
                float pmx = ev->button.x;
                float pmy = ev->button.y;
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
        float pmx = ev->motion.x;
        float pmy = ev->motion.y;
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
                float mx = ev->button.x;
                float my = ev->button.y;
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
                   dock_layout now uses SDL_GetWindowSize (logical coords),
                   so mouse coords can be used directly. */
                int root = d->windows[drag->source_window].root_node;
                int hover = dock_node_at_point(d, root, mx, my);
                if (hover >= 0) {
                    DockNode *hn = &d->nodes[hover];
                    DropZone zone = dock_drop_zone(hn, mx, my);
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
                float pmx = ev->motion.x;
                float pmy = ev->motion.y;
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

    if (ev->type == SDL_EVENT_KEY_DOWN && !ev->key.repeat) {
        SDL_Window *key_window = SDL_GetWindowFromID(ev->key.windowID);
        if (key_window == (SDL_Window *)e->window &&
            ((ev->key.mod & SDL_KMOD_GUI) || (ev->key.mod & SDL_KMOD_CTRL)) &&
            ev->key.key == SDLK_S) {
            if (!gs->project_loaded || !gs->project_path[0]) {
                fprintf(stderr, "Save skipped: no project file loaded\n");
            } else if (editor_save_scene_to_toml(gs, gs->project_path)) {
                fprintf(stderr, "Scene saved to %s\n", gs->project_path);
            } else {
                fprintf(stderr, "Save failed for %s\n", gs->project_path);
            }
            return 1;
        }
    }

    if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        ev->button.button == SDL_BUTTON_LEFT) {
        evwin = SDL_GetWindowFromEvent(ev);
        {
            float lx, ly;
            if (panel_event_hit(d, PANEL_EDITOR, evwin, ev->button.x, ev->button.y, &lx, &ly) &&
                editor_toolbar_contains_point(e, lx, ly)) {
                int action = editor_toolbar_button_hit(e, lx, ly);
                if (action != EDITOR_TOOLBAR_ACTION_NONE) {
                    editor_apply_toolbar_action(gs, e, action);
                }
                return 1;
            }
        }
    }

    /* Mouse-look: right-button drag, or left-button drag inside editor panel (away from gizmo). */
    if (!editor_is_play_mode(gs) && ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        evwin = SDL_GetWindowFromEvent(ev);
        if (ev->button.button == SDL_BUTTON_RIGHT &&
            evwin == (SDL_Window *)e->window) {
            editor_begin_mouse_look(e, SDL_BUTTON_RIGHT);
        } else if (ev->button.button == SDL_BUTTON_LEFT &&
                   e->gizmo_hovered == GIZMO_NONE &&
                   e->gizmo_active == GIZMO_NONE) {
            float lx = 0.0f, ly = 0.0f;
            if (panel_event_hit(d, PANEL_EDITOR, evwin, ev->button.x, ev->button.y, &lx, &ly)) {
                if (editor_toolbar_contains_point(e, lx, ly)) return 1;
                editor_begin_mouse_look(e, SDL_BUTTON_LEFT);
                return 1;
            }
        }
    }
    if (!editor_is_play_mode(gs) &&
        ev->type == SDL_EVENT_MOUSE_BUTTON_UP &&
        (ev->button.button == SDL_BUTTON_LEFT ||
         ev->button.button == SDL_BUTTON_RIGHT)) {
        if (e->cam_mouse_look && e->cam_mouse_button == ev->button.button) {
            editor_end_mouse_look(e);
        }
    }
    if (!editor_is_play_mode(gs) && ev->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        evwin = SDL_GetWindowFromEvent(ev);
        if (evwin == (SDL_Window *)e->window && e->cam_mouse_look) {
            editor_end_mouse_look(e);
        }
    }

    /* Gizmo: left-click to start drag */
    if (!editor_is_play_mode(gs) &&
        ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        ev->button.button == SDL_BUTTON_LEFT) {
        evwin = SDL_GetWindowFromEvent(ev);
        if (evwin == (SDL_Window *)e->window && e->gizmo_hovered != GIZMO_NONE) {
            float fw2, fh2, giz_len, drag_axis_len;
            Mat4 ed_proj, ed_view, vp;
            Vec3 ed_fwd, giz;
            Vec3 basis_axes[3];
            int selected = e->scene_selected_entity;
            int is_transform_drag = (e->gizmo_hovered >= GIZMO_TRANSLATE_X &&
                                     e->gizmo_hovered <= GIZMO_TRANSLATE_Z);

            if (!selected_entity_gizmo_basis(gs, e, &giz, basis_axes, &giz_len)) {
                e->gizmo_active = GIZMO_NONE;
                e->gizmo_entity_index = -1;
                return 0;
            }

            e->gizmo_active = e->gizmo_hovered;
            e->gizmo_entity_index = selected;
            e->gizmo_drag_accum = 0;
            e->gizmo_drag_mode = 0;
            e->gizmo_drag_axis_local_scale = 1.0f;
            drag_axis_len = giz_len;

            if (is_transform_drag) {
                transform_component *tc = find_transform_component_mut(gs, selected);
                int axis_index;
                if (!tc) {
                    e->gizmo_active = GIZMO_NONE;
                    e->gizmo_entity_index = -1;
                    return 0;
                }
                e->gizmo_drag_mode = 0;
                e->gizmo_drag_start_entity_pos = tc->position;
                axis_index = e->gizmo_active - 1;
                if (axis_index < 0) axis_index = 0;
                if (axis_index > 2) axis_index = 2;
                e->gizmo_drag_axis_world = basis_axes[axis_index];
            } else {
                capsule_collider_component *cc = find_capsule_collider_component_mut(gs, selected);
                capsule_handle_info handles[6];
                int i, found = -1;
                if (!cc) {
                    e->gizmo_active = GIZMO_NONE;
                    e->gizmo_entity_index = -1;
                    return 0;
                }
                build_capsule_edit_handles(editor_world_matrix_for_entity(gs, selected),
                                           cc->radius, cc->half_height, handles);
                for (i = 0; i < 6; i++) {
                    if (handles[i].id == e->gizmo_hovered) {
                        found = i;
                        break;
                    }
                }
                if (found < 0) {
                    e->gizmo_active = GIZMO_NONE;
                    e->gizmo_entity_index = -1;
                    return 0;
                }

                e->gizmo_drag_mode = (handles[found].id == GIZMO_CAPSULE_HEIGHT_TOP ||
                                      handles[found].id == GIZMO_CAPSULE_HEIGHT_BOTTOM) ? 2 : 1;
                e->gizmo_drag_start_capsule_radius = cc->radius;
                e->gizmo_drag_start_capsule_half_height = cc->half_height;
                e->gizmo_drag_axis_world = handles[found].axis_world;
                e->gizmo_drag_axis_local_scale = handles[found].axis_local_scale;
                giz = handles[found].position;
                drag_axis_len = giz_len * 0.8f;
                if (drag_axis_len < 0.12f) drag_axis_len = 0.12f;
            }

            fw2 = e->panel_w; fh2 = e->panel_h;
            ed_proj = editor_projection_matrix(e, fw2, fh2);
            ed_fwd  = cam_forward(e);
            ed_view = mat4_look_at(e->cam_pos, vec3_add(e->cam_pos, ed_fwd), VEC3(0, 1, 0));
            vp = mat4_mul(ed_proj, ed_view);
            setup_drag_screen_axis(e, vp, fw2, fh2, giz, e->gizmo_drag_axis_world, drag_axis_len);
        }
    }

    /* Gizmo: mouse motion during drag */
    if (!editor_is_play_mode(gs) &&
        ev->type == SDL_EVENT_MOUSE_MOTION && e->gizmo_active != GIZMO_NONE) {
        float dot = ev->motion.xrel * e->gizmo_screen_axis.x
                  + ev->motion.yrel * e->gizmo_screen_axis.y;
        float world_delta;
        e->gizmo_drag_accum += dot;
        world_delta = e->gizmo_drag_accum * e->gizmo_world_per_pixel;

        if (e->gizmo_drag_mode == 0) {
            Vec3 delta;
            transform_component *tc = find_transform_component_mut(gs, e->gizmo_entity_index);
            if (!tc) return 0;
            delta = vec3_scale(e->gizmo_drag_axis_world, world_delta);
            tc->position = vec3_add(e->gizmo_drag_start_entity_pos, delta);
        } else {
            capsule_collider_component *cc = find_capsule_collider_component_mut(gs, e->gizmo_entity_index);
            float axis_scale = e->gizmo_drag_axis_local_scale;
            float local_delta;
            rect updated;
            if (!cc) return 0;
            if (axis_scale < 0.001f) axis_scale = 1.0f;
            local_delta = world_delta / axis_scale;
            if (e->gizmo_drag_mode == 1) {
                cc->radius = fmaxf(0.05f, e->gizmo_drag_start_capsule_radius + local_delta);
            } else {
                cc->half_height = fmaxf(0.05f, e->gizmo_drag_start_capsule_half_height + local_delta);
            }
            updated = capsule_aabb_rect(cc->radius, cc->half_height);
            updated.x = cc->aabb.x;
            updated.y = cc->aabb.y;
            cc->aabb = updated;
        }
    }

    /* Gizmo: left-button up ends drag */
    if (!editor_is_play_mode(gs) &&
        ev->type == SDL_EVENT_MOUSE_BUTTON_UP &&
        ev->button.button == SDL_BUTTON_LEFT) {
        e->gizmo_active = GIZMO_NONE;
        e->gizmo_entity_index = -1;
        e->gizmo_drag_mode = 0;
    }

    /* ── Profiler + Scene Tree + Project Browser input for Clay containers ── */
    if (ev->type == SDL_EVENT_MOUSE_MOTION) {
        evwin = SDL_GetWindowFromEvent(ev);
        {
            float lx, ly;
            if (panel_event_hit(d, PANEL_PROFILER, evwin, ev->motion.x, ev->motion.y, &lx, &ly)) {
                e->prof_mouse_x = lx;
                e->prof_mouse_y = ly;
            }
            if (panel_event_hit(d, PANEL_SCENE_TREE, evwin, ev->motion.x, ev->motion.y, &lx, &ly)) {
                e->scene_tree_mouse_x = lx;
                e->scene_tree_mouse_y = ly;
            } else {
                e->scene_tree_mouse_x = -10000.0f;
                e->scene_tree_mouse_y = -10000.0f;
            }
            if (panel_event_hit(d, PANEL_ASSETS, evwin, ev->motion.x, ev->motion.y, &lx, &ly)) {
                e->project_browser_mouse_x = lx;
                e->project_browser_mouse_y = ly;
            } else {
                e->project_browser_mouse_x = -10000.0f;
                e->project_browser_mouse_y = -10000.0f;
            }
        }
    }
    if (ev->type == SDL_EVENT_MOUSE_WHEEL) {
        evwin = SDL_GetWindowFromEvent(ev);
        {
            float mx, my;
            int consumed = 0;
            SDL_GetMouseState(&mx, &my);
            {
                float lx, ly;
                if (panel_event_hit(d, PANEL_EDITOR, evwin, mx, my, &lx, &ly) &&
                    !editor_toolbar_contains_point(e, lx, ly) &&
                    !editor_is_play_mode(gs)) {
                    float wheel = ev->wheel.y;
                    if (wheel != 0.0f) {
                        if (e->cam_projection_mode == EDITOR_CAMERA_ORTHOGRAPHIC) {
                            float base = editor_ortho_size(e);
                            float factor = 1.0f - (wheel * 0.12f);
                            if (factor < 0.1f) factor = 0.1f;
                            e->cam_ortho_size = fmaxf(2.0f, fminf(80.0f, base * factor));
                        } else {
                            Vec3 forward = cam_forward(e);
                            float step = fmaxf(0.2f, e->cam_speed * 0.25f);
                            e->cam_pos = vec3_add(e->cam_pos, vec3_scale(forward, wheel * step));
                        }
                        consumed = 1;
                    }
                }
            }
            if (panel_event_hit(d, PANEL_PROFILER, evwin, mx, my, NULL, NULL)) {
                e->prof_scroll_y += ev->wheel.y * 3.0f;
                consumed = 1;
            }
            if (panel_event_hit(d, PANEL_SCENE_TREE, evwin, mx, my, NULL, NULL)) {
                e->scene_tree_scroll_y += ev->wheel.y * 3.0f;
                consumed = 1;
            }
            if (panel_event_hit(d, PANEL_ASSETS, evwin, mx, my, NULL, NULL)) {
                e->project_browser_scroll_y += ev->wheel.y * 3.0f;
                consumed = 1;
            }
            if (consumed) return 1;
        }
    }
    if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev->button.button == SDL_BUTTON_LEFT) {
        evwin = SDL_GetWindowFromEvent(ev);
        {
            float lx, ly;
            e->prof_mouse_down = panel_event_hit(d, PANEL_PROFILER, evwin,
                ev->button.x, ev->button.y, &lx, &ly);
            if (e->prof_mouse_down) {
                e->prof_mouse_x = lx;
                e->prof_mouse_y = ly;
                e->prof_click = 1;
            }

            e->scene_tree_mouse_down = panel_event_hit(d, PANEL_SCENE_TREE, evwin,
                ev->button.x, ev->button.y, &lx, &ly);
            if (e->scene_tree_mouse_down) {
                e->scene_tree_mouse_x = lx;
                e->scene_tree_mouse_y = ly;
                e->scene_tree_click = 1;
            }

            e->project_browser_mouse_down = panel_event_hit(d, PANEL_ASSETS, evwin,
                ev->button.x, ev->button.y, &lx, &ly);
            if (e->project_browser_mouse_down) {
                e->project_browser_mouse_x = lx;
                e->project_browser_mouse_y = ly;
            }
        }
    }
    if (ev->type == SDL_EVENT_MOUSE_BUTTON_UP && ev->button.button == SDL_BUTTON_LEFT) {
        e->prof_mouse_down = 0;
        e->scene_tree_mouse_down = 0;
        e->project_browser_mouse_down = 0;
    }

    return 0;
}
