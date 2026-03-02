#include <game.h>
#include <export.h>
#include <math3d.h>
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <ctype.h>

#define CLAY_IMPLEMENTATION
#include "clay.h"

#include "cache_profiler.h"
#include "cpu_profiler.h"
#include "state_migration.gen.h"

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
#define EDITOR_PICK_MIN_SCREEN_RADIUS_PX 5.5f
#define EDITOR_PICK_TINY_MAX_DISTANCE    8.0f

/* Shared UI scale for editor panels. Keep typography and spacing tokenized so
   scene tree, browser, inspector, and top bars feel consistent. */
#define UI_FONT_TITLE      15
#define UI_FONT_BODY       14
#define UI_FONT_SECONDARY  13

#define UI_SPACE_XXS       2
#define UI_SPACE_XS        4
#define UI_SPACE_SM        6
#define UI_SPACE_MD        8
#define UI_SPACE_LG       10
#define UI_SPACE_XL       12

#define UI_PANEL_PADDING   UI_SPACE_LG
#define UI_PANEL_GAP       UI_SPACE_SM
#define UI_SECTION_PADDING UI_SPACE_MD
#define UI_SECTION_GAP     UI_SPACE_SM
#define UI_ROW_PADDING_X   UI_SPACE_MD
#define UI_ROW_PADDING_Y   UI_SPACE_XS
#define UI_ROW_GAP         UI_SPACE_XS
#define UI_TREE_INDENT     14

#define UI_RADIUS_SM       3
#define UI_RADIUS_MD       4
#define UI_RADIUS_LG       6

/* Bootstrap icon UTF-8 codepoints (from bootstrap-icons.css). */
#define UI_ICON_PLAY_FILL       "\xEF\x93\xB4" /* U+F4F4 */
#define UI_ICON_STOP_FILL       "\xEF\x96\x92" /* U+F592 */
#define UI_ICON_CAMERA          "\xEF\x88\xA0" /* U+F220 */
#define UI_ICON_BOUNDING_BOX    "\xEF\x86\xB6" /* U+F1B6 */
#define UI_ICON_ARROW_LEFT      "\xEF\x84\xAF" /* U+F12F */
#define UI_ICON_ARROW_RIGHT     "\xEF\x84\xB8" /* U+F138 */
#define UI_ICON_ARROW_DOWN      "\xEF\x84\xA8" /* U+F128 */
#define UI_ICON_ARROW_UP        "\xEF\x85\x88" /* U+F148 */

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

static int editor_cpu_prof_enabled(const editor_state *e) {
    return e && cpu_prof_get_capture_enabled();
}

#define EDITOR_CPU_ZONE_BEGIN(es, name) do { \
    if (editor_cpu_prof_enabled((es))) cpu_zone_begin(name); \
} while (0)
#define EDITOR_CPU_ZONE_END(es) do { \
    if (editor_cpu_prof_enabled((es))) cpu_zone_end(); \
} while (0)
#define EDITOR_CACHE_ZONE_BEGIN(name) do { cache_zone_begin(name); } while (0)
#define EDITOR_CACHE_ZONE_END()       do { cache_zone_end(); } while (0)

