/* ── DOD Existential Processing Benchmark ──────────────────────────────────
   Measures the hot paths changed by the DOD Chapter 3 refactor:
     1. Component lookup (find_* via index table)
     2. Physics queries (query_combat_targets, query_movement_obstacles)
     3. Trigger update with swap-and-pop
   Also measures the OLD O(n) scan approach for comparison.

   Build:  lib/tcc/macos/tcc -Blib/tcc/macos -DMAC_OS_X_VERSION_MIN_REQUIRED=1100
           -o build/Debug/bench_dod -Isrc -Isrc/engine -Isrc/editor
           -Ilib/SDL3/include -Ilib/cgltf tests/bench_dod.c
   Run:    build/Debug/bench_dod
   ──────────────────────────────────────────────────────────────────────── */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>

#define EXPORT

#include "game.h"
#include "state_migration.gen.h"

/* ── Stubs ─────────────────────────────────────────────────────────────── */

void debug_draw_rect(debug_renderer *dr, vec2 c, float w, float h, debug_color col) {
    (void)dr; (void)c; (void)w; (void)h; (void)col;
}
GltfModel load_glb(const char *path, arena *a) {
    GltfModel m; memset(&m, 0, sizeof(m)); (void)path; (void)a; return m;
}
void load_animations_glb(const char *path, GltfModel *model, arena *a) {
    (void)path; (void)model; (void)a;
}
void gltf_set_gpu_device(void *dev) { (void)dev; }
void debug_draw_line(debug_renderer *dr, vec2 s, vec2 e, debug_color col) {
    (void)dr; (void)s; (void)e; (void)col;
}
const project_mesh *project_find_mesh(const project_data *p, int entity) {
    (void)p; (void)entity; return NULL;
}
const project_anim *project_find_anim(const project_data *p, int entity) {
    (void)p; (void)entity; return NULL;
}
const project_box_collider *project_find_box_collider(const project_data *p, int entity) {
    (void)p; (void)entity; return NULL;
}
const char *project_find_asset(const project_data *p, const char *key, project_asset_type type) {
    (void)p; (void)key; (void)type; return NULL;
}

#include "anim.c"
#include "engine/engine.c"

/* Rename physics symbols to avoid collision with engine.c */
#define find_transform_component     phys_find_transform_component
#define find_parent_transform_component phys_find_parent_transform_component
#define find_health_component        phys_find_health_component
#define find_velocity_component      phys_find_velocity_component
#define find_rigid_body_component    phys_find_rigid_body_component
#define find_character_controller_component phys_find_character_controller_component
#define find_box_collider_component  phys_find_box_collider_component
#define find_capsule_collider_component phys_find_capsule_collider_component
#define find_camera_component        phys_find_camera_component
#define find_mesh_component          phys_find_mesh_component
#define resolve_world_position       phys_resolve_world_position
#define resolve_parent_world_offset  phys_resolve_parent_world_offset
#include "engine/physics.c"
#undef find_transform_component
#undef find_parent_transform_component
#undef find_health_component
#undef find_velocity_component
#undef find_rigid_body_component
#undef find_character_controller_component
#undef find_box_collider_component
#undef find_capsule_collider_component
#undef find_camera_component
#undef find_mesh_component
#undef resolve_world_position
#undef resolve_parent_world_offset

/* ── Timer ─────────────────────────────────────────────────────────────── */

static double now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1e6 + (double)tv.tv_usec;
}

/* ── Arena ─────────────────────────────────────────────────────────────── */

#define BENCH_ARENA_SIZE (32 * 1024 * 1024)
static char bench_arena_buf[BENCH_ARENA_SIZE];
static arena bench_arena;

static arena *reset_arena(void) {
    arena_init(&bench_arena, bench_arena_buf, BENCH_ARENA_SIZE);
    return &bench_arena;
}

/* ── Scene Setup ──────────────────────────────────────────────────────── */

#define ENTITY_COUNT 256