static int cpu_zone_highlights_record_tag(const char *zone, const char *record_tag) {
    if (!zone || !zone[0] || !record_tag || !record_tag[0]) return 0;

    if (strcmp(zone, record_tag) == 0) return 1;

    /* Precise Clay context mapping for profiler/editor UI zones. */
    if (strcmp(record_tag, "clay_editor") == 0 &&
        (strcmp(zone, "layout_memory_profiler") == 0 ||
         strcmp(zone, "ui_prepare_profiler") == 0)) return 1;
    if (strcmp(record_tag, "clay_scene_tree") == 0 &&
        (strcmp(zone, "layout_scene_tree") == 0 ||
         strcmp(zone, "ui_prepare_scene_tree") == 0)) return 1;
    if (strcmp(record_tag, "clay_project_browser") == 0 &&
        (strcmp(zone, "layout_project_browser") == 0 ||
         strcmp(zone, "ui_prepare_project_browser") == 0)) return 1;
    if (strcmp(record_tag, "clay_inspector") == 0 &&
        (strcmp(zone, "layout_inspector") == 0 ||
         strcmp(zone, "ui_prepare_inspector") == 0)) return 1;
    if (strcmp(record_tag, "clay_menu_bar") == 0 &&
        (strcmp(zone, "layout_menu_bar") == 0 ||
         strcmp(zone, "ui_prepare_menu_bar") == 0)) return 1;
    if (strcmp(record_tag, "clay_editor_toolbar") == 0 &&
        (strcmp(zone, "layout_editor_toolbar") == 0 ||
         strcmp(zone, "ui_prepare_editor_toolbar") == 0)) return 1;
    if (strcmp(record_tag, "clay_cache_prof") == 0 &&
        (strcmp(zone, "layout_cache_profiler") == 0 ||
         strcmp(zone, "ui_prepare_cache_profiler") == 0)) return 1;
    if (strcmp(record_tag, "clay_cpu_prof") == 0 &&
        (strcmp(zone, "layout_cpu_profiler") == 0 ||
         strcmp(zone, "ui_prepare_cpu_profiler") == 0)) return 1;

    /* Rendering pipeline zones map to rendering-owned arena records. */
    if (strncmp(zone, "render_", 7) == 0 ||
        strncmp(zone, "ui_prepare_", 11) == 0 ||
        strcmp(zone, "composite_windows") == 0 ||
        strcmp(zone, "build_composite_quads") == 0 ||
        strcmp(zone, "draw_upload_build") == 0 ||
        strcmp(zone, "upload_editor_lines") == 0 ||
        strcmp(zone, "upload_bone_matrices") == 0 ||
        strcmp(zone, "gpu_submit") == 0 ||
        strcmp(zone, "release_frame_resources") == 0) {
        if (strcmp(record_tag, "draw_commands") == 0 ||
            strcmp(record_tag, "mesh_commands") == 0 ||
            strcmp(record_tag, "debug_lines") == 0 ||
            strcmp(record_tag, "debug_vertices") == 0) {
            return 1;
        }
    }

    /* Editor update zones map to editor-owned arena records. */
    if (strcmp(zone, "editor_update") == 0 ||
        strcmp(zone, "editor_update_frame") == 0 ||
        strncmp(zone, "editor_", 7) == 0 ||
        strncmp(zone, "layout_", 7) == 0) {
        if (strcmp(record_tag, "dock_state") == 0 ||
            strncmp(record_tag, "clay_", 5) == 0) {
            return 1;
        }
    }

    /* Gameplay/engine zones map to gameplay ECS record buckets. */
    if (strcmp(zone, "engine_update_call") == 0 ||
        strcmp(zone, "update_engine") == 0 ||
        strcmp(zone, "input") == 0 ||
        strcmp(zone, "physics") == 0 ||
        strcmp(zone, "mesh_sync") == 0 ||
        strcmp(zone, "animation") == 0) {
        if (strstr(record_tag, "_components") ||
            strcmp(record_tag, "entities") == 0 ||
            strcmp(record_tag, "scene_models") == 0) {
            return 1;
        }
    }

    return 0;
}

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
                                int *color_id, int click, int *collapsed_nodes,
                                const char *cpu_focus_tag) {
    uint32_t i;
    for (i = 0; i < a->record_count; i++) {
        arena_record *r = &a->records[i];
        int rid = *row_id;
        int is_parent = r->child && r->child->record_count > 0;
        int hovered;
        int cpu_match = 0;
        Clay_Color swatch;

        if (cpu_focus_tag && cpu_focus_tag[0] && r->tag &&
            cpu_zone_highlights_record_tag(cpu_focus_tag, r->tag)) {
            cpu_match = 1;
        }

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
                const char *disclosure = (rid < PROFILER_TREE_MAX_NODES && collapsed_nodes[rid])
                    ? UI_ICON_ARROW_RIGHT
                    : UI_ICON_ARROW_DOWN;
                snprintf(label_bufs[bidx], sizeof(label_bufs[bidx]), "%s %s",
                    disclosure, r->tag);
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
                .backgroundColor = Clay_Hovered()
                    ? ((Clay_Color){60, 60, 70, 255})
                    : (cpu_match ? ((Clay_Color){52, 68, 92, 255}) : ((Clay_Color){0, 0, 0, 0}))
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
                                             : (cpu_match ? ((Clay_Color){220, 232, 255, 255})
                                                          : ((Clay_Color){200, 200, 200, 255})),
                        .fontSize = UI_FONT_BODY
                    }));
                }

                /* Size — right-aligned */
                {
                    Clay_String sz_s = {false, (int32_t)strlen(size_bufs[bidx]), size_bufs[bidx]};
                    CLAY_TEXT(sz_s, CLAY_TEXT_CONFIG({.textColor = {140, 140, 150, 255}, .fontSize = UI_FONT_BODY}));
                }
            }
        }

        (*row_id)++;

        if (r->child && !(rid < PROFILER_TREE_MAX_NODES && collapsed_nodes[rid])) {
            profiler_tree_arena(r->child, depth + 1, row_id,
                                base_offset + r->offset + (uint32_t)sizeof(arena),
                                hover, color_id, click, collapsed_nodes, cpu_focus_tag);
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

static int editor_utf8_decode_at(const char *text, int len, int index, int *out_cp, int *out_next) {
    unsigned char c0;
    int cp;
    int next;

    if (!text || index < 0 || index >= len || !out_cp) return 0;

    c0 = (unsigned char)text[index];
    cp = (int)c0;
    next = index + 1;

    if ((c0 & 0x80u) == 0) {
        /* ASCII */
    } else if ((c0 & 0xE0u) == 0xC0u && index + 1 < len) {
        cp = ((int)(c0 & 0x1Fu) << 6) |
             ((int)(text[index + 1] & 0x3F));
        next = index + 2;
    } else if ((c0 & 0xF0u) == 0xE0u && index + 2 < len) {
        cp = ((int)(c0 & 0x0Fu) << 12) |
             ((int)(text[index + 1] & 0x3F) << 6) |
             ((int)(text[index + 2] & 0x3F));
        next = index + 3;
    } else if ((c0 & 0xF8u) == 0xF0u && index + 3 < len) {
        cp = ((int)(c0 & 0x07u) << 18) |
             ((int)(text[index + 1] & 0x3F) << 12) |
             ((int)(text[index + 2] & 0x3F) << 6) |
             ((int)(text[index + 3] & 0x3F));
        next = index + 4;
    }

    *out_cp = cp;
    if (out_next) *out_next = next;
    return 1;
}

/* Simple text measurement for Clay — mostly ASCII metrics from externals.
   For UTF-8 glyphs (e.g. icon font codepoints), fall back to ~1em width. */
static Clay_Dimensions profiler_measure_text(Clay_StringSlice text,
                                              Clay_TextElementConfig *config,
                                              void *userData) {
    editor_state *e = (editor_state *)userData;
    float scale = (float)config->fontSize;
    float width = 0;
    int i = 0;
    while (i < text.length) {
        int ch = 0;
        int next = i + 1;
        if (!editor_utf8_decode_at(text.chars, text.length, i, &ch, &next)) break;
        if (ch >= 0 && ch < 128) {
            width += e->font_advances[ch] * scale;
        } else {
            width += 1.0f * scale;
        }
        if (next <= i) break;
        i = next;
    }
    return (Clay_Dimensions){
        ceilf(width + 1.0f),
        ceilf(e->font_line_height * scale)
    };
}

static int find_parent_component(const game_state *gs, int entity_index, int *out_parent_index) {
    int idx;
    if (entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return 0;
    idx = gs->parent_index[entity_index];
    if (idx < 0) return 0;
    if (out_parent_index) *out_parent_index = gs->parent_components[idx].parent_entity_index;
    return 1;
}

static int has_parent_transform_component(const game_state *gs, int entity_index, int *out_parent_index) {
    int idx;
    if (entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return 0;
    idx = gs->parent_transform_index[entity_index];
    if (idx < 0) return 0;
    if (out_parent_index) *out_parent_index = gs->parent_transform_components[idx].parent_entity_index;
    return 1;
}

static int has_parent_rotation_component(const game_state *gs, int entity_index) {
    if (entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return 0;
    return gs->parent_rotation_index[entity_index] >= 0;
}

static int has_mesh_component(const game_state *gs, int entity_index, int *out_visible) {
    int idx;
    if (entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return 0;
    idx = gs->mesh_index[entity_index];
    if (idx < 0) return 0;
    if (out_visible) *out_visible = gs->mesh_components[idx].visible;
    return 1;
}

static int has_animation_component(const game_state *gs, int entity_index,
                                   int *out_playing, int *out_clip, float *out_time, float *out_speed) {
    int idx;
    if (entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return 0;
    idx = gs->animation_index[entity_index];
    if (idx < 0) return 0;
    if (out_playing) *out_playing = gs->animation_components[idx].playing;
    if (out_clip) *out_clip = gs->animation_components[idx].active_clip;
    if (out_time) *out_time = gs->animation_components[idx].anim_time;
    if (out_speed) *out_speed = gs->animation_components[idx].speed;
    return 1;
}

static int has_transform_component(const game_state *gs, int entity_index, Vec3 *out_position) {
    int idx;
    if (entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return 0;
    idx = gs->transform_index[entity_index];
    if (idx < 0) return 0;
    if (out_position) *out_position = gs->transform_components[idx].position;
    return 1;
}

static transform_component *find_transform_component_mut(game_state *gs, int entity_index) {
    int idx;
    if (!gs || entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return NULL;
    idx = gs->transform_index[entity_index];
    return idx >= 0 ? &gs->transform_components[idx] : NULL;
}

static const box_collider_component *find_box_collider_component_read(const game_state *gs,
                                                                      int entity_index) {
    int idx;
    if (!gs || entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return NULL;
    idx = gs->box_collider_index[entity_index];
    return idx >= 0 ? &gs->box_collider_components[idx] : NULL;
}

static const capsule_collider_component *find_capsule_collider_component_read(const game_state *gs,
                                                                              int entity_index) {
    int idx;
    if (!gs || entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return NULL;
    idx = gs->capsule_collider_index[entity_index];
    return idx >= 0 ? &gs->capsule_collider_components[idx] : NULL;
}

static capsule_collider_component *find_capsule_collider_component_mut(game_state *gs, int entity_index) {
    int idx;
    if (!gs || entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return NULL;
    idx = gs->capsule_collider_index[entity_index];
    return idx >= 0 ? &gs->capsule_collider_components[idx] : NULL;
}

static int has_rotation_component(const game_state *gs, int entity_index, float *out_rotation_y_deg) {
    int idx;
    if (entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return 0;
    idx = gs->rotation_index[entity_index];
    if (idx < 0) return 0;
    if (out_rotation_y_deg) *out_rotation_y_deg = gs->rotation_components[idx].rotation_y_deg;
    return 1;
}

static int has_scale_component(const game_state *gs, int entity_index, Vec3 *out_scale) {
    int idx;
    if (entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return 0;
    idx = gs->scale_index[entity_index];
    if (idx < 0) return 0;
    if (out_scale) *out_scale = gs->scale_components[idx].scale;
    return 1;
}

static int has_velocity_component(const game_state *gs, int entity_index, Vec3 *out_velocity) {
    int idx;
    if (entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return 0;
    idx = gs->velocity_index[entity_index];
    if (idx < 0) return 0;
    if (out_velocity) *out_velocity = gs->velocity_components[idx].velocity;
    return 1;
}

static int has_health_component(const game_state *gs, int entity_index, float *out_health, float *out_max) {
    int idx;
    if (entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return 0;
    idx = gs->health_index[entity_index];
    if (idx < 0) return 0;
    if (out_health) *out_health = gs->health_components[idx].health;
    if (out_max) *out_max = gs->health_components[idx].max_health;
    return 1;
}

static int has_box_collider_component(const game_state *gs, int entity_index, rect *out_rect) {
    int idx;
    if (entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return 0;
    idx = gs->box_collider_index[entity_index];
    if (idx < 0) return 0;
    if (out_rect) *out_rect = gs->box_collider_components[idx].rect;
    return 1;
}

static int has_capsule_collider_component(const game_state *gs, int entity_index,
                                          float *out_radius, float *out_half_height,
                                          rect *out_aabb) {
    int idx;
    if (entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return 0;
    idx = gs->capsule_collider_index[entity_index];
    if (idx < 0) return 0;
    if (out_radius) *out_radius = gs->capsule_collider_components[idx].radius;
    if (out_half_height) *out_half_height = gs->capsule_collider_components[idx].half_height;
    if (out_aabb) *out_aabb = gs->capsule_collider_components[idx].aabb;
    return 1;
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
    int idx;
    if (entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return 0;
    idx = gs->camera_index[entity_index];
    if (idx < 0) return 0;
    if (out_fov) *out_fov = gs->camera_components[idx].fov_deg;
    if (out_near) *out_near = gs->camera_components[idx].near_plane;
    if (out_far) *out_far = gs->camera_components[idx].far_plane;
    if (out_target) *out_target = gs->camera_components[idx].target;
    if (out_up) *out_up = gs->camera_components[idx].up;
    return 1;
}

static int has_trigger_component(const game_state *gs, int entity_index,
                                 trigger_type *out_type, int *out_target_entity,
                                 float *out_radius, int *out_activated) {
    int idx;
    if (entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return 0;
    idx = gs->trigger_index[entity_index];
    if (idx < 0) return 0;
    if (out_type) *out_type = gs->trigger_components[idx].type;
    if (out_target_entity) *out_target_entity = gs->trigger_components[idx].target_entity;
    if (out_radius) *out_radius = gs->trigger_components[idx].radius;
    if (out_activated) *out_activated = gs->trigger_components[idx].activated;
    return 1;
}

static int has_rigid_body_component(const game_state *gs, int entity_index, int *out_use_gravity) {
    int idx;
    if (entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return 0;
    idx = gs->rigid_body_index[entity_index];
    if (idx < 0) return 0;
    if (out_use_gravity) *out_use_gravity = gs->rigid_body_components[idx].use_gravity ? 1 : 0;
    return 1;
}

static int has_character_controller_component(const game_state *gs, int entity_index,
                                              float *out_move_speed, float *out_jump_speed) {
    int idx;
    if (entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return 0;
    idx = gs->character_controller_index[entity_index];
    if (idx < 0) return 0;
    if (out_move_speed) *out_move_speed = gs->character_controller_components[idx].move_speed;
    if (out_jump_speed) *out_jump_speed = gs->character_controller_components[idx].jump_speed;
    return 1;
}

static const mesh_component *find_mesh_component_read(const game_state *gs, int entity_index) {
    int idx;
    if (!gs || entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return NULL;
    idx = gs->mesh_index[entity_index];
    return idx >= 0 ? &gs->mesh_components[idx] : NULL;
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

static char *editor_trim_whitespace(char *text) {
    char *end;
    if (!text) return text;
    while (*text && isspace((unsigned char)*text)) text++;
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
    return text;
}

static int editor_parse_bool_value(const char *text, int *out_value) {
    if (!text || !out_value) return 0;
    if (strcmp(text, "true") == 0 || strcmp(text, "1") == 0) {
        *out_value = 1;
        return 1;
    }
    if (strcmp(text, "false") == 0 || strcmp(text, "0") == 0) {
        *out_value = 0;
        return 1;
    }
    return 0;
}

static int editor_parse_int_list(const char *text, int *out_values, int max_values) {
    const char *p;
    int count;
    if (!text || !out_values || max_values <= 0) return 0;

    p = text;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '[') return 0;
    p++;

    count = 0;
    while (*p && *p != ']') {
        char *endp;
        long value;
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (!*p || *p == ']') break;

        value = strtol(p, &endp, 10);
        if (endp == p) break;
        if (count < max_values) {
            out_values[count++] = (int)value;
        }
        p = endp;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == ',') p++;
    }

    return count;
}

static int editor_build_layout_state_path(const char *project_path,
                                          char *out_path,
                                          int out_path_size) {
    const char *slash_fwd;
    const char *slash_back;
    const char *base;
    const char *dot;
    size_t prefix_len;
    const char *suffix = ".editor.layout.toml";
    size_t suffix_len = strlen(suffix);

    if (!project_path || !project_path[0] || !out_path || out_path_size <= 0) return 0;

    slash_fwd = strrchr(project_path, '/');
    slash_back = strrchr(project_path, '\\');
    base = project_path;
    if (slash_fwd && (!slash_back || slash_fwd > slash_back)) base = slash_fwd + 1;
    else if (slash_back) base = slash_back + 1;

    dot = strrchr(base, '.');
    if (dot) {
        prefix_len = (size_t)(dot - project_path);
    } else {
        prefix_len = strlen(project_path);
    }
    if (prefix_len + suffix_len + 1 > (size_t)out_path_size) return 0;

    memcpy(out_path, project_path, prefix_len);
    memcpy(out_path + prefix_len, suffix, suffix_len + 1);
    return 1;
}

static uint64_t editor_hash_bytes(uint64_t hash, const void *data, size_t size) {
    const unsigned char *bytes = (const unsigned char *)data;
    size_t i;
    for (i = 0; i < size; i++) {
        hash ^= (uint64_t)bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

static uint64_t editor_dock_layout_hash(const dock_state *d) {
    uint64_t hash = 1469598103934665603ull;
    int i;

    if (!d) return hash;

    for (i = 0; i < MAX_DOCK_NODES; i++) {
        const DockNode *n = &d->nodes[i];
        hash = editor_hash_bytes(hash, &n->in_use, sizeof(n->in_use));
        if (!n->in_use) continue;
        hash = editor_hash_bytes(hash, &n->type, sizeof(n->type));
        hash = editor_hash_bytes(hash, &n->children[0], sizeof(n->children[0]));
        hash = editor_hash_bytes(hash, &n->children[1], sizeof(n->children[1]));
        hash = editor_hash_bytes(hash, &n->ratio, sizeof(n->ratio));
        hash = editor_hash_bytes(hash, &n->panel_count, sizeof(n->panel_count));
        hash = editor_hash_bytes(hash, &n->active_tab, sizeof(n->active_tab));
        hash = editor_hash_bytes(hash, &n->panels[0], sizeof(n->panels));
    }
    for (i = 0; i < MAX_DOCK_WINDOWS; i++) {
        hash = editor_hash_bytes(hash, &d->windows[i].in_use, sizeof(d->windows[i].in_use));
        hash = editor_hash_bytes(hash, &d->windows[i].root_node, sizeof(d->windows[i].root_node));
    }

    return hash;
}

static int editor_validate_dock_node_recursive(const dock_state *d,
                                               int node_idx,
                                               uint8_t *visited,
                                               uint8_t *panel_seen) {
    const DockNode *n;
    int i;

    if (!d || !visited || !panel_seen) return 0;
    if (node_idx < 0 || node_idx >= MAX_DOCK_NODES) return 0;
    if (visited[node_idx]) return 0;
    n = &d->nodes[node_idx];
    if (!n->in_use) return 0;
    visited[node_idx] = 1;

    if (n->type == DOCK_SPLIT_H || n->type == DOCK_SPLIT_V) {
        if (n->children[0] < 0 || n->children[1] < 0) return 0;
        if (n->children[0] == n->children[1]) return 0;
        if (!editor_validate_dock_node_recursive(d, n->children[0], visited, panel_seen)) return 0;
        if (!editor_validate_dock_node_recursive(d, n->children[1], visited, panel_seen)) return 0;
        return 1;
    }

    if (n->type != DOCK_TABS) return 0;
    if (n->panel_count <= 0 || n->panel_count > MAX_DOCK_PANELS) return 0;
    if (n->active_tab < 0 || n->active_tab >= n->panel_count) return 0;
    for (i = 0; i < n->panel_count; i++) {
        int panel = (int)n->panels[i];
        if (panel < 0 || panel >= PANEL_COUNT) return 0;
        if (panel_seen[panel]) return 0;
        panel_seen[panel] = 1;
    }
    return 1;
}

static int editor_validate_dock_layout(const dock_state *d, int root_node) {
    uint8_t visited[MAX_DOCK_NODES];
    uint8_t panel_seen[PANEL_COUNT];
    memset(visited, 0, sizeof(visited));
    memset(panel_seen, 0, sizeof(panel_seen));
    return editor_validate_dock_node_recursive(d, root_node, visited, panel_seen);
}

static int editor_find_tabs_leaf_with_room(const dock_state *d, int node_idx) {
    const DockNode *n;
    int left;
    if (!d || node_idx < 0 || node_idx >= MAX_DOCK_NODES) return -1;
    n = &d->nodes[node_idx];
    if (!n->in_use) return -1;
    if (n->type == DOCK_TABS) {
        if (n->panel_count < MAX_TABS_PER_NODE) return node_idx;
        return -1;
    }
    if (n->type != DOCK_SPLIT_H && n->type != DOCK_SPLIT_V) return -1;
    left = editor_find_tabs_leaf_with_room(d, n->children[0]);
    if (left >= 0) return left;
    return editor_find_tabs_leaf_with_room(d, n->children[1]);
}

static void editor_log_error_value(const char *context, error_value err) {
    if (!context) context = "editor";
    if (ERRV_IS_OK(err)) {
        fprintf(stderr, "%s failed with an unspecified error\n", context);
        return;
    }
    fprintf(stderr,
            "%s failed (%d): %s [%s:%d]\n",
            context,
            err.code,
            err.message ? err.message : "(no message)",
            err.file ? err.file : "(unknown)",
            err.line);
}

static void editor_ensure_required_panels(dock_state *d) {
    static const PanelId required_panels[] = {
        PANEL_GAME,
        PANEL_EDITOR,
        PANEL_PROFILER,
        PANEL_SCENE_TREE,
        PANEL_INSPECTOR,
        PANEL_ASSETS,
        PANEL_CACHE_PROFILER,
        PANEL_CPU_PROFILER
    };
    int i;

    if (!d || !d->windows[0].in_use || d->windows[0].root_node < 0) return;

    for (i = 0; i < (int)(sizeof(required_panels) / sizeof(required_panels[0])); i++) {
        int target_leaf;
        if (dock_find_leaf_for_panel_global(d, required_panels[i], NULL) >= 0) continue;
        target_leaf = editor_find_tabs_leaf_with_room(d, d->windows[0].root_node);
        if (target_leaf >= 0) {
            error_value err = ERRV_OK;
            if (dock_add_panel_with_error(d, target_leaf, required_panels[i], &err) < 0) {
                editor_log_error_value("editor_ensure_required_panels:dock_add_panel", err);
            }
        }
    }
}

static int editor_save_dock_layout_to_toml(const dock_state *d, const char *path) {
    FILE *fp;
    int i, j;
    if (!d || !path || !path[0]) return 0;
    if (!d->windows[0].in_use || d->windows[0].root_node < 0) return 0;

    fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "editor layout save failed for '%s': %s\n", path, strerror(errno));
        return 0;
    }

    fprintf(fp, "# Saved by Anitra editor\n\n");
    fprintf(fp, "[meta]\n");
    fprintf(fp, "version = 1\n");
    fprintf(fp, "main_root = %d\n\n", d->windows[0].root_node);

    fprintf(fp, "[window0]\n");
    fprintf(fp, "in_use = %s\n", d->windows[0].in_use ? "true" : "false");
    fprintf(fp, "root_node = %d\n\n", d->windows[0].root_node);

    for (i = 0; i < MAX_DOCK_NODES; i++) {
        const DockNode *n = &d->nodes[i];
        if (!n->in_use) continue;

        fprintf(fp, "[node%d]\n", i);
        fprintf(fp, "in_use = true\n");
        fprintf(fp, "type = %d\n", (int)n->type);
        fprintf(fp, "child0 = %d\n", n->children[0]);
        fprintf(fp, "child1 = %d\n", n->children[1]);
        fprintf(fp, "ratio = %.6f\n", (double)n->ratio);
        fprintf(fp, "panel_count = %d\n", n->panel_count);
        fprintf(fp, "active_tab = %d\n", n->active_tab);
        fprintf(fp, "panels = [");
        for (j = 0; j < n->panel_count && j < MAX_DOCK_PANELS; j++) {
            fprintf(fp, "%s%d", (j == 0) ? "" : ", ", (int)n->panels[j]);
        }
        fprintf(fp, "]\n\n");
    }

    if (fclose(fp) != 0) {
        fprintf(stderr, "editor layout save failed closing '%s': %s\n", path, strerror(errno));
        return 0;
    }
    return 1;
}

static int editor_load_dock_layout_from_toml(dock_state *d, const char *path) {
    enum {
        LAYOUT_SECTION_NONE = 0,
        LAYOUT_SECTION_META,
        LAYOUT_SECTION_WINDOW,
        LAYOUT_SECTION_NODE
    };
    FILE *fp;
    dock_state parsed;
    char line[1024];
    int section;
    int section_index;
    int i;
    int main_root;
    int window0_root;
    void *main_window;

    if (!d || !path || !path[0]) return 0;
    fp = fopen(path, "rb");
    if (!fp) return 0;

    memset(&parsed, 0, sizeof(parsed));
    for (i = 0; i < MAX_DOCK_NODES; i++) {
        parsed.nodes[i].children[0] = -1;
        parsed.nodes[i].children[1] = -1;
        parsed.nodes[i].type = DOCK_TABS;
        parsed.nodes[i].ratio = 0.5f;
    }
    for (i = 0; i < MAX_DOCK_WINDOWS; i++) {
        parsed.windows[i].root_node = -1;
    }

    section = LAYOUT_SECTION_NONE;
    section_index = -1;
    main_root = -1;
    window0_root = -1;

    while (fgets(line, (int)sizeof(line), fp)) {
        char *cursor = editor_trim_whitespace(line);
        char *eq;
        char *key;
        char *value;

        if (!cursor[0] || cursor[0] == '#') continue;

        if (cursor[0] == '[') {
            int parsed_index = -1;
            if (strcmp(cursor, "[meta]") == 0) {
                section = LAYOUT_SECTION_META;
                section_index = -1;
                continue;
            }
            if (sscanf(cursor, "[window%d]", &parsed_index) == 1) {
                section = LAYOUT_SECTION_WINDOW;
                section_index = parsed_index;
                continue;
            }
            if (sscanf(cursor, "[node%d]", &parsed_index) == 1) {
                section = LAYOUT_SECTION_NODE;
                section_index = parsed_index;
                continue;
            }
            section = LAYOUT_SECTION_NONE;
            section_index = -1;
            continue;
        }

        eq = strchr(cursor, '=');
        if (!eq) continue;
        *eq = '\0';
        key = editor_trim_whitespace(cursor);
        value = editor_trim_whitespace(eq + 1);
        if (!key[0]) continue;

        if (section == LAYOUT_SECTION_META) {
            if (strcmp(key, "main_root") == 0) {
                main_root = atoi(value);
            }
        } else if (section == LAYOUT_SECTION_WINDOW) {
            if (section_index == 0) {
                if (strcmp(key, "in_use") == 0) {
                    int flag = 0;
                    if (editor_parse_bool_value(value, &flag)) {
                        parsed.windows[0].in_use = flag;
                    }
                } else if (strcmp(key, "root_node") == 0) {
                    window0_root = atoi(value);
                    parsed.windows[0].root_node = window0_root;
                }
            }
        } else if (section == LAYOUT_SECTION_NODE) {
            DockNode *n;
            if (section_index < 0 || section_index >= MAX_DOCK_NODES) continue;
            n = &parsed.nodes[section_index];

            if (strcmp(key, "in_use") == 0) {
                int flag = 0;
                if (editor_parse_bool_value(value, &flag)) {
                    n->in_use = flag;
                }
            } else if (strcmp(key, "type") == 0) {
                n->type = (DockNodeType)atoi(value);
            } else if (strcmp(key, "child0") == 0) {
                n->children[0] = atoi(value);
            } else if (strcmp(key, "child1") == 0) {
                n->children[1] = atoi(value);
            } else if (strcmp(key, "ratio") == 0) {
                n->ratio = strtof(value, NULL);
            } else if (strcmp(key, "panel_count") == 0) {
                n->panel_count = atoi(value);
            } else if (strcmp(key, "active_tab") == 0) {
                n->active_tab = atoi(value);
            } else if (strcmp(key, "panels") == 0) {
                int panels[MAX_DOCK_PANELS];
                int panel_count = editor_parse_int_list(value, panels, MAX_DOCK_PANELS);
                int pi;
                if (panel_count > 0) {
                    n->panel_count = panel_count;
                    for (pi = 0; pi < panel_count; pi++) {
                        n->panels[pi] = (PanelId)panels[pi];
                    }
                }
            }
        }
    }
    fclose(fp);

    if (main_root < 0) main_root = window0_root;
    if (main_root < 0) return 0;

    for (i = 0; i < MAX_DOCK_NODES; i++) {
        DockNode *n = &parsed.nodes[i];
        if (!n->in_use) continue;
        if (n->type != DOCK_SPLIT_H && n->type != DOCK_SPLIT_V && n->type != DOCK_TABS) {
            return 0;
        }
        if ((n->type == DOCK_SPLIT_H || n->type == DOCK_SPLIT_V) &&
            (n->ratio <= 0.001f || n->ratio >= 0.999f)) {
            n->ratio = 0.5f;
        }
        if (n->type == DOCK_TABS) {
            if (n->panel_count < 0) n->panel_count = 0;
            if (n->panel_count > MAX_DOCK_PANELS) n->panel_count = MAX_DOCK_PANELS;
            if (n->panel_count > 0) {
                if (n->active_tab < 0) n->active_tab = 0;
                if (n->active_tab >= n->panel_count) n->active_tab = n->panel_count - 1;
            } else {
                n->active_tab = 0;
            }
        }
    }

    if (!editor_validate_dock_layout(&parsed, main_root)) return 0;

    main_window = d->windows[0].sdl_window;
    memset(d, 0, sizeof(*d));
    memcpy(d->nodes, parsed.nodes, sizeof(parsed.nodes));
    d->windows[0].in_use = 1;
    d->windows[0].root_node = main_root;
    d->windows[0].sdl_window = main_window;
    d->initialized = 1;

    editor_ensure_required_panels(d);
    return 1;
}

static int editor_refresh_layout_path(const game_state *gs, editor_state *e) {
    if (!gs || !e) return 0;
    if (!gs->project_loaded || !gs->project_path[0]) {
        e->editor_layout_path[0] = '\0';
        return 0;
    }
    if (!editor_build_layout_state_path(gs->project_path,
                                        e->editor_layout_path,
                                        (int)sizeof(e->editor_layout_path))) {
        e->editor_layout_path[0] = '\0';
        return 0;
    }
    return 1;
}

static int editor_save_layout_state(const game_state *gs, editor_state *e) {
    if (!gs || !e || !e->dock) return 0;
    if (!editor_refresh_layout_path(gs, e)) return 0;
    return editor_save_dock_layout_to_toml((dock_state *)e->dock, e->editor_layout_path);
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

static void toml_write_assets_from_unified(FILE *fp, const char *section_name,
                                            const project_asset *assets, int count,
                                            project_asset_type type) {
    int i, found = 0;
    if (!fp || !section_name || count <= 0) return;
    for (i = 0; i < count; i++)
        if (assets[i].type == type) { found = 1; break; }
    if (!found) return;
    fprintf(fp, "[assets.%s]\n", section_name);
    for (i = 0; i < count; i++) {
        if (assets[i].type != type) continue;
        if (!assets[i].key[0] || !assets[i].path[0]) continue;
        fprintf(fp, "%-12s = ", assets[i].key);
        toml_write_escaped_string(fp, assets[i].path);
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
    {
        const project_mesh *pm = project_find_mesh(project, entity_index);
        if (pm && pm->model[0])
            return pm->model;
    }

    mc = find_mesh_component_read(gs, entity_index);
    if (!mc) return NULL;
    asset = find_scene_model_asset_read(gs, mc->model_asset_index);
    if (!asset) return NULL;

    if (asset->key[0] && !string_starts_with(asset->key, "asset_")) {
        return asset->key;
    }

    for (i = 0; i < project->asset_count; i++) {
        if ((project->assets[i].type == ASSET_MODEL || project->assets[i].type == ASSET_DUNGEON_PIECE) &&
            strcmp(project->assets[i].path, asset->path) == 0) {
            return project->assets[i].key;
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
    {
        const project_anim *pa = project_find_anim(project, entity_index);
        if (pa && pa->asset[0])
            return pa->asset;
    }

    mc = find_mesh_component_read(gs, entity_index);
    if (mc) {
        asset = find_scene_model_asset_read(gs, mc->model_asset_index);
        if (asset && asset->animation_path[0]) {
            for (i = 0; i < project->asset_count; i++) {
                if (project->assets[i].type == ASSET_ANIMATION &&
                    strcmp(project->assets[i].path, asset->animation_path) == 0) {
                    return project->assets[i].key;
                }
            }
        }
    }

    /* Fallback: first animation asset */
    for (i = 0; i < project->asset_count; i++) {
        if (project->assets[i].type == ASSET_ANIMATION && project->assets[i].key[0])
            return project->assets[i].key;
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
    if (count > PROJECT_COMP_MAX) count = PROJECT_COMP_MAX;
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

    toml_write_assets_from_unified(fp, "models", project->assets, project->asset_count, ASSET_MODEL);
    toml_write_assets_from_unified(fp, "animations", project->assets, project->asset_count, ASSET_ANIMATION);
    toml_write_assets_from_unified(fp, "dungeon_pieces", project->assets, project->asset_count, ASSET_DUNGEON_PIECE);
    toml_write_assets_from_unified(fp, "sprites", project->assets, project->asset_count, ASSET_SPRITE);

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
            Vec3 vel = VEC3(0,0,0);
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
            {
                const project_box_collider *pbc = project_find_box_collider(project, i);
                if (pbc && pbc->half_extents[1] > 0.0f)
                    hy = pbc->half_extents[1];
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

    count = 0;
    for (i = 0; i < scene_count; i++) if (has_trigger_component(gs, i, NULL, NULL, NULL, NULL)) count++;
    if (count > 0) {
        fprintf(fp, "[triggers]\n");
        for (i = 0; i < scene_count; i++) {
            trigger_type type = TRIGGER_DOOR;
            int target_entity = 0;
            float radius = 1.0f;
            const char *type_str;
            if (!has_trigger_component(gs, i, &type, &target_entity, &radius, NULL)) continue;
            type_str = (type == TRIGGER_PICKUP) ? "pickup" : "door";
            fprintf(fp, "\"%d\" = { type = ", i);
            toml_write_escaped_string(fp, type_str);
            fprintf(fp, ", target = %d, radius = %.4f }\n", target_entity, radius);
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
        gs->parent_transform_components[write_i] = pt;
        gs->parent_transform_index[pt.entity_index] = write_i;
        write_i++;
    }
    gs->parent_transform_index[entity_index] = -1;
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
    gs->parent_transform_index[entity_index] = i;
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

static int scene_tree_entity_has_children(int entity_index,
                                          const int *parent_of, int entity_count) {
    int child;
    if (!parent_of) return 0;
    if (entity_index < 0 || entity_index >= entity_count) return 0;
    for (child = 0; child < entity_count; child++) {
        if (child == entity_index) continue;
        if (parent_of[child] == entity_index) return 1;
    }
    return 0;
}

static void scene_tree_mark_descendants_visited(int entity_index,
                                                const int *parent_of, int entity_count,
                                                int *visited) {
    int child;
    if (!parent_of || !visited) return;
    if (entity_index < 0 || entity_index >= entity_count) return;

    for (child = 0; child < entity_count; child++) {
        if (child == entity_index) continue;
        if (parent_of[child] != entity_index) continue;
        if (visited[child]) continue;
        visited[child] = 1;
        scene_tree_mark_descendants_visited(child, parent_of, entity_count, visited);
    }
}

static int editor_scene_entity_selected(const editor_state *e, int entity_index) {
    if (!e) return 0;
    if (entity_index < 0 || entity_index >= SCENE_TREE_MAX_ENTITIES) return 0;
    return e->scene_selection_mask[entity_index] ? 1 : 0;
}

static void editor_scene_selection_clear(editor_state *e) {
    if (!e) return;
    memset(e->scene_selection_mask, 0, sizeof(e->scene_selection_mask));
    e->scene_selection_count = 0;
    e->scene_selected_entity = -1;
}

static void editor_scene_selection_set_single(editor_state *e, int entity_index) {
    if (!e) return;
    editor_scene_selection_clear(e);
    if (entity_index < 0 || entity_index >= SCENE_TREE_MAX_ENTITIES) return;
    e->scene_selection_mask[entity_index] = 1;
    e->scene_selection_count = 1;
    e->scene_selected_entity = entity_index;
}

static void editor_scene_selection_toggle(editor_state *e, int entity_index) {
    int i;
    if (!e) return;
    if (entity_index < 0 || entity_index >= SCENE_TREE_MAX_ENTITIES) return;

    if (e->scene_selection_mask[entity_index]) {
        e->scene_selection_mask[entity_index] = 0;
        if (e->scene_selection_count > 0) e->scene_selection_count--;
        if (e->scene_selected_entity == entity_index) {
            e->scene_selected_entity = -1;
            for (i = 0; i < SCENE_TREE_MAX_ENTITIES; i++) {
                if (e->scene_selection_mask[i]) {
                    e->scene_selected_entity = i;
                    break;
                }
            }
        }
    } else {
        e->scene_selection_mask[entity_index] = 1;
        e->scene_selection_count++;
        e->scene_selected_entity = entity_index;
    }
}

static void editor_scene_selection_apply_click(editor_state *e, int entity_index, int shift_down) {
    if (!e) return;
    if (shift_down) {
        editor_scene_selection_toggle(e, entity_index);
    } else {
        editor_scene_selection_set_single(e, entity_index);
    }
}

static void editor_scene_selection_sync(editor_state *e, int scene_count) {
    int i;
    int first_selected = -1;
    int count = 0;
    if (!e) return;

    if (scene_count < 0) scene_count = 0;
    if (scene_count > SCENE_TREE_MAX_ENTITIES) scene_count = SCENE_TREE_MAX_ENTITIES;

    for (i = 0; i < SCENE_TREE_MAX_ENTITIES; i++) {
        int keep = (i < scene_count) && e->scene_selection_mask[i];
        e->scene_selection_mask[i] = keep ? 1 : 0;
        if (keep) {
            if (first_selected < 0) first_selected = i;
            count++;
        }
    }

    if (e->scene_selected_entity >= 0 && e->scene_selected_entity < scene_count) {
        if (!e->scene_selection_mask[e->scene_selected_entity]) {
            e->scene_selection_mask[e->scene_selected_entity] = 1;
            count++;
            if (first_selected < 0) first_selected = e->scene_selected_entity;
        }
    } else {
        e->scene_selected_entity = -1;
    }

    if (e->scene_selected_entity < 0) e->scene_selected_entity = first_selected;
    e->scene_selection_count = count;
}

static void scene_tree_cancel_drag(editor_state *e) {
    if (!e) return;
    e->scene_tree_drag_active = 0;
    e->scene_tree_drag_entity = -1;
    e->scene_tree_drop_target = -1;
    e->scene_tree_drop_mode = SCENE_TREE_DROP_NONE;
}

static void scene_tree_begin_drag(editor_state *e, int entity_index) {
    if (!e) return;
    e->scene_tree_drag_active = 1;
    e->scene_tree_drag_entity = entity_index;
    e->scene_tree_drop_target = -1;
    e->scene_tree_drop_mode = SCENE_TREE_DROP_NONE;
}

static void scene_tree_emit_entity_row(const game_state *gs, editor_state *e,
                                       int entity_index, int depth, int click,
                                       int shift_toggle,
                                       const int *parent_of, int entity_count) {
    static char row_bufs[SCENE_TREE_MAX_ENTITIES][64];
    int bi = entity_index % SCENE_TREE_MAX_ENTITIES;
    int selected = editor_scene_entity_selected(e, entity_index);
    int dragging = e->scene_tree_drag_active && e->scene_tree_mouse_down;
    int sibling_parent = -1;
    int nested_valid = scene_tree_parent_assignment_is_valid(e->scene_tree_drag_entity, entity_index,
                                                              parent_of, entity_count);
    int sibling_valid;
    const char *name = NULL;
    int has_children = scene_tree_entity_has_children(entity_index, parent_of, entity_count);
    int collapsed = 0;
    const char *disclosure_label = " ";

    if (entity_index >= 0 && entity_index < SCENE_TREE_MAX_ENTITIES) {
        collapsed = e->scene_tree_collapsed[entity_index] ? 1 : 0;
    }
    if (has_children) disclosure_label = collapsed ? UI_ICON_ARROW_RIGHT : UI_ICON_ARROW_DOWN;

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
                editor_scene_selection_apply_click(e, entity_index, shift_toggle);
                if (shift_toggle) {
                    scene_tree_cancel_drag(e);
                } else {
                    scene_tree_begin_drag(e, entity_index);
                }
            }
            if (hovered && dragging && entity_index != e->scene_tree_drag_entity) {
                e->scene_tree_drop_target = entity_index;
                e->scene_tree_drop_mode = SCENE_TREE_DROP_SIBLING_BEFORE;
            }
        }

        CLAY(CLAY_IDI("STEntityRowBody", (int32_t)entity_index), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIT({0}) },
                .padding = {
                    .left = (uint16_t)(UI_ROW_PADDING_X + depth * UI_TREE_INDENT),
                    .right = UI_ROW_PADDING_X,
                    .top = UI_ROW_PADDING_Y,
                    .bottom = UI_ROW_PADDING_Y
                },
                .childGap = UI_SPACE_XS,
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childAlignment = { CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER }
            },
            .backgroundColor = (dragging && entity_index != e->scene_tree_drag_entity && Clay_Hovered())
                                   ? (nested_valid ? ((Clay_Color){64, 94, 136, 255})
                                                   : ((Clay_Color){120, 60, 60, 255}))
                                   : ((dragging && entity_index == e->scene_tree_drag_entity)
                                          ? ((Clay_Color){74, 74, 74, 255})
                                          : (selected ? ((Clay_Color){52, 62, 84, 255})
                                                      : (Clay_Hovered() ? ((Clay_Color){48, 56, 72, 255})
                                                                        : ((Clay_Color){0, 0, 0, 0})))),
            .cornerRadius = CLAY_CORNER_RADIUS(UI_RADIUS_MD)
        }) {
            int hovered = Clay_Hovered();
            int disclosure_clicked = 0;
            CLAY(CLAY_IDI("STEntityDisclosure", (int32_t)entity_index), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(10), CLAY_SIZING_FIT({0}) }
                }
            }) {
                Clay_String ds = {false, (int32_t)strlen(disclosure_label), (char *)disclosure_label};
                Clay_Color dcolor = has_children
                    ? ((Clay_Color){188, 198, 216, 255})
                    : ((Clay_Color){108, 116, 132, 255});
                CLAY_TEXT(ds, CLAY_TEXT_CONFIG({.textColor = dcolor, .fontSize = UI_FONT_BODY}));
                if (has_children && click && Clay_Hovered()) {
                    if (entity_index >= 0 && entity_index < SCENE_TREE_MAX_ENTITIES) {
                        e->scene_tree_collapsed[entity_index] = collapsed ? 0 : 1;
                    }
                    disclosure_clicked = 1;
                }
            }

            if (hovered && click && !disclosure_clicked) {
                editor_scene_selection_apply_click(e, entity_index, shift_toggle);
                if (shift_toggle) {
                    scene_tree_cancel_drag(e);
                } else {
                    scene_tree_begin_drag(e, entity_index);
                }
            }
            if (hovered && dragging && entity_index != e->scene_tree_drag_entity) {
                e->scene_tree_drop_target = entity_index;
                e->scene_tree_drop_mode = SCENE_TREE_DROP_NESTED;
            }

            Clay_String cs = {false, (int32_t)strlen(row_bufs[bi]), row_bufs[bi]};
            CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {224, 230, 240, 255}, .fontSize = UI_FONT_BODY}));
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
                editor_scene_selection_apply_click(e, entity_index, shift_toggle);
                if (shift_toggle) {
                    scene_tree_cancel_drag(e);
                } else {
                    scene_tree_begin_drag(e, entity_index);
                }
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
                                             int shift_toggle,
                                             const int *parent_of, int entity_count,
                                             int *visited) {
    int child;
    int collapsed = 0;
    if (entity_index < 0 || entity_index >= entity_count) return;
    if (visited[entity_index]) return;

    visited[entity_index] = 1;
    scene_tree_emit_entity_row(gs, e, entity_index, depth, click, shift_toggle,
                               parent_of, entity_count);

    if (entity_index >= 0 && entity_index < SCENE_TREE_MAX_ENTITIES) {
        collapsed = e->scene_tree_collapsed[entity_index] ? 1 : 0;
    }
    if (collapsed) {
        scene_tree_mark_descendants_visited(entity_index, parent_of, entity_count, visited);
        return;
    }

    for (child = 0; child < entity_count; child++) {
        if (parent_of[child] == entity_index && child != entity_index) {
            scene_tree_emit_entity_recursive(gs, e, child, depth + 1, click,
                                             shift_toggle,
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

static Vec3 cam_forward(const editor_state *e) {
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

static Mat4 editor_world_matrix_for_entity(const game_state *gs, int entity_index);
static Vec3 editor_transform_point(Mat4 m, Vec3 p);

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

static int editor_build_pick_ray(const editor_state *e, float panel_x, float panel_y,
                                 Vec3 *out_origin, Vec3 *out_dir) {
    float w, h;
    float nx, ny;
    float aspect;
    Vec3 fwd, right, up;
    if (!e || !out_origin || !out_dir) return 0;

    w = e->panel_w;
    h = e->panel_h;
    if (w < 1.0f || h < 1.0f) return 0;

    nx = (panel_x / w) * 2.0f - 1.0f;
    ny = 1.0f - (panel_y / h) * 2.0f;

    fwd = vec3_normalize(cam_forward(e));
    right = vec3_normalize(vec3_cross(fwd, VEC3(0.0f, 1.0f, 0.0f)));
    if (vec3_len(right) < 1e-4f) right = VEC3(1.0f, 0.0f, 0.0f);
    up = vec3_normalize(vec3_cross(right, fwd));
    if (vec3_len(up) < 1e-4f) up = VEC3(0.0f, 1.0f, 0.0f);

    aspect = w / h;
    if (e->cam_projection_mode == EDITOR_CAMERA_ORTHOGRAPHIC) {
        float ortho_size = editor_ortho_size(e);
        float half_h = ortho_size * 0.5f;
        float half_w = half_h * aspect;
        *out_origin = vec3_add(e->cam_pos,
                               vec3_add(vec3_scale(right, nx * half_w),
                                        vec3_scale(up, ny * half_h)));
        *out_dir = fwd;
    } else {
        float tan_half_fov = tanf(60.0f * 3.14159265f / 180.0f * 0.5f);
        Vec3 dir = vec3_add(fwd,
                            vec3_add(vec3_scale(right, nx * tan_half_fov * aspect),
                                     vec3_scale(up, ny * tan_half_fov)));
        *out_origin = e->cam_pos;
        *out_dir = vec3_normalize(dir);
    }
    return 1;
}

static int ray_sphere_hit(Vec3 ro, Vec3 rd, Vec3 center, float radius, float *out_t) {
    Vec3 oc = vec3_sub(ro, center);
    float a = vec3_dot(rd, rd);
    float b = 2.0f * vec3_dot(oc, rd);
    float c = vec3_dot(oc, oc) - radius * radius;
    float disc = b * b - 4.0f * a * c;
    float t0, t1, t;
    if (disc < 0.0f) return 0;

    disc = sqrtf(disc);
    t0 = (-b - disc) / (2.0f * a);
    t1 = (-b + disc) / (2.0f * a);
    t = (t0 > 0.0f) ? t0 : ((t1 > 0.0f) ? t1 : -1.0f);
    if (t <= 0.0f) return 0;
    if (out_t) *out_t = t;
    return 1;
}

static float editor_pick_projected_radius_px(const editor_state *e, Vec3 center_world,
                                             float radius_world) {
    const float fov_rad = 60.0f * 3.14159265f / 180.0f;
    if (!e || radius_world <= 0.0f || e->panel_h < 1.0f) return 0.0f;

    if (e->cam_projection_mode == EDITOR_CAMERA_ORTHOGRAPHIC) {
        float ortho_size = editor_ortho_size(e);
        if (ortho_size < 0.001f) return 0.0f;
        return radius_world * (e->panel_h / ortho_size);
    } else {
        Vec3 to_center = vec3_sub(center_world, e->cam_pos);
        Vec3 fwd = vec3_normalize(cam_forward(e));
        float depth = vec3_dot(to_center, fwd);
        float focal_px;
        if (depth < 0.05f) return 0.0f;
        focal_px = (e->panel_h * 0.5f) / tanf(fov_rad * 0.5f);
        return (radius_world / depth) * focal_px;
    }
}

static int editor_pick_click_through_allows(const editor_state *e, Vec3 center_world,
                                            float radius_world) {
    float radius_px;
    float cam_dist;
    if (!e) return 0;
    radius_px = editor_pick_projected_radius_px(e, center_world, radius_world);
    cam_dist = vec3_len(vec3_sub(center_world, e->cam_pos));
    if (radius_px < EDITOR_PICK_MIN_SCREEN_RADIUS_PX &&
        cam_dist > EDITOR_PICK_TINY_MAX_DISTANCE) {
        return 0;
    }
    return 1;
}

static int editor_pick_entity_sphere(const game_state *gs, int entity_index,
                                     Vec3 *out_center_world, float *out_radius_world) {
    const mesh_component *mc;
    const scene_model_asset *asset = NULL;
    const box_collider_component *boxc = NULL;
    const capsule_collider_component *capc = NULL;
    Mat4 world;
    Vec3 center_local = VEC3(0.0f, 0.0f, 0.0f);
    Vec3 center_world;
    Vec3 axis_x, axis_y, axis_z;
    float sx, sy, sz, max_scale;
    float radius_local = 0.0f;
    float radius_world;
    int has_pick_volume = 0;

    if (!gs || !out_center_world || !out_radius_world) return 0;

    mc = find_mesh_component_read(gs, entity_index);
    if (mc && mc->visible) {
        asset = find_scene_model_asset_read(gs, mc->model_asset_index);
        if (asset && asset->loaded && asset->model.mesh.primitive_count > 0) {
            center_local = VEC3(asset->model.mesh.bounds_center[0],
                                asset->model.mesh.bounds_center[1],
                                asset->model.mesh.bounds_center[2]);
            radius_local = asset->model.mesh.bounds_radius > 0.01f
                               ? asset->model.mesh.bounds_radius
                               : 0.75f;
            has_pick_volume = 1;
        }
    }

    if (!has_pick_volume) {
        capc = find_capsule_collider_component_read(gs, entity_index);
        if (capc) {
            float r = fabsf(capc->radius);
            float hh = fabsf(capc->half_height);
            radius_local = r + hh;
            if (radius_local < 0.1f) radius_local = 0.1f;
            has_pick_volume = 1;
        } else {
            boxc = find_box_collider_component_read(gs, entity_index);
            if (boxc) {
                float hx = fabsf(boxc->rect.w) * 0.5f;
                float hz = fabsf(boxc->rect.h) * 0.5f;
                float hy = fabsf(boxc->half_height);
                if (hx < 0.01f && hz < 0.01f) {
                    hx = 0.5f;
                    hz = 0.5f;
                }
                if (hy < 0.01f) hy = fmaxf(0.5f, fmaxf(hx, hz));
                radius_local = sqrtf(hx * hx + hy * hy + hz * hz);
                if (radius_local < 0.1f) radius_local = 0.1f;
                has_pick_volume = 1;
            }
        }
    }

    if (!has_pick_volume) return 0;

    world = editor_world_matrix_for_entity(gs, entity_index);
    center_world = editor_transform_point(world, center_local);

    axis_x = VEC3(world.m[0], world.m[1], world.m[2]);
    axis_y = VEC3(world.m[4], world.m[5], world.m[6]);
    axis_z = VEC3(world.m[8], world.m[9], world.m[10]);
    sx = vec3_len(axis_x);
    sy = vec3_len(axis_y);
    sz = vec3_len(axis_z);
    max_scale = fmaxf(sx, fmaxf(sy, sz));
    if (max_scale < 1e-4f) max_scale = 1.0f;

    radius_world = radius_local * max_scale;
    if (radius_world < 0.05f) radius_world = 0.05f;

    *out_center_world = center_world;
    *out_radius_world = radius_world;
    return 1;
}

static int editor_pick_scene_entity(const game_state *gs, const editor_state *e,
                                    float panel_x, float panel_y, int *out_entity) {
    int i;
    int entity_count;
    int best_entity = -1;
    float best_t = 1e30f;
    Vec3 ray_origin, ray_dir;
    if (!gs || !e || !out_entity || !gs->scene_entities) return 0;
    if (!editor_build_pick_ray(e, panel_x, panel_y, &ray_origin, &ray_dir)) return 0;
    entity_count = gs->scene_entity_count;
    if (entity_count > SCENE_TREE_MAX_ENTITIES) entity_count = SCENE_TREE_MAX_ENTITIES;

    for (i = 0; i < entity_count; i++) {
        Vec3 center_world;
        float radius_world;
        float hit_t;

        if (!editor_pick_entity_sphere(gs, i, &center_world, &radius_world)) continue;
        if (!ray_sphere_hit(ray_origin, ray_dir, center_world, radius_world, &hit_t)) continue;
        if (!editor_pick_click_through_allows(e, center_world, radius_world)) continue;
        if (hit_t < best_t) {
            best_t = hit_t;
            best_entity = i;
        }
    }

    if (best_entity < 0) return 0;
    *out_entity = best_entity;
    return 1;
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

static void editor_activate_panel_tab(editor_state *e, PanelId panel) {
    dock_state *d;
    int win_idx = -1;
    int node;
    int i;
    if (!e || !e->dock) return;
    d = (dock_state *)e->dock;
    node = dock_find_leaf_for_panel_global(d, panel, &win_idx);
    if (node < 0) return;

    for (i = 0; i < d->nodes[node].panel_count; i++) {
        if (d->nodes[node].panels[i] == panel) {
            dock_set_active_tab(d, node, i);
            break;
        }
    }

    if (win_idx >= 0 && win_idx < MAX_DOCK_WINDOWS && d->windows[win_idx].sdl_window) {
        SDL_RaiseWindow((SDL_Window *)d->windows[win_idx].sdl_window);
    }
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
        editor_activate_panel_tab(e, PANEL_EDITOR);
        return;
    }

    gs->mesh3d.anim_time = 0.0f;
    editor_activate_panel_tab(e, PANEL_GAME);
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
    dock_state *d;

    /* ── Migration check ──────────────────────────────────────────── */
    if (e->mig_hdr && es->editor_arena) {
        uint64_t old_hash = e->mig_hdr->layout_hash;
        uint64_t new_hash = mig_compute_hash(mig_editor_state_fields,
                                              MIG_editor_state_COUNT);
        if (old_hash != new_hash) {
            uint32_t old_size = e->mig_hdr->struct_size;
            uint32_t copy_size = old_size < sizeof(editor_state)
                               ? old_size : (uint32_t)sizeof(editor_state);
            void *old_copy = arena_alloc(es->editor_arena, copy_size, 8,
                                          "mig_editor_old");
            if (old_copy) {
                mig_header *old_hdr = e->mig_hdr;
                struct arena *saved_root = e->root_arena;
                struct arena *saved_editor = e->editor_arena;
                memcpy(old_copy, e, copy_size);
                memset(e, 0, sizeof(editor_state));
                mig_migrate_struct(e, mig_editor_state_fields,
                                   MIG_editor_state_COUNT,
                                   (uint32_t)sizeof(editor_state),
                                   old_copy, old_hdr);
                /* Restore arena pointers (critical, may not migrate correctly) */
                e->root_arena = saved_root;
                e->editor_arena = saved_editor;
                e->initialized = 0; /* force re-init for defaults */
                e->mig_hdr = NULL;  /* will be re-stored below */
                fprintf(stderr, "Migrated editor_state (%u -> %u bytes)\n",
                        old_size, (uint32_t)sizeof(editor_state));
            }
        }
    }

    d = (dock_state *)e->dock;

    /* ── Store current field table ────────────────────────────────── */
    if (!e->mig_hdr && es->editor_arena) {
        e->mig_hdr = (mig_header *)mig_store(
            es->editor_arena, mig_editor_state_fields,
            MIG_editor_state_COUNT, (uint32_t)sizeof(editor_state),
            mig_compute_hash(mig_editor_state_fields, MIG_editor_state_COUNT),
            "mig_editor_state");
    }

    if (e->initialized) return;

    /* ── Defaults (conditional to preserve migrated values) ─────── */
    if (e->cam_pos.x == 0.0f && e->cam_pos.y == 0.0f && e->cam_pos.z == 0.0f)
        e->cam_pos = VEC3(0.0f, 3.0f, 8.0f);
    if (e->cam_yaw == 0.0f)    e->cam_yaw    = 3.14159265f;
    if (e->cam_pitch == 0.0f)  e->cam_pitch  = -0.3f;
    if (e->cam_speed == 0.0f)  e->cam_speed  = 5.0f;
    if (e->cam_sens == 0.0f)   e->cam_sens   = 0.003f;
    e->cam_projection_mode = e->cam_projection_mode; /* 0 = PERSPECTIVE is valid default */
    if (e->cam_ortho_size == 0.0f) e->cam_ortho_size = 12.0f;
    e->cam_mouse_look = 0;
    e->cam_mouse_button = 0;
    if (e->open == 0) e->open = 1;
    e->gizmo_hovered = GIZMO_NONE;
    e->gizmo_active  = GIZMO_NONE;
    if (e->gizmo_entity_index == 0) e->gizmo_entity_index = -1;
    e->gizmo_drag_mode = 0;
    e->gizmo_drag_start_capsule_radius = 0.0f;
    e->gizmo_drag_start_capsule_half_height = 0.0f;
    if (e->gizmo_drag_axis_local_scale == 0.0f) e->gizmo_drag_axis_local_scale = 1.0f;
    e->line_count = 0;
    if (e->menu_open == 0) e->menu_open = -1;
    if (e->menu_hover == 0) e->menu_hover = -1;
    e->scene_selected_entity = 0;
    memset(e->scene_selection_mask, 0, sizeof(e->scene_selection_mask));
    e->scene_selection_count = 0;
    memset(e->scene_tree_collapsed, 0, sizeof(e->scene_tree_collapsed));
    e->scene_tree_drag_active = 0;
    if (e->scene_tree_drag_entity == 0) e->scene_tree_drag_entity = -1;
    if (e->scene_tree_drop_target == 0) e->scene_tree_drop_target = -1;
    e->scene_tree_drop_mode = SCENE_TREE_DROP_NONE;
    e->scene_tree_click_shift = 0;
    if (e->project_browser_mouse_x == 0.0f) e->project_browser_mouse_x = -10000.0f;
    if (e->project_browser_mouse_y == 0.0f) e->project_browser_mouse_y = -10000.0f;
    e->project_browser_scroll_y = 0.0f;
    e->project_browser_mouse_down = 0;
    e->project_browser_click = 0;
    e->pb_selected_path[0] = '\0';
    e->pb_selected_asset_key[0] = '\0';
    e->pb_selected_asset_path[0] = '\0';
    if (e->pb_selected_asset_type == 0) e->pb_selected_asset_type = -1;
    e->pb_thumbnail_count = 0;
    if (e->cpu_prof_flame_zoom == 0.0f) e->cpu_prof_flame_zoom = 1.0f;
    if (e->cpu_prof_flame_center == 0.0f) e->cpu_prof_flame_center = 0.5f;
    e->cpu_prof_flame_zoom_wheel = 0.0f;
    e->cpu_prof_flame_pan_wheel = 0.0f;
    e->cpu_prof_flame_x = 0.0f;
    e->cpu_prof_flame_y = 0.0f;
    e->cpu_prof_flame_w = 0.0f;
    e->cpu_prof_flame_h = 0.0f;
    e->cpu_prof_minimap_dragging = 0;
    if (e->cpu_prof_minimap_drag_offset == 0.0f) e->cpu_prof_minimap_drag_offset = 0.5f;
    e->cpu_prof_text_arena = NULL;
    memset(e->cpu_prof_tree_collapsed, 0, sizeof(e->cpu_prof_tree_collapsed));
    e->cpu_prof_tree_frame_id = 0;
    e->cpu_prof_hover_zone_name[0] = '\0';
    e->cpu_prof_hover_zone_active = 0;
    e->cpu_prof_selected_zone_name[0] = '\0';
    e->cpu_prof_selected_zone_active = 0;
    e->editor_layout_path[0] = '\0';
    e->dock_layout_last_hash = 0;
    e->dock_layout_save_accum = 0.0f;
    e->dock_layout_hash_valid = 0;
    e->initialized = 1;

    if (d) {
        if (editor_refresh_layout_path(gs, e) &&
            editor_load_dock_layout_from_toml(d, e->editor_layout_path)) {
            fprintf(stderr, "Loaded editor layout from %s\n", e->editor_layout_path);
        }
        editor_ensure_required_panels(d);
        e->dock_layout_last_hash = editor_dock_layout_hash(d);
        e->dock_layout_hash_valid = 1;
    }

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

    if (!e->cache_prof_clay_ctx && es->editor_arena) {
        uint32_t clay_size = (uint32_t)Clay_MinMemorySize();
        arena *clay_sub = arena_alloc_subarena(es->editor_arena, clay_size, 16, "clay_cache_prof");
        if (clay_sub) {
            Clay_Arena ca = Clay_CreateArenaWithCapacityAndMemory(clay_size, clay_sub->base);
            Clay_ErrorHandler err = {0};
            e->cache_prof_clay_ctx = Clay_Initialize(ca, (Clay_Dimensions){800, 600}, err);
            Clay_SetCurrentContext((Clay_Context *)e->cache_prof_clay_ctx);
            Clay_SetMeasureTextFunction(profiler_measure_text, e);
        }
    }

    if (!e->cpu_prof_clay_ctx && es->editor_arena) {
        uint32_t clay_size = (uint32_t)Clay_MinMemorySize();
        arena *clay_sub = arena_alloc_subarena(es->editor_arena, clay_size, 16, "clay_cpu_prof");
        if (clay_sub) {
            Clay_Arena ca = Clay_CreateArenaWithCapacityAndMemory(clay_size, clay_sub->base);
            Clay_ErrorHandler err = {0};
            e->cpu_prof_clay_ctx = Clay_Initialize(ca, (Clay_Dimensions){800, 600}, err);
            Clay_SetCurrentContext((Clay_Context *)e->cpu_prof_clay_ctx);
            Clay_SetMeasureTextFunction(profiler_measure_text, e);
        }
    }

    if (!e->cpu_prof_text_arena && es->editor_arena) {
        e->cpu_prof_text_arena = arena_alloc_subarena(es->editor_arena, 16 * 1024, 16, "cpu_prof_text");
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
    const char *cpu_focus_tag;
    int have_cpu_focus;
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
    cpu_focus_tag = NULL;
    if (e->cpu_prof_hover_zone_active && e->cpu_prof_hover_zone_name[0]) {
        cpu_focus_tag = e->cpu_prof_hover_zone_name;
    } else if (e->cpu_prof_selected_zone_active && e->cpu_prof_selected_zone_name[0]) {
        cpu_focus_tag = e->cpu_prof_selected_zone_name;
    }
    have_cpu_focus = (cpu_focus_tag && cpu_focus_tag[0]) ? 1 : 0;

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
                .padding = { .left = 8, .right = 8, .top = 6, .bottom = 10 },
                .childGap = 4,
                .layoutDirection = CLAY_TOP_TO_BOTTOM
            }
        }) {
            Clay_String ts = {false, (int32_t)strlen(title_buf), title_buf};
            CLAY_TEXT(ts, CLAY_TEXT_CONFIG({.textColor = {220, 220, 220, 255}, .fontSize = UI_FONT_BODY}));
            if (have_cpu_focus) {
                static char focus_buf[96];
                snprintf(focus_buf, sizeof(focus_buf), "CPU focus: %s",
                         cpu_focus_tag ? cpu_focus_tag : "");
                {
                    Clay_String fs = {false, (int32_t)strlen(focus_buf), focus_buf};
                    CLAY_TEXT(fs, CLAY_TEXT_CONFIG({.textColor = {176, 204, 245, 255}, .fontSize = UI_FONT_BODY}));
                }
            }
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
                    CLAY_TEXT(hdr, CLAY_TEXT_CONFIG({.textColor = {180, 180, 190, 255}, .fontSize = UI_FONT_BODY}));
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
                                    CLAY_TEXT(rs, CLAY_TEXT_CONFIG({.textColor = {220, 220, 220, 255}, .fontSize = UI_FONT_BODY}));
                                }
                            }
                        }

                        {
                            HoverInfo hi = {0};
                            int row_id = 0;
                            int tree_color = 0;
                            profiler_tree_arena(a, 1, &row_id, 0, &hi,
                                                &tree_color, click, e->profiler_tree_collapsed,
                                                cpu_focus_tag);
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
                    CLAY_TEXT(hdr, CLAY_TEXT_CONFIG({.textColor = {180, 180, 190, 255}, .fontSize = UI_FONT_BODY}));
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
                        CLAY_TEXT(ls, CLAY_TEXT_CONFIG({.textColor = {170, 170, 170, 255}, .fontSize = UI_FONT_BODY}));
                    }
                    CLAY(CLAY_ID("GLFreeBox"), {
                        .layout = { .sizing = { CLAY_SIZING_FIXED(10), CLAY_SIZING_FIXED(10) } },
                        .backgroundColor = {50, 50, 55, 255},
                        .cornerRadius = CLAY_CORNER_RADIUS(1)
                    }) {}
                    {
                        Clay_String ls = CLAY_STRING("= free");
                        CLAY_TEXT(ls, CLAY_TEXT_CONFIG({.textColor = {170, 170, 170, 255}, .fontSize = UI_FONT_BODY}));
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
                                int is_cpu_match = have_cpu_focus && flat[fi].tag &&
                                    cpu_zone_highlights_record_tag(cpu_focus_tag, flat[fi].tag);
                                if (have_hover || have_cpu_focus) {
                                    if (is_hovered || is_cpu_match) {
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

/* ── Folder tree helpers for project browser ────────────────────────── */

#define PB_MAX_FOLDERS 64

static int pb_folder_cmp(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

static int pb_count_depth(const char *path) {
    int d = 0;
    const char *p;
    for (p = path; *p; p++) if (*p == '/') d++;
    return d;
}

static const char *pb_last_component(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? (s + 1) : path;
}

/* Extract directory portion of a file path (everything before last '/') */
static void pb_extract_dir(const char *path, char *out, int max_len) {
    const char *last = strrchr(path, '/');
    int len;
    if (!last) { out[0] = '\0'; return; }
    len = (int)(last - path);
    if (len >= max_len) len = max_len - 1;
    memcpy(out, path, (size_t)len);
    out[len] = '\0';
}

/* Check if folder path is already in the list */
static int pb_has_folder(char folders[][256], int count, const char *path) {
    int i;
    for (i = 0; i < count; i++)
        if (strcmp(folders[i], path) == 0) return 1;
    return 0;
}

/* Add a directory and all its parent directories to the folder list */
static void pb_add_folder_chain(char folders[][256], int *count, const char *dir) {
    char buf[256];
    char *last;
    strncpy(buf, dir, 255);
    buf[255] = '\0';
    while (buf[0] && *count < PB_MAX_FOLDERS) {
        if (!pb_has_folder(folders, *count, buf)) {
            strncpy(folders[*count], buf, 255);
            folders[*count][255] = '\0';
            (*count)++;
        }
        last = strrchr(buf, '/');
        if (last) *last = '\0';
        else break;
    }
}

/* Build sorted folder list from all asset paths. Returns folder count. */
static int pb_build_folder_list(project_data *proj, char folders[][256]) {
    int count = 0;
    int i;
    char dir[256];
    for (i = 0; i < proj->asset_count; i++) {
        pb_extract_dir(proj->assets[i].path, dir, 256);
        if (dir[0]) pb_add_folder_chain(folders, &count, dir);
    }
    qsort(folders, (size_t)count, 256, pb_folder_cmp);
    return count;
}

/* Check if an asset path is under the given folder (or subfolder) */
static int pb_path_matches(const char *asset_path, const char *folder_path) {
    char dir[256];
    int flen;
    if (!folder_path[0]) return 1; /* empty = show all */
    pb_extract_dir(asset_path, dir, 256);
    flen = (int)strlen(folder_path);
    return (strncmp(dir, folder_path, (size_t)flen) == 0 &&
            (dir[flen] == '\0' || dir[flen] == '/'));
}

/* Determine swatch color for an asset type: 0=model 1=anim 2=dungeon 3=sprite */
static Clay_Color pb_asset_type_color(int type) {
    switch (type) {
        case 0: return (Clay_Color){94, 138, 216, 255};
        case 1: return (Clay_Color){198, 138, 84, 255};
        case 2: return (Clay_Color){116, 172, 118, 255};
        case 3: return (Clay_Color){166, 132, 204, 255};
        default: return (Clay_Color){160, 170, 190, 255};
    }
}

static const char *pb_asset_type_name(int type) {
    switch (type) {
        case 0: return "Model";
        case 1: return "Animation";
        case 2: return "Dungeon Piece";
        case 3: return "Sprite";
        default: return "Unknown";
    }
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
            .padding = CLAY_PADDING_ALL(UI_SECTION_PADDING),
            .childGap = UI_SECTION_GAP,
            .childAlignment = { CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER },
            .layoutDirection = CLAY_LEFT_TO_RIGHT
        },
        .backgroundColor = Clay_Hovered() ? ((Clay_Color){52, 60, 78, 255})
                                          : ((Clay_Color){40, 48, 64, 255}),
        .cornerRadius = CLAY_CORNER_RADIUS(UI_RADIUS_MD)
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
                .childGap = UI_SPACE_XXS,
                .layoutDirection = CLAY_TOP_TO_BOTTOM
            }
        }) {
            text = (Clay_String){false, (int32_t)strlen(display_key), display_key};
            CLAY_TEXT(text, CLAY_TEXT_CONFIG({.textColor = {228, 234, 242, 255}, .fontSize = UI_FONT_BODY}));

            text = (Clay_String){false, (int32_t)strlen(file_name), file_name};
            CLAY_TEXT(text, CLAY_TEXT_CONFIG({.textColor = {190, 200, 220, 255}, .fontSize = UI_FONT_SECONDARY}));

            text = (Clay_String){false, (int32_t)strlen(display_path), display_path};
            CLAY_TEXT(text, CLAY_TEXT_CONFIG({.textColor = {150, 162, 186, 255}, .fontSize = UI_FONT_SECONDARY}));
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
            .childGap = UI_SECTION_GAP,
            .layoutDirection = CLAY_TOP_TO_BOTTOM
        }
    }) {
        CLAY_TEXT(header, CLAY_TEXT_CONFIG({.textColor = {182, 208, 244, 255}, .fontSize = UI_FONT_BODY}));

        if (count <= 0) {
            Clay_String empty = CLAY_STRING("(none)");
            CLAY_TEXT(empty, CLAY_TEXT_CONFIG({.textColor = {130, 142, 166, 255}, .fontSize = UI_FONT_SECONDARY}));
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
    int shift_toggle;
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
    shift_toggle = click && e->scene_tree_click_shift;
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
            .padding = CLAY_PADDING_ALL(UI_PANEL_PADDING),
            .childGap = UI_PANEL_GAP,
            .layoutDirection = CLAY_TOP_TO_BOTTOM
        },
        .backgroundColor = {24, 28, 36, 255}
    }) {
        Clay_String ts = {false, (int32_t)strlen(title_buf), title_buf};
        CLAY_TEXT(ts, CLAY_TEXT_CONFIG({.textColor = {220, 225, 235, 255}, .fontSize = UI_FONT_TITLE}));

        CLAY(CLAY_ID("STList"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_GROW({0}) },
                .childGap = UI_ROW_GAP,
                .layoutDirection = CLAY_TOP_TO_BOTTOM
            },
            .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() }
        }) {
            CLAY(CLAY_ID("STListContent"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIT({0}) },
                    .childGap = UI_ROW_GAP,
                    .layoutDirection = CLAY_TOP_TO_BOTTOM
                }
            }) {
                int i;
                if (entity_count <= 0) {
                    Clay_String cs = CLAY_STRING("(empty)");
                    CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {150, 160, 180, 255}, .fontSize = UI_FONT_SECONDARY}));
                } else {
                    for (i = 0; i < entity_count; i++) {
                        int p = parent_of[i];
                        int is_root = (p < 0 || p >= entity_count || p == i);
                        if (is_root) {
                            scene_tree_emit_entity_recursive(gs, e, i, 0, click,
                                                             shift_toggle,
                                                             parent_of, entity_count, visited);
                        }
                    }
                    for (i = 0; i < entity_count; i++) {
                        if (!visited[i]) {
                            scene_tree_emit_entity_recursive(gs, e, i, 0, click,
                                                             shift_toggle,
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
            editor_scene_selection_set_single(e, child);
        }
        e->scene_tree_drag_active = 0;
        e->scene_tree_drag_entity = -1;
        e->scene_tree_drop_target = -1;
        e->scene_tree_drop_mode = SCENE_TREE_DROP_NONE;
    }

    e->scene_tree_click = 0;
    e->scene_tree_click_shift = 0;
}

static void project_browser_layout(game_state *gs, editor_state *es) {
    editor_state *e = es;
    dock_state *d = (dock_state *)e->dock;
    Clay_Context *ctx = (Clay_Context *)e->project_browser_clay_ctx;
    int browser_win_idx;
    int browser_node;
    int win_w, win_h;
    int total_assets;
    int click;
    int i, vis_count, cols, row, col, num_rows;
    int folder_count;
    Clay_RenderCommandArray commands;

    static char title_buf[128];
    static char folders[PB_MAX_FOLDERS][256];
    const char *sel_path;

    /* Visible asset refs (filtered by selected folder) */
    #define PB_MAX_VIS 128
    const char *vis_keys[PB_MAX_VIS];
    const char *vis_paths[PB_MAX_VIS];
    Clay_Color  vis_swatch[PB_MAX_VIS];
    int         vis_type[PB_MAX_VIS];

    if (!ctx) {
        e->project_browser_cmd_count = 0;
        e->project_browser_cmd_array = NULL;
        e->pb_thumbnail_count = 0;
        return;
    }

    Clay_SetCurrentContext(ctx);
    browser_node = dock_find_leaf_for_panel_global(d, PANEL_ASSETS, &browser_win_idx);
    if (browser_node < 0) {
        e->project_browser_cmd_count = 0;
        e->project_browser_cmd_array = NULL;
        e->pb_thumbnail_count = 0;
        return;
    }
    {
        DockNode *n = &d->nodes[browser_node];
        win_w = (int)n->w;
        win_h = (int)(n->h - DOCK_HEADER_HEIGHT);
        if (win_h < 1) win_h = 1;
    }
    Clay_SetLayoutDimensions((Clay_Dimensions){(float)win_w, (float)win_h});

    click = e->project_browser_click;
    {
        Clay_Vector2 mpos = {e->project_browser_mouse_x, e->project_browser_mouse_y};
        Clay_Vector2 sdelta = {0, e->project_browser_scroll_y};
        Clay_SetPointerState(mpos, (bool)e->project_browser_mouse_down);
        Clay_UpdateScrollContainers(true, sdelta, gs->dt);
        e->project_browser_scroll_y = 0.0f;
    }

    total_assets = gs->project.asset_count;
    snprintf(title_buf, sizeof(title_buf), "Project Browser  (%d assets)", total_assets);
    sel_path = e->pb_selected_path;

    folder_count = pb_build_folder_list(&gs->project, folders);

    vis_count = 0;
    for (i = 0; i < gs->project.asset_count && vis_count < PB_MAX_VIS; i++) {
        const project_asset *a = &gs->project.assets[i];
        if (pb_path_matches(a->path, sel_path)) {
            vis_keys[vis_count] = a->key;
            vis_paths[vis_count] = a->path;
            vis_swatch[vis_count] = pb_asset_type_color((int)a->type);
            vis_type[vis_count] = (int)a->type;
            vis_count++;
        }
    }

    /* Compute grid columns from available width */
    {
        int grid_w = win_w - PB_FOLDER_TREE_WIDTH - 24;
        cols = grid_w / (PB_ICON_SIZE + PB_ICON_GAP);
        if (cols < 1) cols = 1;
    }
    num_rows = vis_count > 0 ? (vis_count + cols - 1) / cols : 0;

    Clay_BeginLayout();

    /* Root: 2-column horizontal layout */
    CLAY(CLAY_ID("PBRoot"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_GROW({0}) },
            .layoutDirection = CLAY_LEFT_TO_RIGHT
        },
        .backgroundColor = {24, 28, 36, 255}
    }) {

        /* ── LEFT COLUMN: Folder tree ── */
        CLAY(CLAY_ID("PBFolderTree"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(PB_FOLDER_TREE_WIDTH), CLAY_SIZING_GROW({0}) },
                .padding = CLAY_PADDING_ALL(UI_SECTION_PADDING),
                .childGap = UI_SPACE_XXS,
                .layoutDirection = CLAY_TOP_TO_BOTTOM
            },
            .backgroundColor = {28, 32, 42, 255},
            .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() }
        }) {
            CLAY(CLAY_ID("PBTreeContent"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIT({0}) },
                    .childGap = UI_SPACE_XXS,
                    .layoutDirection = CLAY_TOP_TO_BOTTOM
                }
            }) {
                /* "All" root entry */
                {
                    int is_root_sel = (sel_path[0] == '\0');
                    CLAY(CLAY_ID("PBFolderAll"), {
                        .layout = {
                            .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIT({0}) },
                            .padding = {
                                .left = UI_ROW_PADDING_X,
                                .right = UI_ROW_PADDING_X,
                                .top = UI_ROW_PADDING_Y,
                                .bottom = UI_ROW_PADDING_Y
                            },
                            .childGap = UI_ROW_GAP,
                            .layoutDirection = CLAY_LEFT_TO_RIGHT,
                            .childAlignment = { CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER }
                        },
                        .backgroundColor = is_root_sel ? ((Clay_Color){60, 90, 140, 255})
                                         : Clay_Hovered() ? ((Clay_Color){44, 52, 68, 255})
                                         : ((Clay_Color){0, 0, 0, 0}),
                        .cornerRadius = CLAY_CORNER_RADIUS(UI_RADIUS_SM)
                    }) {
                        {
                            Clay_String n = CLAY_STRING("All");
                            CLAY_TEXT(n, CLAY_TEXT_CONFIG({
                                .textColor = is_root_sel ? ((Clay_Color){255, 255, 255, 255})
                                                         : ((Clay_Color){200, 210, 225, 255}),
                                .fontSize = UI_FONT_SECONDARY
                            }));
                        }
                        if (Clay_Hovered() && click) {
                            e->pb_selected_path[0] = '\0';
                        }
                    }
                }

                /* Folder entries */
                for (i = 0; i < folder_count; i++) {
                    int depth = pb_count_depth(folders[i]);
                    const char *display = pb_last_component(folders[i]);
                    int is_selected = (strcmp(folders[i], sel_path) == 0);
                    int indent = depth * UI_TREE_INDENT;

                    CLAY(CLAY_IDI("PBFolder", i), {
                        .layout = {
                            .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIT({0}) },
                            .padding = {
                                .left = (uint16_t)(UI_SPACE_XS + indent),
                                .right = UI_SPACE_XS,
                                .top = UI_SPACE_XS,
                                .bottom = UI_SPACE_XS
                            },
                            .childGap = UI_ROW_GAP,
                            .layoutDirection = CLAY_LEFT_TO_RIGHT,
                            .childAlignment = { CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER }
                        },
                        .backgroundColor = is_selected ? ((Clay_Color){60, 90, 140, 255})
                                         : Clay_Hovered() ? ((Clay_Color){44, 52, 68, 255})
                                         : ((Clay_Color){0, 0, 0, 0}),
                        .cornerRadius = CLAY_CORNER_RADIUS(UI_RADIUS_SM)
                    }) {
                        /* Folder icon */
                        CLAY(CLAY_IDI("PBFIcon", i), {
                            .layout = {
                                .sizing = { CLAY_SIZING_FIXED(8), CLAY_SIZING_FIXED(8) }
                            },
                            .backgroundColor = {140, 160, 190, 255},
                            .cornerRadius = CLAY_CORNER_RADIUS(2)
                        }) {}

                        /* Folder name */
                        {
                            Clay_String fname = {false, (int32_t)strlen(display), display};
                            CLAY_TEXT(fname, CLAY_TEXT_CONFIG({
                                .textColor = is_selected ? ((Clay_Color){255, 255, 255, 255})
                                                         : ((Clay_Color){190, 200, 218, 255}),
                                .fontSize = UI_FONT_SECONDARY
                            }));
                        }

                        /* Handle click */
                        if (Clay_Hovered() && click) {
                            strncpy(e->pb_selected_path, folders[i], 255);
                            e->pb_selected_path[255] = '\0';
                        }
                    }
                }
            }
        }

        /* ── Separator ── */
        CLAY(CLAY_ID("PBSep"), {
            .layout = { .sizing = { CLAY_SIZING_FIXED(1), CLAY_SIZING_GROW({0}) } },
            .backgroundColor = {50, 58, 75, 255}
        }) {}

        /* ── RIGHT COLUMN: Icon grid ── */
        CLAY(CLAY_ID("PBGridArea"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_GROW({0}) },
                .padding = CLAY_PADDING_ALL(UI_PANEL_PADDING),
                .childGap = UI_SECTION_GAP,
                .layoutDirection = CLAY_TOP_TO_BOTTOM
            }
        }) {
            /* Header with path breadcrumb */
            {
                static char header_buf[300];
                Clay_String ts;
                if (sel_path[0])
                    snprintf(header_buf, sizeof(header_buf), "%s", sel_path);
                else
                    snprintf(header_buf, sizeof(header_buf), "%s", title_buf);
                ts = (Clay_String){ false, (int32_t)strlen(header_buf), header_buf };
                CLAY_TEXT(ts, CLAY_TEXT_CONFIG({.textColor = {180, 190, 208, 255}, .fontSize = UI_FONT_SECONDARY}));
            }

            /* Scrollable grid */
            CLAY(CLAY_ID("PBGridScroll"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_GROW({0}) }
                },
                .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() }
            }) {
                CLAY(CLAY_ID("PBGridContent"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIT({0}) },
                        .childGap = PB_ICON_GAP,
                        .layoutDirection = CLAY_TOP_TO_BOTTOM
                    }
                }) {
                    if (vis_count == 0) {
                        Clay_String cs = CLAY_STRING("(no assets in this folder)");
                        CLAY_TEXT(cs, CLAY_TEXT_CONFIG({
                            .textColor = {130, 142, 166, 255}, .fontSize = UI_FONT_SECONDARY
                        }));
                    } else {
                        for (row = 0; row < num_rows; row++) {
                            CLAY(CLAY_IDI("PBRow", row), {
                                .layout = {
                                    .sizing = { CLAY_SIZING_GROW({0}),
                                                CLAY_SIZING_FIT({0}) },
                                    .childGap = PB_ICON_GAP,
                                    .layoutDirection = CLAY_LEFT_TO_RIGHT
                                }
                            }) {
                                for (col = 0; col < cols; col++) {
                                    int idx = row * cols + col;
                                    if (idx >= vis_count) break;
                                    {
                                        const char *key = vis_keys[idx];
                                        const char *path = vis_paths[idx];
                                        Clay_Color sw = vis_swatch[idx];
                                        Clay_String ks;
                                        int tile_selected = (strcmp(e->pb_selected_asset_key, key) == 0 &&
                                                             e->pb_selected_asset_type == vis_type[idx]);

                                        CLAY(CLAY_IDI("PBTile", idx), {
                                            .layout = {
                                                .sizing = {
                                                    CLAY_SIZING_FIXED(PB_ICON_SIZE),
                                                    CLAY_SIZING_FIT({0})
                                                },
                                                .padding = CLAY_PADDING_ALL(UI_SPACE_XS),
                                                .childGap = UI_ROW_GAP,
                                                .layoutDirection = CLAY_TOP_TO_BOTTOM,
                                                .childAlignment = {
                                                    CLAY_ALIGN_X_CENTER,
                                                    CLAY_ALIGN_Y_TOP
                                                }
                                            },
                                            .backgroundColor = tile_selected
                                                ? ((Clay_Color){58, 72, 98, 255})
                                                : Clay_Hovered()
                                                    ? ((Clay_Color){52, 62, 82, 255})
                                                    : ((Clay_Color){34, 40, 54, 255}),
                                            .cornerRadius = CLAY_CORNER_RADIUS(UI_RADIUS_LG)
                                        }) {
                                            /* Color swatch */
                                            CLAY(CLAY_IDI("PBSwFrame", idx), {
                                                .layout = {
                                                    .sizing = {
                                                        CLAY_SIZING_FIXED(PB_ICON_SIZE - 8),
                                                        CLAY_SIZING_FIXED(44)
                                                    },
                                                    .padding = CLAY_PADDING_ALL(2)
                                                },
                                                .backgroundColor = sw,
                                                .cornerRadius = CLAY_CORNER_RADIUS(UI_RADIUS_MD)
                                            }) {
                                                CLAY(CLAY_IDI("PBSw", idx), {
                                                    .layout = {
                                                        .sizing = {
                                                            CLAY_SIZING_GROW({0}),
                                                            CLAY_SIZING_GROW({0})
                                                        }
                                                    },
                                                    .backgroundColor = {24, 30, 40, 255},
                                                    .cornerRadius = CLAY_CORNER_RADIUS(UI_RADIUS_SM)
                                                }) {}
                                            }

                                            /* Asset name */
                                            ks = (Clay_String){false,
                                                (int32_t)strlen(key), key};
                                            CLAY_TEXT(ks, CLAY_TEXT_CONFIG({
                                                .textColor = {210, 218, 230, 255},
                                                .fontSize = UI_FONT_SECONDARY
                                            }));

                                            if (Clay_Hovered() && click) {
                                                strncpy(e->pb_selected_asset_key, key,
                                                        sizeof(e->pb_selected_asset_key) - 1);
                                                e->pb_selected_asset_key[sizeof(e->pb_selected_asset_key) - 1] = '\0';
                                                strncpy(e->pb_selected_asset_path, path,
                                                        sizeof(e->pb_selected_asset_path) - 1);
                                                e->pb_selected_asset_path[sizeof(e->pb_selected_asset_path) - 1] = '\0';
                                                e->pb_selected_asset_type = vis_type[idx];
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    commands = Clay_EndLayout();
    e->project_browser_cmd_count = commands.length;
    e->project_browser_cmd_array = commands.internalArray;

    /* Build thumbnail requests from swatch bounding boxes */
    e->pb_thumbnail_count = 0;
    for (i = 0; i < vis_count && i < PB_MAX_THUMBNAILS; i++) {
        Clay_ElementData ed = Clay_GetElementData(
            Clay_GetElementIdWithIndex(CLAY_STRING("PBSw"), (uint32_t)i));
        if (!ed.found) continue;
        {
            pb_thumbnail_request *req = &e->pb_thumbnails[e->pb_thumbnail_count];
            strncpy(req->key, vis_keys[i], sizeof(req->key) - 1);
            req->key[sizeof(req->key) - 1] = '\0';
            req->type = vis_type[i];
            req->x = ed.boundingBox.x;
            req->y = ed.boundingBox.y;
            req->w = ed.boundingBox.width;
            req->h = ed.boundingBox.height;
            e->pb_thumbnail_count++;
        }
    }

    e->project_browser_click = 0;
}

static void inspector_layout(game_state *gs, editor_state *es) {
    editor_state *e = es;
    dock_state *d = (dock_state *)e->dock;
    Clay_Context *ctx = (Clay_Context *)e->inspector_clay_ctx;
    int insp_win_idx;
    int insp_node;
    int win_w, win_h;
    int selected;
    int has_asset_selected;
    static char title_buf[96];
    static char line_bufs[96][256];
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
    has_asset_selected = (e->pb_selected_asset_key[0] != '\0' &&
                          e->pb_selected_asset_type >= 0 &&
                          e->pb_selected_asset_type <= 3);

    snprintf(title_buf, sizeof(title_buf), "Inspector");
    Clay_BeginLayout();

    CLAY(CLAY_ID("INRoot"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_GROW({0}) },
            .padding = CLAY_PADDING_ALL(UI_PANEL_PADDING),
            .childGap = UI_PANEL_GAP,
            .layoutDirection = CLAY_TOP_TO_BOTTOM
        },
        .backgroundColor = {24, 28, 36, 255}
    }) {
        Clay_String ts = {false, (int32_t)strlen(title_buf), title_buf};
        CLAY_TEXT(ts, CLAY_TEXT_CONFIG({.textColor = {220, 225, 235, 255}, .fontSize = UI_FONT_TITLE}));

        CLAY(CLAY_ID("INBody"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_GROW({0}) },
                .padding = CLAY_PADDING_ALL(UI_PANEL_PADDING),
                .childGap = UI_SECTION_GAP,
                .layoutDirection = CLAY_TOP_TO_BOTTOM
            },
            .backgroundColor = {36, 42, 56, 255},
            .cornerRadius = CLAY_CORNER_RADIUS(UI_RADIUS_MD)
        }) {
            if (has_asset_selected) {
                int line_i = 0;
                const char *asset_file = project_browser_path_basename(e->pb_selected_asset_path);
                const char *asset_ext = strrchr(asset_file, '.');
                const char *type_name = pb_asset_type_name(e->pb_selected_asset_type);
                const char *bound_mesh_key = NULL;
                scene_model_asset *asset_model = NULL;
                int ai;

                if (e->pb_selected_asset_type == 0 || e->pb_selected_asset_type == 2) {
                    for (ai = 0; ai < gs->scene_model_asset_count; ai++) {
                        if (strcmp(gs->scene_model_assets[ai].key, e->pb_selected_asset_key) == 0) {
                            asset_model = &gs->scene_model_assets[ai];
                            break;
                        }
                    }
                } else if (e->pb_selected_asset_type == 1) {
                    for (ai = 0; ai < gs->project.anim_count; ai++) {
                        const project_anim *pa = &gs->project.anims[ai];
                        const project_mesh *pm = project_find_mesh(&gs->project, pa->entity);
                        if (!pm) continue;
                        if (strcmp(pa->asset, e->pb_selected_asset_key) != 0) continue;
                        bound_mesh_key = pm->model;
                        break;
                    }
                    if (bound_mesh_key) {
                        for (ai = 0; ai < gs->scene_model_asset_count; ai++) {
                            if (strcmp(gs->scene_model_assets[ai].key, bound_mesh_key) == 0) {
                                asset_model = &gs->scene_model_assets[ai];
                                break;
                            }
                        }
                    }
                }

                CLAY(CLAY_ID("INAssetDetails"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIT({0}) },
                        .padding = CLAY_PADDING_ALL(UI_SECTION_PADDING),
                        .childGap = UI_ROW_GAP,
                        .layoutDirection = CLAY_TOP_TO_BOTTOM
                    },
                    .backgroundColor = {30, 36, 48, 255},
                    .cornerRadius = CLAY_CORNER_RADIUS(UI_RADIUS_MD)
                }) {
                    Clay_String hs = CLAY_STRING("Selected Asset");
                    CLAY_TEXT(hs, CLAY_TEXT_CONFIG({.textColor = {228, 236, 248, 255}, .fontSize = UI_FONT_BODY}));

                    snprintf(line_bufs[line_i], sizeof(line_bufs[line_i]),
                             "Type: %s", type_name);
                    {
                        Clay_String ls = {false, (int32_t)strlen(line_bufs[line_i]), line_bufs[line_i]};
                        CLAY_TEXT(ls, CLAY_TEXT_CONFIG({.textColor = {188, 206, 236, 255}, .fontSize = UI_FONT_SECONDARY}));
                    }
                    line_i++;

                    snprintf(line_bufs[line_i], sizeof(line_bufs[line_i]),
                             "Key: %s", e->pb_selected_asset_key);
                    {
                        Clay_String ls = {false, (int32_t)strlen(line_bufs[line_i]), line_bufs[line_i]};
                        CLAY_TEXT(ls, CLAY_TEXT_CONFIG({.textColor = {214, 222, 236, 255}, .fontSize = UI_FONT_SECONDARY}));
                    }
                    line_i++;

                    snprintf(line_bufs[line_i], sizeof(line_bufs[line_i]),
                             "File: %s", asset_file);
                    {
                        Clay_String ls = {false, (int32_t)strlen(line_bufs[line_i]), line_bufs[line_i]};
                        CLAY_TEXT(ls, CLAY_TEXT_CONFIG({.textColor = {184, 194, 210, 255}, .fontSize = UI_FONT_SECONDARY}));
                    }
                    line_i++;

                    snprintf(line_bufs[line_i], sizeof(line_bufs[line_i]),
                             "Path: %s", e->pb_selected_asset_path);
                    {
                        Clay_String ls = {false, (int32_t)strlen(line_bufs[line_i]), line_bufs[line_i]};
                        CLAY_TEXT(ls, CLAY_TEXT_CONFIG({.textColor = {164, 174, 194, 255}, .fontSize = UI_FONT_SECONDARY}));
                    }
                    line_i++;

                    if (asset_ext) {
                        snprintf(line_bufs[line_i], sizeof(line_bufs[line_i]),
                                 "Extension: %s", asset_ext);
                        {
                            Clay_String ls = {false, (int32_t)strlen(line_bufs[line_i]), line_bufs[line_i]};
                            CLAY_TEXT(ls, CLAY_TEXT_CONFIG({.textColor = {164, 174, 194, 255}, .fontSize = UI_FONT_SECONDARY}));
                        }
                        line_i++;
                    }

                    if (e->pb_selected_asset_type == 1) {
                        snprintf(line_bufs[line_i], sizeof(line_bufs[line_i]),
                                 "Rig Mesh: %s", bound_mesh_key ? bound_mesh_key : "(no matching scene rig)");
                        {
                            Clay_String ls = {false, (int32_t)strlen(line_bufs[line_i]), line_bufs[line_i]};
                            CLAY_TEXT(ls, CLAY_TEXT_CONFIG({.textColor = {198, 178, 146, 255}, .fontSize = UI_FONT_SECONDARY}));
                        }
                        line_i++;
                    }

                    if (asset_model) {
                        GltfMesh *mesh = &asset_model->model.mesh;
                        snprintf(line_bufs[line_i], sizeof(line_bufs[line_i]),
                                 "Loaded: %s  Meshes: %u  Skinned: %s  Clips: %u",
                                 asset_model->loaded ? "yes" : "no",
                                 (unsigned int)mesh->primitive_count,
                                 asset_model->has_skeleton ? "yes" : "no",
                                 (unsigned int)asset_model->model.clip_count);
                        {
                            Clay_String ls = {false, (int32_t)strlen(line_bufs[line_i]), line_bufs[line_i]};
                            CLAY_TEXT(ls, CLAY_TEXT_CONFIG({.textColor = {154, 212, 176, 255}, .fontSize = UI_FONT_SECONDARY}));
                        }
                        line_i++;

                        snprintf(line_bufs[line_i], sizeof(line_bufs[line_i]),
                                 "Bounds center=(%.2f, %.2f, %.2f) radius=%.2f",
                                 mesh->bounds_center[0], mesh->bounds_center[1],
                                 mesh->bounds_center[2], mesh->bounds_radius);
                        {
                            Clay_String ls = {false, (int32_t)strlen(line_bufs[line_i]), line_bufs[line_i]};
                            CLAY_TEXT(ls, CLAY_TEXT_CONFIG({.textColor = {154, 212, 176, 255}, .fontSize = UI_FONT_SECONDARY}));
                        }
                        line_i++;
                    } else if (e->pb_selected_asset_type != 3) {
                        Clay_String ms = CLAY_STRING("Loaded mesh info unavailable.");
                        CLAY_TEXT(ms, CLAY_TEXT_CONFIG({.textColor = {180, 152, 152, 255}, .fontSize = UI_FONT_SECONDARY}));
                    }
                }
            }

            if (!has_asset_selected) {
                if (selected < 0) {
                    Clay_String cs = CLAY_STRING("No entity selected.");
                    CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {170, 180, 198, 255}, .fontSize = UI_FONT_SECONDARY}));
                } else {
                int line_i = 0;
                int parent_idx = -1;
                int parent_transform_idx = -1;
                Vec3 tpos = VEC3(0.0f, 0.0f, 0.0f);
                float ry = 0.0f;
                Vec3 tscale = VEC3(1.0f, 1.0f, 1.0f);
                Vec3 vvel = VEC3(0.0f, 0.0f, 0.0f);
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
                    CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {240, 240, 240, 255}, .fontSize = UI_FONT_BODY}));
                }
                line_i++;

                {
                    Clay_String cs = CLAY_STRING("Components");
                    CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {190, 200, 218, 255}, .fontSize = UI_FONT_BODY}));
                }
                if (!has_components) {
                    Clay_String cs = CLAY_STRING("- (none)");
                    CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {150, 160, 180, 255}, .fontSize = UI_FONT_SECONDARY}));
                }

                if (has_transform) {
                    snprintf(line_bufs[line_i], sizeof(line_bufs[line_i]),
                             "- Transform (position=%.2f, %.2f, %.2f)",
                             tpos.x, tpos.y, tpos.z);
                    {
                        Clay_String cs = {false, (int32_t)strlen(line_bufs[line_i]), line_bufs[line_i]};
                        CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {205, 215, 232, 255}, .fontSize = UI_FONT_SECONDARY}));
                    }
                    line_i++;
                }
                if (has_rotation) {
                    snprintf(line_bufs[line_i], sizeof(line_bufs[line_i]),
                             "- Rotation (y=%.2f deg)", ry);
                    {
                        Clay_String cs = {false, (int32_t)strlen(line_bufs[line_i]), line_bufs[line_i]};
                        CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {205, 215, 232, 255}, .fontSize = UI_FONT_SECONDARY}));
                    }
                    line_i++;
                }
                if (has_scale) {
                    snprintf(line_bufs[line_i], sizeof(line_bufs[line_i]),
                             "- Scale (%.2f, %.2f, %.2f)",
                             tscale.x, tscale.y, tscale.z);
                    {
                        Clay_String cs = {false, (int32_t)strlen(line_bufs[line_i]), line_bufs[line_i]};
                        CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {205, 215, 232, 255}, .fontSize = UI_FONT_SECONDARY}));
                    }
                    line_i++;
                }
                if (has_velocity) {
                    snprintf(line_bufs[line_i], sizeof(line_bufs[line_i]),
                             "- Velocity (%.2f, %.2f)", vvel.x, vvel.y);
                    {
                        Clay_String cs = {false, (int32_t)strlen(line_bufs[line_i]), line_bufs[line_i]};
                        CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {205, 215, 232, 255}, .fontSize = UI_FONT_SECONDARY}));
                    }
                    line_i++;
                }
                if (has_character_controller) {
                    snprintf(line_bufs[line_i], sizeof(line_bufs[line_i]),
                             "- Character Controller (move=%.2f jump=%.2f)",
                             cc_move_speed, cc_jump_speed);
                    {
                        Clay_String cs = {false, (int32_t)strlen(line_bufs[line_i]), line_bufs[line_i]};
                        CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {205, 215, 232, 255}, .fontSize = UI_FONT_SECONDARY}));
                    }
                    line_i++;
                }
                if (has_health) {
                    snprintf(line_bufs[line_i], sizeof(line_bufs[line_i]),
                             "- Health (%.1f / %.1f)", hcur, hmax);
                    {
                        Clay_String cs = {false, (int32_t)strlen(line_bufs[line_i]), line_bufs[line_i]};
                        CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {205, 215, 232, 255}, .fontSize = UI_FONT_SECONDARY}));
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
                        CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {205, 215, 232, 255}, .fontSize = UI_FONT_SECONDARY}));
                    }
                    line_i++;
                }
                if (has_mesh) {
                    snprintf(line_bufs[line_i], sizeof(line_bufs[line_i]),
                             "- Mesh (visible=%d)", mesh_visible);
                    {
                        Clay_String cs = {false, (int32_t)strlen(line_bufs[line_i]), line_bufs[line_i]};
                        CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {190, 220, 255, 255}, .fontSize = UI_FONT_SECONDARY}));
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
                        CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {164, 198, 236, 255}, .fontSize = UI_FONT_SECONDARY}));
                    }
                    line_i++;
                }
                if (has_camera) {
                    snprintf(line_bufs[line_i], sizeof(line_bufs[line_i]),
                             "- Camera (fov=%.1f near=%.2f far=%.1f)", cam_fov, cam_near, cam_far);
                    {
                        Clay_String cs = {false, (int32_t)strlen(line_bufs[line_i]), line_bufs[line_i]};
                        CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {190, 220, 255, 255}, .fontSize = UI_FONT_SECONDARY}));
                    }
                    line_i++;

                    snprintf(line_bufs[line_i], sizeof(line_bufs[line_i]),
                             "  target=(%.2f, %.2f, %.2f) up=(%.2f, %.2f, %.2f)",
                             cam_target.x, cam_target.y, cam_target.z,
                             cam_up.x, cam_up.y, cam_up.z);
                    {
                        Clay_String cs = {false, (int32_t)strlen(line_bufs[line_i]), line_bufs[line_i]};
                        CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {164, 198, 236, 255}, .fontSize = UI_FONT_SECONDARY}));
                    }
                    line_i++;
                }
                if (has_parent) {
                    snprintf(line_bufs[line_i], sizeof(line_bufs[line_i]),
                             "- Parent (entity %d)", parent_idx);
                    {
                        Clay_String cs = {false, (int32_t)strlen(line_bufs[line_i]), line_bufs[line_i]};
                        CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {170, 210, 255, 255}, .fontSize = UI_FONT_SECONDARY}));
                    }
                    line_i++;
                }
                if (has_parent_transform) {
                    snprintf(line_bufs[line_i], sizeof(line_bufs[line_i]),
                             "- Parent Transform (entity %d)", parent_transform_idx);
                    {
                        Clay_String cs = {false, (int32_t)strlen(line_bufs[line_i]), line_bufs[line_i]};
                        CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {170, 255, 190, 255}, .fontSize = UI_FONT_SECONDARY}));
                    }
                    line_i++;
                }
                if (has_parent_rotation) {
                    Clay_String cs = CLAY_STRING("- Parent Rotation");
                    CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {255, 210, 170, 255}, .fontSize = UI_FONT_SECONDARY}));
                }
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
            .padding = { .left = UI_SPACE_SM, .right = UI_SECTION_PADDING, .top = UI_SPACE_XXS, .bottom = UI_SPACE_XXS },
            .childGap = UI_SPACE_SM,
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
            .childAlignment = { CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER }
        },
        .backgroundColor = {24, 28, 36, 255}
    }) {
        CLAY(CLAY_ID("MenuFile"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIT({0}), CLAY_SIZING_FIXED(22) },
                .padding = { .left = UI_PANEL_PADDING, .right = UI_PANEL_PADDING, .top = UI_SPACE_XXS, .bottom = UI_SPACE_XXS },
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
                .fontSize = UI_FONT_BODY
            }));

            if (e->menu_open == MENU_FILE) {
                int i;
                CLAY(CLAY_ID("FileDropdown"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(190), CLAY_SIZING_FIT({0}) },
                        .padding = CLAY_PADDING_ALL(UI_SPACE_XS),
                        .childGap = UI_SPACE_XXS,
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
                                .padding = {
                                    .left = UI_ROW_PADDING_X,
                                    .right = UI_ROW_PADDING_X,
                                    .top = UI_SPACE_XS,
                                    .bottom = UI_SPACE_XS
                                },
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
                                    .fontSize = UI_FONT_BODY
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
                .sizing = { CLAY_SIZING_FIT({0}), CLAY_SIZING_FIXED(22) },
                .padding = { .left = UI_PANEL_PADDING, .right = UI_PANEL_PADDING, .top = UI_SPACE_XXS, .bottom = UI_SPACE_XXS },
                .childGap = UI_SPACE_SM,
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
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
                const char *icon_label = play_mode ? UI_ICON_STOP_FILL : UI_ICON_PLAY_FILL;
                const char *text_label = play_mode ? "Stop" : "Play";
                Clay_String icon_text = { false, (int32_t)strlen(icon_label), icon_label };
                Clay_String word_text = { false, (int32_t)strlen(text_label), text_label };

                CLAY_TEXT(icon_text, CLAY_TEXT_CONFIG({
                    .textColor = {238, 242, 248, 255},
                    .fontSize = UI_FONT_BODY
                }));
                CLAY_TEXT(word_text, CLAY_TEXT_CONFIG({
                    .textColor = {238, 242, 248, 255},
                    .fontSize = UI_FONT_BODY
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
            .cornerRadius = CLAY_CORNER_RADIUS(UI_RADIUS_LG)
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
                    .cornerRadius = CLAY_CORNER_RADIUS(UI_RADIUS_MD)
                }) {
                    Clay_String cs = CLAY_STRING(UI_ICON_CAMERA " Perspective");
                    CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {236, 240, 248, 255}, .fontSize = UI_FONT_BODY}));
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
                    .cornerRadius = CLAY_CORNER_RADIUS(UI_RADIUS_MD)
                }) {
                    Clay_String cs = CLAY_STRING(UI_ICON_BOUNDING_BOX " Orthographic");
                    CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {236, 240, 248, 255}, .fontSize = UI_FONT_BODY}));
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
                    .cornerRadius = CLAY_CORNER_RADIUS(UI_RADIUS_MD)
                }) {
                    Clay_String cs = CLAY_STRING(UI_ICON_ARROW_LEFT " Front");
                    CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {236, 240, 248, 255}, .fontSize = UI_FONT_BODY}));
                }

                CLAY(CLAY_ID("EDToolbarSide"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(EDITOR_VIEWBAR_VIEW_WIDTH),
                                    CLAY_SIZING_FIXED(EDITOR_VIEWBAR_BUTTON_HEIGHT) },
                        .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}
                    },
                    .backgroundColor = {56, 66, 84, 220},
                    .cornerRadius = CLAY_CORNER_RADIUS(UI_RADIUS_MD)
                }) {
                    Clay_String cs = CLAY_STRING(UI_ICON_ARROW_RIGHT " Side");
                    CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {236, 240, 248, 255}, .fontSize = UI_FONT_BODY}));
                }

                CLAY(CLAY_ID("EDToolbarTop"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(EDITOR_VIEWBAR_VIEW_WIDTH),
                                    CLAY_SIZING_FIXED(EDITOR_VIEWBAR_BUTTON_HEIGHT) },
                        .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}
                    },
                    .backgroundColor = {56, 66, 84, 220},
                    .cornerRadius = CLAY_CORNER_RADIUS(UI_RADIUS_MD)
                }) {
                    Clay_String cs = CLAY_STRING(UI_ICON_ARROW_UP " Top");
                    CLAY_TEXT(cs, CLAY_TEXT_CONFIG({.textColor = {236, 240, 248, 255}, .fontSize = UI_FONT_BODY}));
                }
            }

           
        }
    }

    commands = Clay_EndLayout();
    e->editor_toolbar_cmd_count = commands.length;
    e->editor_toolbar_cmd_array = commands.internalArray;
}

/* ── Cache Profiler Clay layout ── */

static void cache_profiler_layout(game_state *gs, editor_state *es) {
    editor_state *e = es;
    dock_state *d = (dock_state *)e->dock;
    Clay_Context *ctx = (Clay_Context *)e->cache_prof_clay_ctx;
    int win_idx, node;
    int win_w, win_h;
    Clay_RenderCommandArray commands;
    cache_prof_frame *frame;
    int i;
    static char bufs[CACHE_PROF_MAX_ZONES][5][32];

    if (!ctx) return;

    Clay_SetCurrentContext(ctx);

    node = dock_find_leaf_for_panel_global(d, PANEL_CACHE_PROFILER, &win_idx);
    if (node >= 0) {
        DockNode *pn = &d->nodes[node];
        win_w = (int)pn->w;
        win_h = (int)(pn->h - DOCK_HEADER_HEIGHT);
        if (win_h < 1) win_h = 1;
    } else {
        win_w = 800;
        win_h = 600;
    }
    Clay_SetLayoutDimensions((Clay_Dimensions){(float)win_w, (float)win_h});

    {
        Clay_Vector2 mpos = {e->cache_prof_mouse_x, e->cache_prof_mouse_y};
        Clay_Vector2 sdelta = {0, e->cache_prof_scroll_y};
        Clay_SetPointerState(mpos, e->cache_prof_mouse_down);
        Clay_UpdateScrollContainers(true, sdelta, gs->dt);
        e->cache_prof_scroll_y = 0;
    }

    Clay_BeginLayout();

    CLAY(CLAY_ID("CPRoot"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_GROW({0}) },
            .padding = CLAY_PADDING_ALL(12),
            .childGap = 4,
            .layoutDirection = CLAY_TOP_TO_BOTTOM
        },
        .backgroundColor = {25, 25, 30, 255}
    }) {
        /* Title */
        {
            Clay_String ts = CLAY_STRING("Cache Profiler");
            CLAY_TEXT(ts, CLAY_TEXT_CONFIG({.textColor = {220, 220, 220, 255}, .fontSize = 16}));
        }

        if (!cache_prof_available()) {
            const char *status = cache_prof_status_message();
            if (!status || !status[0]) status = "Hardware counters not available on this platform";
            Clay_String msg = { false, (int32_t)strlen(status), (char *)status };
            CLAY_TEXT(msg, CLAY_TEXT_CONFIG({.textColor = {170, 120, 120, 255}, .fontSize = 14}));
        } else {
            /* Column headers */
            CLAY(CLAY_ID("CPHeader"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIT({0}) },
                    .padding = { .left = 8, .right = 8, .top = 4, .bottom = 4 },
                    .childGap = 8,
                    .layoutDirection = CLAY_LEFT_TO_RIGHT
                }
            }) {
                CLAY(CLAY_IDI("CPHCol", 0), { .layout = { .sizing = { CLAY_SIZING_FIXED(140), CLAY_SIZING_FIT({0}) } } }) {
                    Clay_String h = CLAY_STRING("Zone");
                    CLAY_TEXT(h, CLAY_TEXT_CONFIG({.textColor = {150, 150, 160, 255}, .fontSize = 14}));
                }
                CLAY(CLAY_IDI("CPHCol", 1), { .layout = { .sizing = { CLAY_SIZING_FIXED(90), CLAY_SIZING_FIT({0}) }, .childAlignment = { CLAY_ALIGN_X_RIGHT, CLAY_ALIGN_Y_CENTER } } }) {
                    Clay_String h = CLAY_STRING("L1D Miss");
                    CLAY_TEXT(h, CLAY_TEXT_CONFIG({.textColor = {150, 150, 160, 255}, .fontSize = 14}));
                }
                CLAY(CLAY_IDI("CPHCol", 2), { .layout = { .sizing = { CLAY_SIZING_FIXED(90), CLAY_SIZING_FIT({0}) }, .childAlignment = { CLAY_ALIGN_X_RIGHT, CLAY_ALIGN_Y_CENTER } } }) {
                    Clay_String h = CLAY_STRING("Branch Miss");
                    CLAY_TEXT(h, CLAY_TEXT_CONFIG({.textColor = {150, 150, 160, 255}, .fontSize = 14}));
                }
                CLAY(CLAY_IDI("CPHCol", 3), { .layout = { .sizing = { CLAY_SIZING_FIXED(100), CLAY_SIZING_FIT({0}) }, .childAlignment = { CLAY_ALIGN_X_RIGHT, CLAY_ALIGN_Y_CENTER } } }) {
                    Clay_String h = CLAY_STRING("Instructions");
                    CLAY_TEXT(h, CLAY_TEXT_CONFIG({.textColor = {150, 150, 160, 255}, .fontSize = 14}));
                }
                CLAY(CLAY_IDI("CPHCol", 4), { .layout = { .sizing = { CLAY_SIZING_FIXED(90), CLAY_SIZING_FIT({0}) }, .childAlignment = { CLAY_ALIGN_X_RIGHT, CLAY_ALIGN_Y_CENTER } } }) {
                    Clay_String h = CLAY_STRING("Cycles");
                    CLAY_TEXT(h, CLAY_TEXT_CONFIG({.textColor = {150, 150, 160, 255}, .fontSize = 14}));
                }
            }

            /* Zone rows — sorted by L1D misses descending */
            frame = cache_prof_get_frame();
            cache_prof_sort_zones();

            /* Scrollable zone list */
            CLAY(CLAY_ID("CPScroll"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_GROW({0}) },
                    .childGap = 2,
                    .layoutDirection = CLAY_TOP_TO_BOTTOM
                },
                .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() }
            }) {
                for (i = 0; i < frame->count && i < CACHE_PROF_MAX_ZONES; i++) {
                    /* Each row */
                    CLAY(CLAY_IDI("CPRow", i), {
                        .layout = {
                            .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIT({0}) },
                            .padding = { .left = 8, .right = 8, .top = 3, .bottom = 3 },
                            .childGap = 8,
                            .layoutDirection = CLAY_LEFT_TO_RIGHT
                        },
                        .backgroundColor = (i % 2 == 0) ? (Clay_Color){35, 35, 40, 255} : (Clay_Color){30, 30, 35, 255}
                    }) {
                        /* Zone name */
                        CLAY(CLAY_IDI("CPName", i), { .layout = { .sizing = { CLAY_SIZING_FIXED(140), CLAY_SIZING_FIT({0}) } } }) {
                            Clay_String ns = {false, (int32_t)strlen(frame->names[i]), (char*)frame->names[i]};
                            CLAY_TEXT(ns, CLAY_TEXT_CONFIG({.textColor = {200, 200, 210, 255}, .fontSize = 14}));
                        }
                        /* L1D miss */
                        CLAY(CLAY_IDI("CPL1D", i), { .layout = { .sizing = { CLAY_SIZING_FIXED(90), CLAY_SIZING_FIT({0}) }, .childAlignment = { CLAY_ALIGN_X_RIGHT, CLAY_ALIGN_Y_CENTER } } }) {
                            snprintf(bufs[i][0], sizeof(bufs[i][0]), "%llu", (unsigned long long)frame->l1d_miss[i]);
                            {
                                Clay_String vs = {false, (int32_t)strlen(bufs[i][0]), bufs[i][0]};
                                CLAY_TEXT(vs, CLAY_TEXT_CONFIG({.textColor = {255, 180, 100, 255}, .fontSize = 14}));
                            }
                        }
                        /* Branch miss */
                        CLAY(CLAY_IDI("CPBr", i), { .layout = { .sizing = { CLAY_SIZING_FIXED(90), CLAY_SIZING_FIT({0}) }, .childAlignment = { CLAY_ALIGN_X_RIGHT, CLAY_ALIGN_Y_CENTER } } }) {
                            snprintf(bufs[i][1], sizeof(bufs[i][1]), "%llu", (unsigned long long)frame->branch_miss[i]);
                            {
                                Clay_String vs = {false, (int32_t)strlen(bufs[i][1]), bufs[i][1]};
                                CLAY_TEXT(vs, CLAY_TEXT_CONFIG({.textColor = {180, 200, 255, 255}, .fontSize = 14}));
                            }
                        }
                        /* Instructions */
                        CLAY(CLAY_IDI("CPIns", i), { .layout = { .sizing = { CLAY_SIZING_FIXED(100), CLAY_SIZING_FIT({0}) }, .childAlignment = { CLAY_ALIGN_X_RIGHT, CLAY_ALIGN_Y_CENTER } } }) {
                            if (frame->instructions[i] > 1000000)
                                snprintf(bufs[i][2], sizeof(bufs[i][2]), "%.1fM", (double)frame->instructions[i] / 1000000.0);
                            else if (frame->instructions[i] > 1000)
                                snprintf(bufs[i][2], sizeof(bufs[i][2]), "%.1fK", (double)frame->instructions[i] / 1000.0);
                            else
                                snprintf(bufs[i][2], sizeof(bufs[i][2]), "%llu", (unsigned long long)frame->instructions[i]);
                            {
                                Clay_String vs = {false, (int32_t)strlen(bufs[i][2]), bufs[i][2]};
                                CLAY_TEXT(vs, CLAY_TEXT_CONFIG({.textColor = {170, 170, 180, 255}, .fontSize = 14}));
                            }
                        }
                        /* Cycles */
                        CLAY(CLAY_IDI("CPCyc", i), { .layout = { .sizing = { CLAY_SIZING_FIXED(90), CLAY_SIZING_FIT({0}) }, .childAlignment = { CLAY_ALIGN_X_RIGHT, CLAY_ALIGN_Y_CENTER } } }) {
                            if (frame->cycles[i] > 1000000)
                                snprintf(bufs[i][3], sizeof(bufs[i][3]), "%.1fM", (double)frame->cycles[i] / 1000000.0);
                            else if (frame->cycles[i] > 1000)
                                snprintf(bufs[i][3], sizeof(bufs[i][3]), "%.1fK", (double)frame->cycles[i] / 1000.0);
                            else
                                snprintf(bufs[i][3], sizeof(bufs[i][3]), "%llu", (unsigned long long)frame->cycles[i]);
                            {
                                Clay_String vs = {false, (int32_t)strlen(bufs[i][3]), bufs[i][3]};
                                CLAY_TEXT(vs, CLAY_TEXT_CONFIG({.textColor = {170, 170, 180, 255}, .fontSize = 14}));
                            }
                        }
                    }
                }
            }
        }
    }

    commands = Clay_EndLayout();
    e->cache_prof_cmd_count = commands.length;
    e->cache_prof_cmd_array = commands.internalArray;
    e->cache_prof_click = 0;
}