static game_state *setup_scene(void) {
    arena *a = reset_arena();
    game_state *gs;
    int i;

    gs = (game_state *)arena_alloc(a, (uint32_t)sizeof(game_state), 16, "gs");
    memset(gs, 0, sizeof(*gs));
    gs->gameplay = arena_alloc_subarena(a, 16 * 1024 * 1024, 16, "gameplay");
    gs->root_arena = a;
    gs->scene_primary_skinned_entity = -1;
    gs->scene_camera_entity = -1;
    gs->dt = 1.0f / 60.0f;
    gs->editor_play_mode = 1;

    /* Allocate component arrays at ENTITY_COUNT capacity */
    gs->scene_entity_capacity = ENTITY_COUNT;
    gs->scene_entities = (entity *)arena_alloc(gs->gameplay,
        (uint32_t)(ENTITY_COUNT * sizeof(entity)), 8, "ent");
    gs->transform_components = (transform_component *)arena_alloc(gs->gameplay,
        (uint32_t)(ENTITY_COUNT * sizeof(transform_component)), 8, "tc");
    gs->transform_component_capacity = ENTITY_COUNT;
    gs->velocity_components = (velocity_component *)arena_alloc(gs->gameplay,
        (uint32_t)(ENTITY_COUNT * sizeof(velocity_component)), 8, "vc");
    gs->velocity_component_capacity = ENTITY_COUNT;
    gs->health_components = (health_component *)arena_alloc(gs->gameplay,
        (uint32_t)(ENTITY_COUNT * sizeof(health_component)), 8, "hc");
    gs->health_component_capacity = ENTITY_COUNT;
    gs->box_collider_components = (box_collider_component *)arena_alloc(gs->gameplay,
        (uint32_t)(ENTITY_COUNT * sizeof(box_collider_component)), 8, "bc");
    gs->box_collider_component_capacity = ENTITY_COUNT;
    gs->capsule_collider_components = (capsule_collider_component *)arena_alloc(gs->gameplay,
        (uint32_t)(ENTITY_COUNT * sizeof(capsule_collider_component)), 8, "cc");
    gs->capsule_collider_component_capacity = ENTITY_COUNT;
    gs->mesh_components = (mesh_component *)arena_alloc(gs->gameplay,
        (uint32_t)(ENTITY_COUNT * sizeof(mesh_component)), 8, "mc");
    gs->mesh_component_capacity = ENTITY_COUNT;
    gs->animation_components = (animation_component *)arena_alloc(gs->gameplay,
        (uint32_t)(ENTITY_COUNT * sizeof(animation_component)), 8, "ac");
    gs->animation_component_capacity = ENTITY_COUNT;
    gs->animation_transition_entries = (animation_transition_entry *)arena_alloc(gs->gameplay,
        (uint32_t)(ENTITY_COUNT * sizeof(animation_transition_entry)), 8, "at");
    gs->animation_transition_capacity = ENTITY_COUNT;
    gs->rigid_body_components = (rigid_body_component *)arena_alloc(gs->gameplay,
        (uint32_t)(ENTITY_COUNT * sizeof(rigid_body_component)), 8, "rb");
    gs->rigid_body_component_capacity = ENTITY_COUNT;
    gs->character_controller_components = (character_controller_component *)arena_alloc(gs->gameplay,
        (uint32_t)(ENTITY_COUNT * sizeof(character_controller_component)), 8, "ccc");
    gs->character_controller_component_capacity = ENTITY_COUNT;
    gs->rotation_components = (rotation_component *)arena_alloc(gs->gameplay,
        (uint32_t)(ENTITY_COUNT * sizeof(rotation_component)), 8, "rc");
    gs->rotation_component_capacity = ENTITY_COUNT;
    gs->scale_components = (scale_component *)arena_alloc(gs->gameplay,
        (uint32_t)(ENTITY_COUNT * sizeof(scale_component)), 8, "sc");
    gs->scale_component_capacity = ENTITY_COUNT;
    gs->parent_transform_components = (parent_transform_component *)arena_alloc(gs->gameplay,
        (uint32_t)(ENTITY_COUNT * sizeof(parent_transform_component)), 8, "ptc");
    gs->parent_transform_component_capacity = ENTITY_COUNT;
    gs->parent_rotation_components = (parent_rotation_component *)arena_alloc(gs->gameplay,
        (uint32_t)(ENTITY_COUNT * sizeof(parent_rotation_component)), 8, "prc");
    gs->parent_rotation_component_capacity = ENTITY_COUNT;
    gs->camera_components = (camera_component *)arena_alloc(gs->gameplay,
        (uint32_t)(ENTITY_COUNT * sizeof(camera_component)), 8, "cam");
    gs->camera_component_capacity = ENTITY_COUNT;
    gs->trigger_components = (trigger_component *)arena_alloc(gs->gameplay,
        (uint32_t)(ENTITY_COUNT * sizeof(trigger_component)), 8, "trig");
    gs->trigger_component_capacity = ENTITY_COUNT;

    /* Draw list */
    gs->dl.meshes = (mesh_draw_command *)arena_alloc(gs->gameplay,
        (uint32_t)(DRAW_LIST_MAX_MESH_COMMANDS * sizeof(mesh_draw_command)), 8, "dlm");
    gs->dl.mesh_capacity = DRAW_LIST_MAX_MESH_COMMANDS;
    gs->dl.sprites = (draw_command *)arena_alloc(gs->gameplay,
        (uint32_t)(8 * sizeof(draw_command)), 8, "dls");
    gs->dl.sprite_capacity = 8;
    gs->dl.lines = (debug_line_command *)arena_alloc(gs->gameplay,
        (uint32_t)(256 * sizeof(debug_line_command)), 8, "dll");
    gs->dl.line_capacity = 256;

    /* Init index arrays */
    memset(gs->parent_index, 0xFF, sizeof(gs->parent_index));
    memset(gs->parent_transform_index, 0xFF, sizeof(gs->parent_transform_index));
    memset(gs->parent_rotation_index, 0xFF, sizeof(gs->parent_rotation_index));
    memset(gs->transform_index, 0xFF, sizeof(gs->transform_index));
    memset(gs->rotation_index, 0xFF, sizeof(gs->rotation_index));
    memset(gs->scale_index, 0xFF, sizeof(gs->scale_index));
    memset(gs->velocity_index, 0xFF, sizeof(gs->velocity_index));
    memset(gs->rigid_body_index, 0xFF, sizeof(gs->rigid_body_index));
    memset(gs->character_controller_index, 0xFF, sizeof(gs->character_controller_index));
    memset(gs->health_index, 0xFF, sizeof(gs->health_index));
    memset(gs->box_collider_index, 0xFF, sizeof(gs->box_collider_index));
    memset(gs->capsule_collider_index, 0xFF, sizeof(gs->capsule_collider_index));
    memset(gs->mesh_index, 0xFF, sizeof(gs->mesh_index));
    memset(gs->animation_index, 0xFF, sizeof(gs->animation_index));
    memset(gs->animation_transition_index, 0xFF, sizeof(gs->animation_transition_index));
    memset(gs->camera_index, 0xFF, sizeof(gs->camera_index));
    memset(gs->trigger_index, 0xFF, sizeof(gs->trigger_index));

    gs->scene_entity_count = ENTITY_COUNT;

    /* Populate: every entity gets a transform.
       Even entities get health + box collider (combat targets / obstacles).
       Entity 0 is the player (CC + velocity + capsule + rigid_body + health). */

    for (i = 0; i < ENTITY_COUNT; i++) {
        float x = (float)(i % 16) * 3.0f;
        float z = (float)(i / 16) * 3.0f;
        push_transform_component(gs, i, VEC3(x, 0.0f, z));
        push_mesh_component(gs, i, 0);
    }

    /* Player entity 0 */
    push_velocity_component(gs, 0, VEC3(0.0f, 0.0f, 0.0f));
    push_rigid_body_component(gs, 0, 1);
    push_character_controller_component(gs, 0, 5.0f, 8.5f);
    push_capsule_collider_component(gs, 0, 0.3f, 0.9f);
    push_health_component(gs, 0, 100.0f, 100.0f);

    /* Even entities 2..ENTITY_COUNT-2: health + box collider (enemies/walls) */
    for (i = 2; i < ENTITY_COUNT; i += 2) {
        push_health_component(gs, i, 50.0f, 50.0f);
        push_box_collider_component(gs, i, (rect){0.0f, 0.0f, 1.0f, 1.0f}, 0.5f);
    }

    /* Camera at last entity */
    push_camera_component(gs, ENTITY_COUNT - 1, 60.0f, 0.1f, 100.0f,
                          VEC3(0,0,0), VEC3(0,1,0));
    gs->scene_camera_entity = ENTITY_COUNT - 1;

    return gs;
}