/* ── CPU Profiler Clay layout ── */

static Clay_Color cpu_prof_flame_color(const char *name, uint16_t depth) {
    uint32_t h = 2166136261u;
    const unsigned char *p = (const unsigned char *)(name ? name : "");
    while (*p) {
        h ^= (uint32_t)(*p++);
        h *= 16777619u;
    }
    h ^= (uint32_t)depth * 0x9e3779b9u;

    return (Clay_Color){
        (uint8_t)(96 + (h & 0x3F)),
        (uint8_t)(88 + ((h >> 6) & 0x5F)),
        (uint8_t)(96 + ((h >> 13) & 0x5F)),
        255
    };
}

static void cpu_profiler_layout(game_state *gs, editor_state *es) {
    editor_state *e = es;
    dock_state *d = (dock_state *)e->dock;
    Clay_Context *ctx = (Clay_Context *)e->cpu_prof_clay_ctx;
    int win_idx, node;
    int win_w, win_h;
    Clay_RenderCommandArray commands;
    cpu_prof_frame *frame = NULL;
    uint64_t frame_id = 0;
    int history_count = 0;
    int i, j;
    int hovered_zone = -1;
    int clicked_zone = -1;
    static char bufs[CPU_PROF_MAX_ZONES][4][80];
    int first_child[CPU_PROF_MAX_ZONES];
    int last_child[CPU_PROF_MAX_ZONES];
    int next_sibling[CPU_PROF_MAX_ZONES];
    int root_next[CPU_PROF_MAX_ZONES];
    uint8_t zoom_visible[CPU_PROF_MAX_ZONES];
    uint16_t root_order[CPU_PROF_MAX_ZONES];
    uint16_t child_order[CPU_PROF_MAX_ZONES];
    uint16_t tree_stack_idx[CPU_PROF_MAX_ZONES];
    uint16_t tree_stack_depth[CPU_PROF_MAX_ZONES];
    int root_first = -1;
    int root_last = -1;
    char *frame_info;
    char *hover_line;
    char *flame_title;
    char *list_info;
    float minimap_view_ratio = 1.0f;
    int minimap_has_view = 0;

    if (!ctx) return;
    if (!e->cpu_prof_text_arena) return;

    arena_reset(e->cpu_prof_text_arena);
    frame_info = (char *)arena_alloc(e->cpu_prof_text_arena, 128, 1, "cpu_frame_info");
    hover_line = (char *)arena_alloc(e->cpu_prof_text_arena, 192, 1, "cpu_hover_line");
    flame_title = (char *)arena_alloc(e->cpu_prof_text_arena, 160, 1, "cpu_flame_title");
    list_info = (char *)arena_alloc(e->cpu_prof_text_arena, 128, 1, "cpu_list_info");
    if (!frame_info || !hover_line || !flame_title || !list_info) return;
    frame_info[0] = '\0';
    hover_line[0] = '\0';
    flame_title[0] = '\0';
    list_info[0] = '\0';

    Clay_SetCurrentContext(ctx);

    node = dock_find_leaf_for_panel_global(d, PANEL_CPU_PROFILER, &win_idx);
    if (node >= 0) {
        DockNode *pn = &d->nodes[node];
        win_w = (int)pn->w;
        win_h = (int)(pn->h - DOCK_HEADER_HEIGHT);
        if (win_h < 1) win_h = 1;
    } else {
        win_w = 800;
        win_h = 600;
    }
    Clay_SetLayoutDimensions((Clay_Dimensions){(float)win_w, (float)win_h});

    {
        Clay_Vector2 mpos = {e->cpu_prof_mouse_x, e->cpu_prof_mouse_y};
        Clay_Vector2 sdelta = {0, e->cpu_prof_scroll_y};
        Clay_SetPointerState(mpos, e->cpu_prof_mouse_down);
        Clay_UpdateScrollContainers(true, sdelta, gs->dt);
        e->cpu_prof_scroll_y = 0;
    }
    e->cpu_prof_hover_zone_active = 0;
    e->cpu_prof_hover_zone_name[0] = '\0';

    Clay_BeginLayout();

    history_count = cpu_prof_get_history_count();
    if (history_count > 0) {
        uint64_t latest_frame_id = cpu_prof_get_frame_id_at_offset(0);
        int k;

        if (!e->cpu_prof_timeline_paused) {
            e->cpu_prof_timeline_offset = 0;
            e->cpu_prof_timeline_frame_id = latest_frame_id;
        } else {
            int found = -1;
            if (e->cpu_prof_timeline_frame_id == 0) {
                e->cpu_prof_timeline_frame_id = latest_frame_id;
            }
            for (k = 0; k < history_count; k++) {
                if (cpu_prof_get_frame_id_at_offset((uint16_t)k) == e->cpu_prof_timeline_frame_id) {
                    found = k;
                    break;
                }
            }
            if (found >= 0) {
                e->cpu_prof_timeline_offset = found;
            } else {
                e->cpu_prof_timeline_offset = history_count - 1;
                e->cpu_prof_timeline_frame_id =
                    cpu_prof_get_frame_id_at_offset((uint16_t)e->cpu_prof_timeline_offset);
            }
        }

        if (e->cpu_prof_timeline_offset < 0) e->cpu_prof_timeline_offset = 0;
        if (e->cpu_prof_timeline_offset >= history_count) e->cpu_prof_timeline_offset = history_count - 1;

        frame = cpu_prof_get_frame_at_offset((uint16_t)e->cpu_prof_timeline_offset);
        frame_id = cpu_prof_get_frame_id_at_offset((uint16_t)e->cpu_prof_timeline_offset);
        if (!frame) {
            e->cpu_prof_timeline_offset = 0;
            frame = cpu_prof_get_frame_at_offset(0);
            frame_id = cpu_prof_get_frame_id_at_offset(0);
            e->cpu_prof_timeline_frame_id = frame_id;
        }
    }

    CLAY(CLAY_ID("CURoot"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_GROW({0}) },
            .padding = CLAY_PADDING_ALL(12),
            .childGap = 4,
            .layoutDirection = CLAY_TOP_TO_BOTTOM
        },
        .backgroundColor = {25, 25, 30, 255}
    }) {
        /* Title */
        {
            Clay_String ts = CLAY_STRING("CPU Profiler");
            CLAY_TEXT(ts, CLAY_TEXT_CONFIG({.textColor = {220, 220, 220, 255}, .fontSize = 16}));
        }

        if (history_count <= 0 || !frame) {
            Clay_String msg = CLAY_STRING("No CPU frame history yet");
            CLAY_TEXT(msg, CLAY_TEXT_CONFIG({.textColor = {168, 168, 178, 255}, .fontSize = 14}));
        } else {
            CLAY(CLAY_ID("CUTimelineControls"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIT({0}) },
                    .layoutDirection = CLAY_LEFT_TO_RIGHT,
                    .childGap = 8,
                    .childAlignment = { CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER }
                }
            }) {
                CLAY(CLAY_ID("CUPauseBtn"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(72), CLAY_SIZING_FIXED(22) },
                        .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER }
                    },
                    .backgroundColor = e->cpu_prof_timeline_paused
                        ? (Clay_Color){96, 72, 72, 255}
                        : (Clay_Color){62, 92, 72, 255},
                    .cornerRadius = CLAY_CORNER_RADIUS(3)
                }) {
                    Clay_String ps = e->cpu_prof_timeline_paused
                        ? CLAY_STRING("Paused")
                        : CLAY_STRING("Live");
                    CLAY_TEXT(ps, CLAY_TEXT_CONFIG({.textColor = {236, 238, 244, 255}, .fontSize = 13}));
                    if (Clay_Hovered() && e->cpu_prof_click) {
                        if (e->cpu_prof_timeline_paused) {
                            e->cpu_prof_timeline_paused = 0;
                            e->cpu_prof_timeline_offset = 0;
                            e->cpu_prof_timeline_frame_id = cpu_prof_get_frame_id_at_offset(0);
                        } else {
                            e->cpu_prof_timeline_paused = 1;
                            e->cpu_prof_timeline_frame_id =
                                frame_id ? frame_id : cpu_prof_get_frame_id_at_offset(0);
                        }
                    }
                }

                CLAY(CLAY_ID("CUPrevBtn"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(26), CLAY_SIZING_FIXED(22) },
                        .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER }
                    },
                    .backgroundColor = {48, 54, 68, 255},
                    .cornerRadius = CLAY_CORNER_RADIUS(3)
                }) {
                    Clay_String s = CLAY_STRING(UI_ICON_ARROW_LEFT);
                    CLAY_TEXT(s, CLAY_TEXT_CONFIG({.textColor = {220, 224, 236, 255}, .fontSize = 13}));
                    if (Clay_Hovered() && e->cpu_prof_click) {
                        e->cpu_prof_timeline_paused = 1;
                        if (e->cpu_prof_timeline_offset + 1 < history_count) e->cpu_prof_timeline_offset++;
                        e->cpu_prof_timeline_frame_id =
                            cpu_prof_get_frame_id_at_offset((uint16_t)e->cpu_prof_timeline_offset);
                    }
                }

                CLAY(CLAY_ID("CUNextBtn"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(26), CLAY_SIZING_FIXED(22) },
                        .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER }
                    },
                    .backgroundColor = {48, 54, 68, 255},
                    .cornerRadius = CLAY_CORNER_RADIUS(3)
                }) {
                    Clay_String s = CLAY_STRING(UI_ICON_ARROW_RIGHT);
                    CLAY_TEXT(s, CLAY_TEXT_CONFIG({.textColor = {220, 224, 236, 255}, .fontSize = 13}));
                    if (Clay_Hovered() && e->cpu_prof_click) {
                        if (e->cpu_prof_timeline_offset > 0) {
                            e->cpu_prof_timeline_paused = 1;
                            e->cpu_prof_timeline_offset--;
                            e->cpu_prof_timeline_frame_id =
                                cpu_prof_get_frame_id_at_offset((uint16_t)e->cpu_prof_timeline_offset);
                        } else {
                            e->cpu_prof_timeline_paused = 0;
                            e->cpu_prof_timeline_offset = 0;
                            e->cpu_prof_timeline_frame_id = cpu_prof_get_frame_id_at_offset(0);
                        }
                    }
                }

                snprintf(frame_info, 128,
                         "Frame #%llu (%s, %d back, %d stored)",
                         (unsigned long long)frame_id,
                         e->cpu_prof_timeline_paused ? "paused" : "live",
                         e->cpu_prof_timeline_offset,
                         history_count);
                {
                    Clay_String info = {false, (int32_t)strlen(frame_info), frame_info};
                    CLAY_TEXT(info, CLAY_TEXT_CONFIG({.textColor = {182, 190, 206, 255}, .fontSize = 13}));
                }
            }

            CLAY(CLAY_ID("CUTimelineOuter"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIXED(26) },
                    .padding = CLAY_PADDING_ALL(3),
                    .layoutDirection = CLAY_TOP_TO_BOTTOM
                },
                .backgroundColor = {19, 20, 25, 255},
                .cornerRadius = CLAY_CORNER_RADIUS(3)
            }) {
                float timeline_w = (float)win_w - 40.0f;
                float bar_w;
                if (timeline_w < 80.0f) timeline_w = 80.0f;
                bar_w = timeline_w / (float)history_count;
                if (bar_w < 0.1f) bar_w = 0.1f;

                CLAY(CLAY_ID("CUTimelineStrip"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_GROW({0}) },
                        .layoutDirection = CLAY_LEFT_TO_RIGHT,
                        .childGap = 0
                    },
                    .backgroundColor = {15, 16, 20, 255},
                    .cornerRadius = CLAY_CORNER_RADIUS(2)
                }) {
                    for (i = 0; i < history_count; i++) {
                        int offset = history_count - 1 - i;
                        cpu_prof_frame *f = cpu_prof_get_frame_at_offset((uint16_t)offset);
                        uint64_t max_dur = 0;
                        Clay_Color c = {58, 64, 76, 255};
                        int selected = e->cpu_prof_timeline_paused && (offset == e->cpu_prof_timeline_offset);
                        if (f) {
                            for (j = 0; j < f->count; j++) {
                                if (f->duration_ns[j] > max_dur) max_dur = f->duration_ns[j];
                            }
                            if (max_dur > 12000000ULL) c = (Clay_Color){212, 96, 88, 255};
                            else if (max_dur > 6000000ULL) c = (Clay_Color){188, 120, 92, 255};
                            else if (max_dur > 2000000ULL) c = (Clay_Color){142, 124, 86, 255};
                            else if (max_dur > 1000000ULL) c = (Clay_Color){102, 122, 92, 255};
                        }
                        if (selected) c = (Clay_Color){92, 150, 232, 255};

                        CLAY(CLAY_IDI("CUTimelineBar", i), {
                            .layout = {
                                .sizing = { CLAY_SIZING_FIXED(bar_w), CLAY_SIZING_GROW({0}) }
                            },
                            .backgroundColor = Clay_Hovered()
                                ? (Clay_Color){(uint8_t)(c.r + 20), (uint8_t)(c.g + 20), (uint8_t)(c.b + 20), 255}
                                : c
                        }) {
                            if (Clay_Hovered() && e->cpu_prof_click) {
                                e->cpu_prof_timeline_paused = 1;
                                e->cpu_prof_timeline_offset = offset;
                                e->cpu_prof_timeline_frame_id =
                                    cpu_prof_get_frame_id_at_offset((uint16_t)offset);
                            }
                        }
                    }
                }
            }

            if (frame->count <= 0) {
                Clay_String msg = CLAY_STRING("Selected frame has no zones");
                CLAY_TEXT(msg, CLAY_TEXT_CONFIG({.textColor = {168, 168, 178, 255}, .fontSize = 14}));
            } else {
            uint64_t min_start = UINT64_MAX;
            uint64_t max_end = 0;
            uint16_t max_depth = 0;
            int zone_count = frame->count < CPU_PROF_MAX_ZONES ? frame->count : CPU_PROF_MAX_ZONES;
            int zoom_visible_count = 0;
            double full_span;
            double zoom;
            double center;
            double view_span;
            double view_start;
            double view_end;
            float flame_w;
            float flame_h;
            const float bar_h = 18.0f;
            const float bar_gap = 2.0f;
            const uint64_t span_ns_min = 1ULL;

            if (e->cpu_prof_tree_frame_id != frame_id) {
                memset(e->cpu_prof_tree_collapsed, 0, sizeof(e->cpu_prof_tree_collapsed));
                e->cpu_prof_tree_frame_id = frame_id;
            }

            root_first = -1;
            root_last = -1;
            for (i = 0; i < zone_count; i++) {
                uint64_t zstart = frame->start_ns[i];
                uint64_t zend = zstart + frame->duration_ns[i];
                if (zstart < min_start) min_start = zstart;
                if (zend > max_end) max_end = zend;
                if (frame->depth[i] > max_depth) max_depth = frame->depth[i];
                first_child[i] = -1;
                last_child[i] = -1;
                next_sibling[i] = -1;
                root_next[i] = -1;
                zoom_visible[i] = 0;
            }

            for (i = 0; i < zone_count; i++) {
                uint16_t parent = frame->parent[i];
                if (parent != CPU_PROF_INVALID_PARENT && parent < (uint16_t)zone_count) {
                    if (first_child[parent] < 0) {
                        first_child[parent] = i;
                    } else {
                        next_sibling[last_child[parent]] = i;
                    }
                    last_child[parent] = i;
                } else {
                    if (root_first < 0) {
                        root_first = i;
                    } else {
                        root_next[root_last] = i;
                    }
                    root_last = i;
                }
            }

            if (max_end <= min_start) max_end = min_start + span_ns_min;
            full_span = (double)(max_end - min_start);
            if (full_span < 1.0) full_span = 1.0;

            zoom = (double)e->cpu_prof_flame_zoom;
            center = (double)e->cpu_prof_flame_center;
            if (zoom < 1.0) zoom = 1.0;
            if (zoom > 256.0) zoom = 256.0;
            if (center < 0.0) center = 0.0;
            if (center > 1.0) center = 1.0;

            if (e->cpu_prof_flame_zoom_wheel != 0.0f && e->cpu_prof_flame_w > 1.0f) {
                double anchor = 0.5;
                double old_view_span;
                double old_view_start;
                double anchor_ns;
                double new_zoom;
                double new_view_span;
                double new_view_start;

                anchor = (double)((e->cpu_prof_mouse_x - e->cpu_prof_flame_x) / e->cpu_prof_flame_w);
                if (anchor < 0.0) anchor = 0.0;
                if (anchor > 1.0) anchor = 1.0;

                old_view_span = full_span / zoom;
                if (old_view_span < 1.0) old_view_span = 1.0;
                if (old_view_span > full_span) old_view_span = full_span;
                old_view_start = (double)min_start + center * full_span - old_view_span * 0.5;
                if (old_view_start < (double)min_start) old_view_start = (double)min_start;
                if (old_view_start + old_view_span > (double)max_end)
                    old_view_start = (double)max_end - old_view_span;

                anchor_ns = old_view_start + anchor * old_view_span;
                new_zoom = zoom * pow(1.2, (double)e->cpu_prof_flame_zoom_wheel);
                if (new_zoom < 1.0) new_zoom = 1.0;
                if (new_zoom > 256.0) new_zoom = 256.0;

                new_view_span = full_span / new_zoom;
                if (new_view_span < 1.0) new_view_span = 1.0;
                if (new_view_span > full_span) new_view_span = full_span;
                new_view_start = anchor_ns - anchor * new_view_span;
                if (new_view_start < (double)min_start) new_view_start = (double)min_start;
                if (new_view_start + new_view_span > (double)max_end)
                    new_view_start = (double)max_end - new_view_span;

                center = ((new_view_start + new_view_span * 0.5) - (double)min_start) / full_span;
                if (center < 0.0) center = 0.0;
                if (center > 1.0) center = 1.0;
                zoom = new_zoom;
            }
            e->cpu_prof_flame_zoom_wheel = 0.0f;

            if (e->cpu_prof_flame_pan_wheel != 0.0f && zoom > 1.0) {
                double pan_step = (double)e->cpu_prof_flame_pan_wheel * 0.10;
                center += pan_step / zoom;
                if (center < 0.0) center = 0.0;
                if (center > 1.0) center = 1.0;
            }
            e->cpu_prof_flame_pan_wheel = 0.0f;

            e->cpu_prof_flame_zoom = (float)zoom;
            e->cpu_prof_flame_center = (float)center;

            view_span = full_span / zoom;
            if (view_span < 1.0) view_span = 1.0;
            if (view_span > full_span) view_span = full_span;
            view_start = (double)min_start + center * full_span - view_span * 0.5;
            if (view_start < (double)min_start) view_start = (double)min_start;
            if (view_start + view_span > (double)max_end)
                view_start = (double)max_end - view_span;
            view_end = view_start + view_span;
            minimap_view_ratio = (float)(view_span / full_span);
            if (minimap_view_ratio < 0.0f) minimap_view_ratio = 0.0f;
            if (minimap_view_ratio > 1.0f) minimap_view_ratio = 1.0f;
            minimap_has_view = 1;

            for (i = 0; i < zone_count; i++) {
                double zstart = (double)frame->start_ns[i];
                double zend = zstart + (double)frame->duration_ns[i];
                if (zend > view_start && zstart < view_end) {
                    zoom_visible[i] = 1;
                    zoom_visible_count++;
                } else {
                    zoom_visible[i] = 0;
                }
            }

            flame_w = (float)win_w - 24.0f - 8.0f;
            if (flame_w < 160.0f) flame_w = 160.0f;
            flame_h = (float)(max_depth + 1) * (bar_h + bar_gap) + 4.0f;
            if (flame_h < 28.0f) flame_h = 28.0f;

            /* Flame graph title */
            snprintf(flame_title, 160,
                     "Flame Graph (current frame)  x%.2f", (double)zoom);
            CLAY(CLAY_ID("CUFlameTitleRow"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIT({0}) },
                    .layoutDirection = CLAY_LEFT_TO_RIGHT
                }
            }) {
                Clay_String h = {false, (int32_t)strlen(flame_title), flame_title};
                CLAY_TEXT(h, CLAY_TEXT_CONFIG({.textColor = {172, 178, 190, 255}, .fontSize = 13}));
            }

            {
                const float mini_h = 14.0f;
                double ratio_start = (view_start - (double)min_start) / full_span;
                float mini_view_x;
                float mini_view_w;

                if (ratio_start < 0.0) ratio_start = 0.0;
                if (ratio_start > 1.0) ratio_start = 1.0;
                mini_view_x = (float)(ratio_start * (double)flame_w);
                mini_view_w = minimap_view_ratio * flame_w;
                if (mini_view_w < 6.0f) mini_view_w = 6.0f;
                if (mini_view_w > flame_w) mini_view_w = flame_w;
                if (mini_view_x < 0.0f) mini_view_x = 0.0f;
                if (mini_view_x + mini_view_w > flame_w) mini_view_x = flame_w - mini_view_w;

                CLAY(CLAY_ID("CUFlameMiniOuter"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIXED(mini_h + 8.0f) },
                        .padding = CLAY_PADDING_ALL(4),
                        .layoutDirection = CLAY_TOP_TO_BOTTOM
                    },
                    .backgroundColor = {20, 20, 24, 255},
                    .cornerRadius = CLAY_CORNER_RADIUS(3)
                }) {
                    CLAY(CLAY_ID("CUFlameMiniMap"), {
                        .layout = {
                            .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIXED(mini_h) }
                        },
                        .backgroundColor = {14, 15, 20, 255},
                        .cornerRadius = CLAY_CORNER_RADIUS(2)
                    }) {
                        for (i = 0; i < frame->count && i < CPU_PROF_MAX_ZONES; i++) {
                            double zstart = (double)frame->start_ns[i];
                            double zdur = (double)frame->duration_ns[i];
                            float mx;
                            float mw;
                            Clay_Color mc;

                            if (zdur <= 0.0) continue;
                            mx = (float)(((zstart - (double)min_start) / full_span) * (double)flame_w);
                            mw = (float)((zdur / full_span) * (double)flame_w);
                            if (mw < 1.0f) mw = 1.0f;
                            if (mx < 0.0f) mx = 0.0f;
                            if (mx >= flame_w) continue;
                            if (mx + mw > flame_w) mw = flame_w - mx;
                            if (mw <= 0.0f) continue;

                            mc = cpu_prof_flame_color(frame->names[i], frame->depth[i]);
                            CLAY(CLAY_IDI("CUFlameMiniBar", i), {
                                .layout = { .sizing = { CLAY_SIZING_FIXED(mw), CLAY_SIZING_FIXED(mini_h) } },
                                .floating = {
                                    .attachTo = CLAY_ATTACH_TO_PARENT,
                                    .attachPoints = {
                                        .element = CLAY_ATTACH_POINT_LEFT_TOP,
                                        .parent = CLAY_ATTACH_POINT_LEFT_TOP
                                    },
                                    .offset = { mx, 0.0f },
                                    .clipTo = CLAY_CLIP_TO_ATTACHED_PARENT
                                },
                                .backgroundColor = (Clay_Color){
                                    (uint8_t)((mc.r * 7) / 10),
                                    (uint8_t)((mc.g * 7) / 10),
                                    (uint8_t)((mc.b * 7) / 10),
                                    220
                                }
                            }) {}
                        }

                        CLAY(CLAY_ID("CUFlameMiniView"), {
                            .layout = { .sizing = { CLAY_SIZING_FIXED(mini_view_w), CLAY_SIZING_FIXED(mini_h) } },
                            .floating = {
                                .attachTo = CLAY_ATTACH_TO_PARENT,
                                .attachPoints = {
                                    .element = CLAY_ATTACH_POINT_LEFT_TOP,
                                    .parent = CLAY_ATTACH_POINT_LEFT_TOP
                                },
                                .offset = { mini_view_x, 0.0f },
                                .clipTo = CLAY_CLIP_TO_ATTACHED_PARENT
                            },
                            .backgroundColor = Clay_Hovered()
                                ? (Clay_Color){130, 176, 248, 128}
                                : (Clay_Color){106, 152, 236, 96},
                            .cornerRadius = CLAY_CORNER_RADIUS(2)
                        }) {}
                    }
                }
            }

            CLAY(CLAY_ID("CUFlameOuter"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIXED(flame_h + 8.0f) },
                    .padding = CLAY_PADDING_ALL(4),
                    .layoutDirection = CLAY_TOP_TO_BOTTOM
                },
                .backgroundColor = {20, 20, 24, 255},
                .cornerRadius = CLAY_CORNER_RADIUS(3)
            }) {
                CLAY(CLAY_ID("CUFlameCanvas"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIXED(flame_h) }
                    },
                    .backgroundColor = {16, 16, 20, 255},
                    .cornerRadius = CLAY_CORNER_RADIUS(2)
                }) {
                    for (i = 0; i < frame->count && i < CPU_PROF_MAX_ZONES; i++) {
                        double zstart = (double)frame->start_ns[i];
                        double zdur = (double)frame->duration_ns[i];
                        double zend = zstart + zdur;
                        double clip_start;
                        double clip_end;
                        float fx;
                        float fw;
                        float fy;
                        Clay_Color c;

                        if (zdur == 0) continue;
                        if (zend <= view_start || zstart >= view_end) continue;

                        clip_start = zstart < view_start ? view_start : zstart;
                        clip_end = zend > view_end ? view_end : zend;
                        if (clip_end <= clip_start) continue;

                        fx = (float)(((clip_start - view_start) / view_span) * (double)flame_w);
                        fw = (float)(((clip_end - clip_start) / view_span) * (double)flame_w);
                        if (fw < 1.0f) fw = 1.0f;
                        if (fx < 0.0f) fx = 0.0f;
                        if (fx + fw > flame_w) fw = flame_w - fx;
                        if (fw <= 0.0f) continue;
                        fy = 2.0f + (float)frame->depth[i] * (bar_h + bar_gap);

                        c = cpu_prof_flame_color(frame->names[i], frame->depth[i]);

                        CLAY(CLAY_IDI("CUFlameBar", i), {
                            .layout = { .sizing = { CLAY_SIZING_FIXED(fw), CLAY_SIZING_FIXED(bar_h) } },
                            .floating = {
                                .attachTo = CLAY_ATTACH_TO_PARENT,
                                .attachPoints = {
                                    .element = CLAY_ATTACH_POINT_LEFT_TOP,
                                    .parent = CLAY_ATTACH_POINT_LEFT_TOP
                                },
                                .offset = { fx, fy },
                                .clipTo = CLAY_CLIP_TO_ATTACHED_PARENT
                            },
                            .backgroundColor = Clay_Hovered()
                                ? (Clay_Color){(uint8_t)(c.r + 18), (uint8_t)(c.g + 18), (uint8_t)(c.b + 18), 255}
                                : c,
                            .cornerRadius = CLAY_CORNER_RADIUS(2)
                        }) {
                            if (fw > 56.0f && frame->names[i]) {
                                Clay_String ns = {false, (int32_t)strlen(frame->names[i]), (char*)frame->names[i]};
                                CLAY_TEXT(ns, CLAY_TEXT_CONFIG({.textColor = {236, 236, 242, 255}, .fontSize = 12}));
                            }
                            if (Clay_Hovered()) {
                                hovered_zone = i;
                                if (e->cpu_prof_click) clicked_zone = i;
                            }
                        }
                    }
                }
            }

            if (hovered_zone >= 0 && hovered_zone < frame->count) {
                uint64_t duration_ns = frame->duration_ns[hovered_zone];
                if (duration_ns > 1000000ULL) {
                    snprintf(hover_line, 192, "Hovered: %s | %.3f ms | depth %u",
                             frame->names[hovered_zone] ? frame->names[hovered_zone] : "(null)",
                             (double)duration_ns / 1000000.0,
                             (unsigned)frame->depth[hovered_zone]);
                } else {
                    snprintf(hover_line, 192, "Hovered: %s | %.2f us | depth %u",
                             frame->names[hovered_zone] ? frame->names[hovered_zone] : "(null)",
                             (double)duration_ns / 1000.0,
                             (unsigned)frame->depth[hovered_zone]);
                }
                {
                    Clay_String hs = {false, (int32_t)strlen(hover_line), hover_line};
                    CLAY_TEXT(hs, CLAY_TEXT_CONFIG({.textColor = {196, 206, 228, 255}, .fontSize = 13}));
                }
            }

            snprintf(list_info, 128, "Calls in zoom: %d / %d", zoom_visible_count, zone_count);
            {
                Clay_String li = {false, (int32_t)strlen(list_info), list_info};
                CLAY_TEXT(li, CLAY_TEXT_CONFIG({.textColor = {164, 172, 188, 255}, .fontSize = 13}));
            }

            /* Column headers for zoom-filtered nested call tree. */
            CLAY(CLAY_ID("CUHeader"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIT({0}) },
                    .padding = { .left = 8, .right = 8, .top = 4, .bottom = 4 },
                    .childGap = 8,
                    .layoutDirection = CLAY_LEFT_TO_RIGHT
                }
            }) {
                CLAY(CLAY_IDI("CUHCol", 0), { .layout = { .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIT({0}) } } }) {
                    Clay_String h = CLAY_STRING("Name (nested)");
                    CLAY_TEXT(h, CLAY_TEXT_CONFIG({.textColor = {150, 150, 160, 255}, .fontSize = 14}));
                }
                CLAY(CLAY_IDI("CUHCol", 1), { .layout = { .sizing = { CLAY_SIZING_FIXED(100), CLAY_SIZING_FIT({0}) }, .childAlignment = { CLAY_ALIGN_X_RIGHT, CLAY_ALIGN_Y_CENTER } } }) {
                    Clay_String h = CLAY_STRING("Duration");
                    CLAY_TEXT(h, CLAY_TEXT_CONFIG({.textColor = {150, 150, 160, 255}, .fontSize = 14}));
                }
                CLAY(CLAY_IDI("CUHCol", 2), { .layout = { .sizing = { CLAY_SIZING_FIXED(60), CLAY_SIZING_FIT({0}) }, .childAlignment = { CLAY_ALIGN_X_RIGHT, CLAY_ALIGN_Y_CENTER } } }) {
                    Clay_String h = CLAY_STRING("Depth");
                    CLAY_TEXT(h, CLAY_TEXT_CONFIG({.textColor = {150, 150, 160, 255}, .fontSize = 14}));
                }
                CLAY(CLAY_IDI("CUHCol", 3), { .layout = { .sizing = { CLAY_SIZING_FIXED(100), CLAY_SIZING_FIT({0}) } } }) {
                    Clay_String h = CLAY_STRING("Parent");
                    CLAY_TEXT(h, CLAY_TEXT_CONFIG({.textColor = {150, 150, 160, 255}, .fontSize = 14}));
                }
            }

            /* Scrollable nested zone list, filtered to visible zoom window. */
            CLAY(CLAY_ID("CUScroll"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_GROW({0}) },
                    .childGap = 2,
                    .layoutDirection = CLAY_TOP_TO_BOTTOM
                },
                .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() }
            }) {
                if (zoom_visible_count <= 0) {
                    CLAY(CLAY_ID("CUZoomEmpty"), {
                        .layout = {
                            .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIT({0}) },
                            .padding = { .left = 8, .right = 8, .top = 6, .bottom = 6 }
                        }
                    }) {
                        Clay_String zs = CLAY_STRING("No calls intersect this zoom range");
                        CLAY_TEXT(zs, CLAY_TEXT_CONFIG({.textColor = {156, 162, 176, 255}, .fontSize = 13}));
                    }
                } else {
                    int row_count = 0;
                    int root_count = 0;
                    int stack_count = 0;
                    int r = root_first;
                    while (r >= 0 && root_count < zone_count) {
                        if (zoom_visible[r]) {
                            root_order[root_count++] = (uint16_t)r;
                        }
                        r = root_next[r];
                    }
                    for (i = root_count - 1; i >= 0; i--) {
                        tree_stack_idx[stack_count] = root_order[i];
                        tree_stack_depth[stack_count] = 0;
                        stack_count++;
                    }

                    while (stack_count > 0 && row_count < zone_count) {
                        uint16_t idx;
                        uint16_t tree_depth;
                        uint16_t parent;
                        const char *name;
                        int child_idx;
                        int child_count = 0;
                        int has_visible_children = 0;
                        int toggle_clicked = 0;
                        int row_hovered = 0;
                        uint16_t indent_px;

                        stack_count--;
                        idx = tree_stack_idx[stack_count];
                        tree_depth = tree_stack_depth[stack_count];
                        if (idx >= (uint16_t)zone_count) continue;
                        if (!zoom_visible[idx]) continue;

                        child_idx = first_child[idx];
                        while (child_idx >= 0 && child_count < zone_count) {
                            if (zoom_visible[child_idx]) {
                                child_order[child_count++] = (uint16_t)child_idx;
                                has_visible_children = 1;
                            }
                            child_idx = next_sibling[child_idx];
                        }

                        parent = frame->parent[idx];
                        name = frame->names[idx] ? frame->names[idx] : "(null)";
                        indent_px = (uint16_t)(tree_depth * 12);
                        if (indent_px > 240) indent_px = 240;

                        CLAY(CLAY_IDI("CURow", row_count), {
                            .layout = {
                                .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIT({0}) },
                                .padding = { .left = 8, .right = 8, .top = 3, .bottom = 3 },
                                .childGap = 8,
                                .layoutDirection = CLAY_LEFT_TO_RIGHT
                            },
                            .backgroundColor = (idx == hovered_zone)
                                ? (Clay_Color){48, 58, 78, 255}
                                : ((row_count % 2 == 0) ? (Clay_Color){35, 35, 40, 255} : (Clay_Color){30, 30, 35, 255})
                        }) {
                            row_hovered = Clay_Hovered();

                            CLAY(CLAY_IDI("CUName", row_count), {
                                .layout = {
                                    .sizing = { CLAY_SIZING_GROW({0}), CLAY_SIZING_FIT({0}) },
                                    .padding = { .left = indent_px, .right = 0, .top = 0, .bottom = 0 },
                                    .childGap = 4,
                                    .layoutDirection = CLAY_LEFT_TO_RIGHT,
                                    .childAlignment = { CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER }
                                }
                            }) {
                                const char *toggle_label = " ";
                                if (has_visible_children) {
                                    toggle_label = e->cpu_prof_tree_collapsed[idx]
                                        ? UI_ICON_ARROW_RIGHT
                                        : UI_ICON_ARROW_DOWN;
                                }
                                CLAY(CLAY_IDI("CUToggle", idx), {
                                    .layout = { .sizing = { CLAY_SIZING_FIXED(10), CLAY_SIZING_FIT({0}) } }
                                }) {
                                    Clay_String ts = {false, (int32_t)strlen(toggle_label), (char *)toggle_label};
                                    CLAY_TEXT(ts, CLAY_TEXT_CONFIG({
                                        .textColor = has_visible_children
                                            ? ((Clay_Color){206, 214, 230, 255})
                                            : ((Clay_Color){120, 124, 134, 255}),
                                        .fontSize = 13
                                    }));
                                    if (has_visible_children && Clay_Hovered() && e->cpu_prof_click) {
                                        e->cpu_prof_tree_collapsed[idx] = e->cpu_prof_tree_collapsed[idx] ? 0 : 1;
                                        toggle_clicked = 1;
                                    }
                                }
                                {
                                    Clay_String ns = {false, (int32_t)strlen(name), (char*)name};
                                    CLAY_TEXT(ns, CLAY_TEXT_CONFIG({.textColor = {200, 200, 210, 255}, .fontSize = 14}));
                                }
                            }

                            CLAY(CLAY_IDI("CUDur", row_count), { .layout = { .sizing = { CLAY_SIZING_FIXED(100), CLAY_SIZING_FIT({0}) }, .childAlignment = { CLAY_ALIGN_X_RIGHT, CLAY_ALIGN_Y_CENTER } } }) {
                                uint64_t duration_ns = frame->duration_ns[idx];
                                if (duration_ns > 1000000ULL)
                                    snprintf(bufs[row_count][0], sizeof(bufs[row_count][0]), "%.2f ms", (double)duration_ns / 1000000.0);
                                else
                                    snprintf(bufs[row_count][0], sizeof(bufs[row_count][0]), "%.1f us", (double)duration_ns / 1000.0);
                                {
                                    Clay_String vs = {false, (int32_t)strlen(bufs[row_count][0]), bufs[row_count][0]};
                                    CLAY_TEXT(vs, CLAY_TEXT_CONFIG({.textColor = {255, 200, 120, 255}, .fontSize = 14}));
                                }
                            }
                            CLAY(CLAY_IDI("CUDep", row_count), { .layout = { .sizing = { CLAY_SIZING_FIXED(60), CLAY_SIZING_FIT({0}) }, .childAlignment = { CLAY_ALIGN_X_RIGHT, CLAY_ALIGN_Y_CENTER } } }) {
                                snprintf(bufs[row_count][1], sizeof(bufs[row_count][1]), "%u", (unsigned)frame->depth[idx]);
                                {
                                    Clay_String vs = {false, (int32_t)strlen(bufs[row_count][1]), bufs[row_count][1]};
                                    CLAY_TEXT(vs, CLAY_TEXT_CONFIG({.textColor = {170, 170, 180, 255}, .fontSize = 14}));
                                }
                            }
                            CLAY(CLAY_IDI("CUPar", row_count), { .layout = { .sizing = { CLAY_SIZING_FIXED(100), CLAY_SIZING_FIT({0}) } } }) {
                                if (parent == CPU_PROF_INVALID_PARENT || parent >= (uint16_t)zone_count) {
                                    snprintf(bufs[row_count][2], sizeof(bufs[row_count][2]), "ROOT");
                                } else {
                                    const char *pname = frame->names[parent] ? frame->names[parent] : "(null)";
                                    snprintf(bufs[row_count][2], sizeof(bufs[row_count][2]), "%s", pname);
                                }
                                {
                                    Clay_String vs = {false, (int32_t)strlen(bufs[row_count][2]), bufs[row_count][2]};
                                    CLAY_TEXT(vs, CLAY_TEXT_CONFIG({.textColor = {170, 170, 180, 255}, .fontSize = 14}));
                                }
                            }

                            if (row_hovered) {
                                hovered_zone = idx;
                                if (e->cpu_prof_click && !toggle_clicked) clicked_zone = idx;
                            }
                        }

                        if (has_visible_children && !e->cpu_prof_tree_collapsed[idx]) {
                            for (j = child_count - 1; j >= 0 && stack_count < zone_count; j--) {
                                tree_stack_idx[stack_count] = child_order[j];
                                tree_stack_depth[stack_count] = tree_depth + 1;
                                stack_count++;
                            }
                        }
                        row_count++;
                    }
                }
            }
            }
        }
    }

    commands = Clay_EndLayout();
    if (!e->cpu_prof_mouse_down) {
        e->cpu_prof_minimap_dragging = 0;
    }
    if (minimap_has_view) {
        Clay_ElementData mm = Clay_GetElementData(CLAY_ID("CUFlameMiniMap"));
        if (mm.found) {
            float ratio = 0.5f;
            float view_ratio = minimap_view_ratio;
            float view_start_ratio;

            if (view_ratio < 0.0f) view_ratio = 0.0f;
            if (view_ratio > 1.0f) view_ratio = 1.0f;

            ratio = (e->cpu_prof_mouse_x - mm.boundingBox.x) /
                    (mm.boundingBox.width > 1.0f ? mm.boundingBox.width : 1.0f);
            if (ratio < 0.0f) ratio = 0.0f;
            if (ratio > 1.0f) ratio = 1.0f;

            view_start_ratio = e->cpu_prof_flame_center - view_ratio * 0.5f;
            if (view_start_ratio < 0.0f) view_start_ratio = 0.0f;
            if (view_start_ratio + view_ratio > 1.0f) view_start_ratio = 1.0f - view_ratio;
            if (view_start_ratio < 0.0f) view_start_ratio = 0.0f;

            if (e->cpu_prof_minimap_dragging && e->cpu_prof_mouse_down && view_ratio < 0.999f) {
                float new_start = ratio - e->cpu_prof_minimap_drag_offset * view_ratio;
                if (new_start < 0.0f) new_start = 0.0f;
                if (new_start + view_ratio > 1.0f) new_start = 1.0f - view_ratio;
                if (new_start < 0.0f) new_start = 0.0f;
                e->cpu_prof_flame_center = new_start + view_ratio * 0.5f;
            }

            if (e->cpu_prof_click &&
                e->cpu_prof_mouse_x >= mm.boundingBox.x &&
                e->cpu_prof_mouse_x <= mm.boundingBox.x + mm.boundingBox.width &&
                e->cpu_prof_mouse_y >= mm.boundingBox.y &&
                e->cpu_prof_mouse_y <= mm.boundingBox.y + mm.boundingBox.height) {
                if (view_ratio < 0.999f &&
                    ratio >= view_start_ratio &&
                    ratio <= view_start_ratio + view_ratio) {
                    e->cpu_prof_minimap_dragging = 1;
                    e->cpu_prof_minimap_drag_offset = (ratio - view_start_ratio) /
                                                      (view_ratio > 0.0001f ? view_ratio : 1.0f);
                } else {
                    e->cpu_prof_flame_center = ratio;
                    if (view_ratio < 1.0f) {
                        if (e->cpu_prof_flame_center < view_ratio * 0.5f)
                            e->cpu_prof_flame_center = view_ratio * 0.5f;
                        if (e->cpu_prof_flame_center > 1.0f - view_ratio * 0.5f)
                            e->cpu_prof_flame_center = 1.0f - view_ratio * 0.5f;
                    } else {
                        e->cpu_prof_flame_center = 0.5f;
                    }
                    e->cpu_prof_minimap_dragging = (view_ratio < 0.999f) ? 1 : 0;
                    e->cpu_prof_minimap_drag_offset = 0.5f;
                }
            }
        } else {
            e->cpu_prof_minimap_dragging = 0;
        }
    } else {
        e->cpu_prof_minimap_dragging = 0;
    }
    {
        Clay_ElementData fe = Clay_GetElementData(CLAY_ID("CUFlameCanvas"));
        if (fe.found) {
            e->cpu_prof_flame_x = fe.boundingBox.x;
            e->cpu_prof_flame_y = fe.boundingBox.y;
            e->cpu_prof_flame_w = fe.boundingBox.width;
            e->cpu_prof_flame_h = fe.boundingBox.height;
        } else {
            e->cpu_prof_flame_x = 0.0f;
            e->cpu_prof_flame_y = 0.0f;
            e->cpu_prof_flame_w = 0.0f;
            e->cpu_prof_flame_h = 0.0f;
        }
    }
    if (frame && hovered_zone >= 0 && hovered_zone < frame->count && frame->names[hovered_zone]) {
        snprintf(e->cpu_prof_hover_zone_name, sizeof(e->cpu_prof_hover_zone_name), "%s",
                 frame->names[hovered_zone]);
        e->cpu_prof_hover_zone_active = 1;
    }
    if (frame && clicked_zone >= 0 && clicked_zone < frame->count && frame->names[clicked_zone]) {
        snprintf(e->cpu_prof_selected_zone_name, sizeof(e->cpu_prof_selected_zone_name), "%s",
                 frame->names[clicked_zone]);
        e->cpu_prof_selected_zone_active = 1;
    }
    if (history_count > 0 && e->cpu_prof_click) {
        Clay_ElementData tl = Clay_GetElementData(CLAY_ID("CUTimelineStrip"));
        if (tl.found) {
            float mx = e->cpu_prof_mouse_x;
            float my = e->cpu_prof_mouse_y;
            if (mx >= tl.boundingBox.x && mx <= tl.boundingBox.x + tl.boundingBox.width &&
                my >= tl.boundingBox.y && my <= tl.boundingBox.y + tl.boundingBox.height) {
                float rel = (mx - tl.boundingBox.x) / (tl.boundingBox.width > 1.0f ? tl.boundingBox.width : 1.0f);
                int idx = (int)(rel * (float)history_count);
                if (idx < 0) idx = 0;
                if (idx >= history_count) idx = history_count - 1;
                e->cpu_prof_timeline_paused = 1;
                e->cpu_prof_timeline_offset = history_count - 1 - idx;
                e->cpu_prof_timeline_frame_id =
                    cpu_prof_get_frame_id_at_offset((uint16_t)e->cpu_prof_timeline_offset);
            }
        }
    }
    e->cpu_prof_cmd_count = commands.length;
    e->cpu_prof_cmd_array = commands.internalArray;
    e->cpu_prof_click = 0;
}