/* ── Old O(n) scan for comparison ─────────────────────────────────────── */

static transform_component *find_transform_linear(game_state *gs, int entity_index) {
    int i;
    for (i = 0; i < gs->transform_component_count; i++) {
        if (gs->transform_components[i].entity_index == entity_index)
            return &gs->transform_components[i];
    }
    return NULL;
}

static health_component *find_health_linear(game_state *gs, int entity_index) {
    int i;
    for (i = 0; i < gs->health_component_count; i++) {
        if (gs->health_components[i].entity_index == entity_index)
            return &gs->health_components[i];
    }
    return NULL;
}

static box_collider_component *find_box_collider_linear(game_state *gs, int entity_index) {
    int i;
    for (i = 0; i < gs->box_collider_component_count; i++) {
        if (gs->box_collider_components[i].entity_index == entity_index)
            return &gs->box_collider_components[i];
    }
    return NULL;
}

/* Old query_combat_targets: iterate all entities, linear find for each */
static int query_combat_targets_linear(game_state *gs, int exclude,
                                       collider_target_view *out, int max) {
    int i, count = 0;
    for (i = 0; i < gs->scene_entity_count && count < max; i++) {
        health_component *hc;
        transform_component *tc;
        box_collider_component *bc;
        if (i == exclude) continue;
        hc = find_health_linear(gs, i);
        if (!hc) continue;
        tc = find_transform_linear(gs, i);
        if (!tc) continue;
        bc = find_box_collider_linear(gs, i);
        if (!bc) continue;
        out[count].entity_index = i;
        out[count].ent = &gs->scene_entities[i];
        out[count].transform = tc;
        out[count].health = hc;
        out[count].box_collider = bc;
        out[count].capsule_collider = NULL;
        count++;
    }
    return count;
}

/* ── Benchmarks ───────────────────────────────────────────────────────── */

#define ITERS 100000
#define QUERY_MAX 256

static volatile int sink; /* prevent dead-code elimination */

static void bench_lookup_indexed(game_state *gs) {
    double t0, t1;
    int i, iter;
    int found = 0;

    t0 = now_us();
    for (iter = 0; iter < ITERS; iter++) {
        for (i = 0; i < ENTITY_COUNT; i++) {
            int idx = gs->transform_index[i];
            if (idx >= 0) found++;
        }
    }
    t1 = now_us();
    sink = found;

    printf("  Index lookup  (%d entities x %dk):  %8.1f us  (%5.1f ns/lookup)\n",
           ENTITY_COUNT, ITERS / 1000,
           t1 - t0,
           (t1 - t0) * 1000.0 / (double)(ITERS * ENTITY_COUNT));
}

static void bench_lookup_linear(game_state *gs) {
    double t0, t1;
    int i, iter;
    int found = 0;

    t0 = now_us();
    for (iter = 0; iter < ITERS; iter++) {
        for (i = 0; i < ENTITY_COUNT; i++) {
            transform_component *tc = find_transform_linear(gs, i);
            if (tc) found++;
        }
    }
    t1 = now_us();
    sink = found;

    printf("  Linear scan   (%d entities x %dk):  %8.1f us  (%5.1f ns/lookup)\n",
           ENTITY_COUNT, ITERS / 1000,
           t1 - t0,
           (t1 - t0) * 1000.0 / (double)(ITERS * ENTITY_COUNT));
}