EXPORT void update_editor(game_state *gs, editor_state *es) {
    editor_state *e = es;
    dock_state *d = (dock_state *)e->dock;

    if (!e->open) return;

    EDITOR_CPU_ZONE_BEGIN(e, "editor_update_frame");
    EDITOR_CACHE_ZONE_BEGIN("editor_update_frame");

    /* ── Dock layout — compute pixel rects for all windows ── */
    EDITOR_CPU_ZONE_BEGIN(e, "editor_dock_layout");
    {
        int wi;
        for (wi = 0; wi < MAX_DOCK_WINDOWS; wi++) {
            int ww, wh;
            if (!d->windows[wi].in_use || !d->windows[wi].sdl_window) continue;
            SDL_GetWindowSize((SDL_Window *)d->windows[wi].sdl_window, &ww, &wh);
            dock_layout(d, wi, ww, wh);
        }
    }
    EDITOR_CPU_ZONE_END(e);

    /* ── Game panel size (for ortho projection in engine) ── */
    EDITOR_CPU_ZONE_BEGIN(e, "editor_game_panel_size");
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
    EDITOR_CPU_ZONE_END(e);

    /* ── Editor panel rect (for coordinate transforms) ── */
    EDITOR_CPU_ZONE_BEGIN(e, "editor_panel_rect");
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
    EDITOR_CPU_ZONE_END(e);

    /* ── Profiler Clay layout (produces render commands for externals) ── */
    EDITOR_CPU_ZONE_BEGIN(e, "editor_layout_prepass");
    {
        int scene_count = gs->scene_entities ? gs->scene_entity_count : 0;
        editor_scene_selection_sync(e, scene_count);
    }
    EDITOR_CPU_ZONE_END(e);
    EDITOR_CPU_ZONE_BEGIN(e, "layout_memory_profiler");
    profiler_layout(gs, es);
    EDITOR_CPU_ZONE_END(e);
    EDITOR_CPU_ZONE_BEGIN(e, "layout_scene_tree");
    scene_tree_layout(gs, es);
    EDITOR_CPU_ZONE_END(e);
    EDITOR_CPU_ZONE_BEGIN(e, "layout_project_browser");
    project_browser_layout(gs, es);
    EDITOR_CPU_ZONE_END(e);
    EDITOR_CPU_ZONE_BEGIN(e, "layout_inspector");
    inspector_layout(gs, es);
    EDITOR_CPU_ZONE_END(e);
    EDITOR_CPU_ZONE_BEGIN(e, "layout_menu_bar");
    menu_bar_layout(gs, es);
    EDITOR_CPU_ZONE_END(e);
    EDITOR_CPU_ZONE_BEGIN(e, "layout_editor_toolbar");
    editor_toolbar_layout(gs, es);
    EDITOR_CPU_ZONE_END(e);
    EDITOR_CPU_ZONE_BEGIN(e, "layout_cache_profiler");
    cache_profiler_layout(gs, es);
    EDITOR_CPU_ZONE_END(e);
    EDITOR_CPU_ZONE_BEGIN(e, "layout_cpu_profiler");
    cpu_profiler_layout(gs, es);
    EDITOR_CPU_ZONE_END(e);

    /* ── Camera, gizmo, lines (existing editor behavior) ── */
    EDITOR_CPU_ZONE_BEGIN(e, "editor_camera_and_gizmo");
    if (!editor_is_play_mode(gs)) {
        update_camera(gs, es);
        update_gizmo_hover(gs, es);
    } else {
        if (e->cam_mouse_look) editor_end_mouse_look(e);
        e->gizmo_hovered = GIZMO_NONE;
        e->gizmo_active = GIZMO_NONE;
        e->gizmo_entity_index = -1;
    }
    EDITOR_CPU_ZONE_END(e);
    EDITOR_CPU_ZONE_BEGIN(e, "editor_build_lines");
    build_lines(gs, es);
    EDITOR_CPU_ZONE_END(e);

    EDITOR_CPU_ZONE_BEGIN(e, "editor_layout_state_save");
    if (d && editor_refresh_layout_path(gs, e)) {
        uint64_t current_hash = editor_dock_layout_hash(d);
        if (!e->dock_layout_hash_valid) {
            e->dock_layout_last_hash = current_hash;
            e->dock_layout_hash_valid = 1;
            e->dock_layout_save_accum = 0.0f;
        } else if (current_hash != e->dock_layout_last_hash) {
            e->dock_layout_save_accum += gs->dt;
            if (e->dock_layout_save_accum >= 0.25f) {
                if (editor_save_layout_state(gs, e)) {
                    e->dock_layout_last_hash = current_hash;
                } else {
                    /* Avoid repeated save spam; retry on the next user-visible change. */
                    e->dock_layout_last_hash = current_hash;
                }
                e->dock_layout_save_accum = 0.0f;
            }
        } else {
            e->dock_layout_save_accum = 0.0f;
        }
    } else {
        e->dock_layout_hash_valid = 0;
        e->dock_layout_save_accum = 0.0f;
    }
    EDITOR_CPU_ZONE_END(e);

    EDITOR_CACHE_ZONE_END();
    EDITOR_CPU_ZONE_END(e);
}