static void bench_query_indexed(game_state *gs) {
    double t0, t1;
    int iter;
    collider_target_view targets[QUERY_MAX];
    int total = 0;

    t0 = now_us();
    for (iter = 0; iter < ITERS; iter++) {
        total += query_combat_targets(gs, 0, targets, QUERY_MAX);
    }
    t1 = now_us();
    sink = total;

    printf("  Query indexed (%d iters):           %8.1f us  (%5.1f ns/iter)\n",
           ITERS,
           t1 - t0,
           (t1 - t0) * 1000.0 / (double)ITERS);
}

static void bench_query_linear(game_state *gs) {
    double t0, t1;
    int iter;
    collider_target_view targets[QUERY_MAX];
    int total = 0;

    t0 = now_us();
    for (iter = 0; iter < ITERS; iter++) {
        total += query_combat_targets_linear(gs, 0, targets, QUERY_MAX);
    }
    t1 = now_us();
    sink = total;

    printf("  Query linear  (%d iters):           %8.1f us  (%5.1f ns/iter)\n",
           ITERS,
           t1 - t0,
           (t1 - t0) * 1000.0 / (double)ITERS);
}

static void bench_trigger_update(void) {
    double t0, t1;
    int iter;
    int triggers_processed = 0;

    /* For triggers we need to re-setup each iteration since swap-and-pop mutates state */
    t0 = now_us();
    for (iter = 0; iter < 10000; iter++) {
        game_state *gs = setup_scene();
        int i;

        /* Add pickup triggers on odd entities near player */
        for (i = 1; i < 32; i += 2) {
            gs->transform_components[gs->transform_index[i]].position = VEC3(0.0f, 0.0f, 0.0f);
            push_trigger_component(gs, i, TRIGGER_PICKUP, -1, 2.0f, NULL);
        }
        triggers_processed += gs->trigger_component_count;
        update_triggers(gs);
    }
    t1 = now_us();
    sink = triggers_processed;

    printf("  Trigger swap-and-pop (10k setups):  %8.1f us  (%5.1f us/iter)\n",
           t1 - t0,
           (t1 - t0) / 10000.0);
}

static void bench_apply_movement(game_state *gs) {
    double t0, t1;
    int iter;

    /* Give player some velocity */
    gs->velocity_components[gs->velocity_index[0]].velocity = VEC3(1.0f, 0.0f, 1.0f);

    t0 = now_us();
    for (iter = 0; iter < ITERS; iter++) {
        apply_movement(gs);
        /* Reset position so collisions stay consistent */
        gs->transform_components[gs->transform_index[0]].position = VEC3(0.0f, 0.0f, 0.0f);
        gs->velocity_components[gs->velocity_index[0]].velocity = VEC3(1.0f, 0.0f, 1.0f);
    }
    t1 = now_us();

    printf("  apply_movement (%dk iters):         %8.1f us  (%5.1f ns/iter)\n",
           ITERS / 1000,
           t1 - t0,
           (t1 - t0) * 1000.0 / (double)ITERS);
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(void) {
    game_state *gs;

    printf("\n=== DOD Existential Processing Benchmark ===\n");
    printf("    %d entities, %d iterations\n\n", ENTITY_COUNT, ITERS);

    gs = setup_scene();

    printf("Component Lookup (transform, all entities):\n");
    bench_lookup_indexed(gs);
    bench_lookup_linear(gs);

    printf("\nCombat Target Query (health + collider):\n");
    bench_query_indexed(gs);
    bench_query_linear(gs);

    printf("\nPhysics (apply_movement with wall slide):\n");
    bench_apply_movement(gs);

    printf("\nTrigger System (setup + swap-and-pop):\n");
    bench_trigger_update();

    printf("\n");
    return 0;
}