EXPORT void destroy_editor(game_state *gs, editor_state *es) {
    if (!gs || !es) return;
    if (editor_save_layout_state(gs, es)) {
        fprintf(stderr, "Editor layout saved to %s\n", es->editor_layout_path);
    }
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
            int should_remove_from_source = 0;
            int remove_node = src_node;
            error_value err = ERRV_OK;

            if (zone == DROP_CENTER) {
                if (src_win != win || src_node != tgt) {
                    int new_tab_idx = dock_add_panel_with_error(d, tgt, panel, &err);
                    if (new_tab_idx >= 0) {
                        dock_set_active_tab(d, tgt, new_tab_idx);
                        should_remove_from_source = 1;
                    } else {
                        editor_log_error_value("dock_drop:center:add_panel", err);
                    }
                }
            } else {
                int existing_leaf = -1;
                if (dock_split_at_with_error(d, win, tgt, panel, zone, NULL, &existing_leaf, &err) == 0) {
                    should_remove_from_source = 1;
                    if (src_win == win && src_node == tgt && existing_leaf >= 0) {
                        remove_node = existing_leaf;
                    }
                } else {
                    editor_log_error_value("dock_drop:split:split_at", err);
                }
            }

            if (should_remove_from_source) {
                err = ERRV_OK;
                if (dock_remove_panel_with_error(d, remove_node, panel, &err) == 0) {
                    int new_root = -1;
                    err = ERRV_OK;
                    if (dock_collapse_empty_with_error(d,
                                                       d->windows[src_win].root_node,
                                                       &new_root,
                                                       &err) == 0) {
                        d->windows[src_win].root_node = new_root;
                        if (d->windows[src_win].root_node < 0 && src_win != 0) {
                            d->cmd_cleanup_windows = 1;
                        }
                    } else {
                        editor_log_error_value("dock_drop:source:collapse_empty", err);
                    }
                } else {
                    editor_log_error_value("dock_drop:source:remove_panel", err);
                }
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
        int cmd_or_ctrl = ((ev->key.mod & SDL_KMOD_GUI) || (ev->key.mod & SDL_KMOD_CTRL));
        if (key_window == (SDL_Window *)e->window && cmd_or_ctrl) {
            if (ev->key.key == SDLK_S) {
                if (!gs->project_loaded || !gs->project_path[0]) {
                    fprintf(stderr, "Save skipped: no project file loaded\n");
                } else if (editor_save_scene_to_toml(gs, gs->project_path)) {
                    fprintf(stderr, "Scene saved to %s\n", gs->project_path);
                    if (editor_save_layout_state(gs, e)) {
                        e->dock_layout_last_hash = editor_dock_layout_hash((dock_state *)e->dock);
                        e->dock_layout_hash_valid = 1;
                        e->dock_layout_save_accum = 0.0f;
                        fprintf(stderr, "Editor layout saved to %s\n", e->editor_layout_path);
                    }
                } else {
                    fprintf(stderr, "Save failed for %s\n", gs->project_path);
                }
                return 1;
            }
            if (ev->key.key == SDLK_P) {
                editor_set_play_mode(gs, e, !editor_is_play_mode(gs));
                return 1;
            }
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
                int picked = -1;
                int shift_down = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
                if (editor_toolbar_contains_point(e, lx, ly)) return 1;
                if (editor_pick_scene_entity(gs, e, lx, ly, &picked)) {
                    editor_scene_selection_apply_click(e, picked, shift_down);
                    return 1;
                }
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
            if (!cc) return 0;
            if (axis_scale < 0.001f) axis_scale = 1.0f;
            local_delta = world_delta / axis_scale;
            if (e->gizmo_drag_mode == 1) {
                cc->radius = fmaxf(0.05f, e->gizmo_drag_start_capsule_radius + local_delta);
            } else {
                cc->half_height = fmaxf(0.05f, e->gizmo_drag_start_capsule_half_height + local_delta);
            }
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
            if (panel_event_hit(d, PANEL_CACHE_PROFILER, evwin, ev->motion.x, ev->motion.y, &lx, &ly)) {
                e->cache_prof_mouse_x = lx;
                e->cache_prof_mouse_y = ly;
            }
            if (panel_event_hit(d, PANEL_CPU_PROFILER, evwin, ev->motion.x, ev->motion.y, &lx, &ly)) {
                e->cpu_prof_mouse_x = lx;
                e->cpu_prof_mouse_y = ly;
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
            if (panel_event_hit(d, PANEL_CACHE_PROFILER, evwin, mx, my, NULL, NULL)) {
                e->cache_prof_scroll_y += ev->wheel.y * 3.0f;
                consumed = 1;
            }
            {
                float lx = 0.0f, ly = 0.0f;
                if (panel_event_hit(d, PANEL_CPU_PROFILER, evwin, mx, my, &lx, &ly)) {
                    int in_flame = 0;
                    float pan_input = 0.0f;
                    int shift_down = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
                    if (e->cpu_prof_flame_w > 1.0f && e->cpu_prof_flame_h > 1.0f) {
                        if (lx >= e->cpu_prof_flame_x &&
                            lx <= e->cpu_prof_flame_x + e->cpu_prof_flame_w &&
                            ly >= e->cpu_prof_flame_y &&
                            ly <= e->cpu_prof_flame_y + e->cpu_prof_flame_h) {
                            in_flame = 1;
                        }
                    }
                    if (in_flame) {
                        pan_input = ev->wheel.x;
                        if (shift_down && pan_input == 0.0f) pan_input = ev->wheel.y;
                        if (pan_input != 0.0f) e->cpu_prof_flame_pan_wheel += pan_input;
                        if (!shift_down || ev->wheel.x != 0.0f) {
                            if (ev->wheel.y != 0.0f) e->cpu_prof_flame_zoom_wheel += ev->wheel.y;
                        }
                    } else {
                        e->cpu_prof_scroll_y += ev->wheel.y * 3.0f;
                    }
                    consumed = 1;
                }
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
                e->scene_tree_click_shift = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
            }

            e->project_browser_mouse_down = panel_event_hit(d, PANEL_ASSETS, evwin,
                ev->button.x, ev->button.y, &lx, &ly);
            if (e->project_browser_mouse_down) {
                e->project_browser_mouse_x = lx;
                e->project_browser_mouse_y = ly;
                e->project_browser_click = 1;
            }

            e->cache_prof_mouse_down = panel_event_hit(d, PANEL_CACHE_PROFILER, evwin,
                ev->button.x, ev->button.y, &lx, &ly);
            if (e->cache_prof_mouse_down) {
                e->cache_prof_mouse_x = lx;
                e->cache_prof_mouse_y = ly;
                e->cache_prof_click = 1;
            }

            e->cpu_prof_mouse_down = panel_event_hit(d, PANEL_CPU_PROFILER, evwin,
                ev->button.x, ev->button.y, &lx, &ly);
            if (e->cpu_prof_mouse_down) {
                e->cpu_prof_mouse_x = lx;
                e->cpu_prof_mouse_y = ly;
                e->cpu_prof_click = 1;
            }
        }
    }
    if (ev->type == SDL_EVENT_MOUSE_BUTTON_UP && ev->button.button == SDL_BUTTON_LEFT) {
        e->prof_mouse_down = 0;
        e->scene_tree_mouse_down = 0;
        e->scene_tree_click_shift = 0;
        e->project_browser_mouse_down = 0;
        e->cache_prof_mouse_down = 0;
        e->cpu_prof_mouse_down = 0;
        e->cpu_prof_minimap_dragging = 0;
    }

    return 0;
}
