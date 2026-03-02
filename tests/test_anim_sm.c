/* ── Anim SM integration tests ─────────────────────────────────────────────
   Standalone test binary — no SDL, no GPU needed.
   Tests the tables-as-states animation state machine pipeline.

   Build:  lib/tcc/macos/tcc -Blib/tcc/macos -DMAC_OS_X_VERSION_MIN_REQUIRED=1100
           -o build/Debug/test_anim_sm -Isrc -Isrc/engine -Isrc/editor
           -Ilib/SDL3/include -Ilib/cgltf tests/test_anim_sm.c
   Run:    build/Debug/test_anim_sm
   ──────────────────────────────────────────────────────────────────────── */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Stub EXPORT before any engine headers */
#define EXPORT

#include "game.h"
#include "state_migration.gen.h"

/* ── Stubs for external deps (physics, gltf_loader) ───────────────────── */

void debug_draw_rect(debug_renderer *dr, vec2 center, float width, float height, debug_color color) {
    (void)dr; (void)center; (void)width; (void)height; (void)color;
}
GltfModel load_glb(const char *path, arena *a) {
    GltfModel m; memset(&m, 0, sizeof(m)); (void)path; (void)a; return m;
}
void load_animations_glb(const char *path, GltfModel *model, arena *a) {
    (void)path; (void)model; (void)a;
}
void gltf_set_gpu_device(void *dev) { (void)dev; }
void debug_draw_line(debug_renderer *dr, vec2 start, vec2 end, debug_color color) {
    (void)dr; (void)start; (void)end; (void)color;
}

/* Stubs for project find helpers (engine.c calls these but tests don't
   exercise build_scene_from_project, so trivial implementations suffice) */
const project_mesh *project_find_mesh(const project_data *p, int entity) {
    int i; if (!p) return NULL;
    for (i = 0; i < p->mesh_count; i++)
        if (p->meshes[i].entity == entity) return &p->meshes[i];
    return NULL;
}
const project_anim *project_find_anim(const project_data *p, int entity) {
    int i; if (!p) return NULL;
    for (i = 0; i < p->anim_count; i++)
        if (p->anims[i].entity == entity) return &p->anims[i];
    return NULL;
}
const project_box_collider *project_find_box_collider(const project_data *p, int entity) {
    int i; if (!p) return NULL;
    for (i = 0; i < p->box_collider_count; i++)
        if (p->box_colliders[i].entity == entity) return &p->box_colliders[i];
    return NULL;
}
const char *project_find_asset(const project_data *p, const char *key, project_asset_type type) {
    int i; if (!p || !key || !key[0]) return NULL;
    for (i = 0; i < p->asset_count; i++)
        if (p->assets[i].type == type && strcmp(p->assets[i].key, key) == 0)
            return p->assets[i].path;
    return NULL;
}

/* Include the engine sources (contains all static SM functions + anim) */
#include "anim.c"
#include "engine.c"

/* physics.c duplicates many static helpers from engine.c — rename to avoid
   redefinition in this single-translation-unit test build. */
#define find_transform_component      phys_find_transform_component
#define find_parent_transform_component phys_find_parent_transform_component
#define find_health_component         phys_find_health_component
#define find_velocity_component       phys_find_velocity_component
#define find_rigid_body_component     phys_find_rigid_body_component
#define find_character_controller_component phys_find_character_controller_component
#define find_box_collider_component   phys_find_box_collider_component
#define find_capsule_collider_component phys_find_capsule_collider_component
#define find_camera_component         phys_find_camera_component
#define find_mesh_component           phys_find_mesh_component
#define resolve_world_position        phys_resolve_world_position
#define resolve_parent_world_offset   phys_resolve_parent_world_offset
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

/* ── Minimal test harness (same as test_dock.c) ───────────────────────── */

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    static void test_##name(void); \
    static void run_##name(void) { \
        tests_run++; \
        printf("  %-55s ", #name); \
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

#define ASSERT_FLOAT_EQ(a, b) do { \
    float _a = (a), _b = (b); \
    if ((_a - _b) > 0.001f || (_b - _a) > 0.001f) { \
        printf("FAIL\n    %s:%d: %s == %.4f, expected %.4f\n", \
               __FILE__, __LINE__, #a, _a, _b); \
        tests_failed++; \
        tests_passed--; \
        return; \
    } \
} while(0)

#define ASSERT_FLOAT_NEAR(a, b, eps) do { \
    float _a = (float)(a), _b = (float)(b); \
    if (_a - _b > (eps) || _b - _a > (eps)) { \
        printf("FAIL\n    %s:%d: %s == %.4f, expected %.4f\n", \
               __FILE__, __LINE__, #a, _a, _b); \
        tests_failed++; \
        tests_passed--; \
        return; \
    } \
} while(0)

/* ── Test helpers ─────────────────────────────────────────────────────── */

#define TEST_ARENA_SIZE (8 * 1024 * 1024)

static uint8_t g_arena_buf[TEST_ARENA_SIZE];


/* Build a minimal 2-joint skeleton with 2 trivial clips (idle, walk) */
static int16_t g_parent_indices[2] = {-1, 0};
static Mat4    g_inverse_bind[2];
static Vec3    g_rest_trans[2];
static Quat    g_rest_rot[2];
static Vec3    g_rest_scale[2];

/* Clip 0 (idle): 2 keyframes for joint 0 translation */
static float g_idle_times[] = {0.0f, 1.0f};
static Vec3  g_idle_trans[] = {{0,0,0}, {0,0.1f,0}};

static ChannelHeader g_idle_headers[1] = {{0, 0, 1}}; /* joint 0, translation, linear */
static ChannelTimes  g_idle_ctimes[1];
static ChannelData   g_idle_cdata[1];

/* Clip 1 (walk): 2 keyframes for joint 0 translation */
static float g_walk_times[] = {0.0f, 0.5f};
static Vec3  g_walk_trans[] = {{0,0,0}, {1,0,0}};

static ChannelHeader g_walk_headers[1] = {{0, 0, 1}};
static ChannelTimes  g_walk_ctimes[1];
static ChannelData   g_walk_cdata[1];

/* Clip 2 (attack): 2 keyframes for joint 0 translation, short duration */
static float g_attack_times[] = {0.0f, 0.4f};
static Vec3  g_attack_trans[] = {{0,0,0}, {0,0,1}};

static ChannelHeader g_attack_headers[1] = {{0, 0, 1}};
static ChannelTimes  g_attack_ctimes[1];
static ChannelData   g_attack_cdata[1];

static AnimClip g_clips[3];

static void init_test_clips(void) {
    int i;
    for (i = 0; i < 2; i++) {
        g_inverse_bind[i] = mat4_identity();
        g_rest_trans[i] = VEC3(0, 0, 0);
        g_rest_rot[i] = QUAT(0, 0, 0, 1);
        g_rest_scale[i] = VEC3(1, 1, 1);
    }

    g_idle_ctimes[0].timestamps = g_idle_times;
    g_idle_ctimes[0].keyframe_count = 2;
    g_idle_cdata[0].vec3s = g_idle_trans;

    g_clips[0].headers = g_idle_headers;
    g_clips[0].times = g_idle_ctimes;
    g_clips[0].data = g_idle_cdata;
    g_clips[0].channel_count = 1;
    g_clips[0].duration = 1.0f;

    g_walk_ctimes[0].timestamps = g_walk_times;
    g_walk_ctimes[0].keyframe_count = 2;
    g_walk_cdata[0].vec3s = g_walk_trans;

    g_clips[1].headers = g_walk_headers;
    g_clips[1].times = g_walk_ctimes;
    g_clips[1].data = g_walk_cdata;
    g_clips[1].channel_count = 1;
    g_clips[1].duration = 0.5f;

    g_attack_ctimes[0].timestamps = g_attack_times;
    g_attack_ctimes[0].keyframe_count = 2;
    g_attack_cdata[0].vec3s = g_attack_trans;

    g_clips[2].headers = g_attack_headers;
    g_clips[2].times = g_attack_ctimes;
    g_clips[2].data = g_attack_cdata;
    g_clips[2].channel_count = 1;
    g_clips[2].duration = 0.4f;
}

static scene_model_asset *setup_test_model_asset(game_state *gs) {
    scene_model_asset *asset = &gs->scene_model_assets[0];
    memset(asset, 0, sizeof(*asset));
    strncpy(asset->key, "test_model", sizeof(asset->key) - 1);
    asset->loaded = 1;
    asset->has_skeleton = 1;
    asset->model.skeleton.parent_indices = g_parent_indices;
    asset->model.skeleton.inverse_bind = g_inverse_bind;
    asset->model.skeleton.rest_translations = g_rest_trans;
    asset->model.skeleton.rest_rotations = g_rest_rot;
    asset->model.skeleton.rest_scales = g_rest_scale;
    asset->model.skeleton.joint_count = 2;
    asset->model.clips = g_clips;
    asset->model.clip_count = sizeof(g_clips) / sizeof(g_clips[0]);
    asset->model.mesh.primitive_count = 1;
    gs->scene_model_asset_count = 1;
    return asset;
}

/* Reset the global arena and return a fresh sub-arena pointer */
static arena *reset_arena(void) {
    arena *a = (arena *)g_arena_buf;
    memset(g_arena_buf, 0, sizeof(g_arena_buf));
    a->base = g_arena_buf + sizeof(arena);
    a->capacity = TEST_ARENA_SIZE - sizeof(arena);
    a->used = 0;
    return a;
}

/* Create a game_state with a fresh arena and minimal scene storage.
   game_state is arena-allocated (too large for the stack). */
static game_state *setup_game_state(void) {
    arena *a = reset_arena();
    game_state *gs = (game_state *)arena_alloc(a,
        (uint32_t)sizeof(game_state), 16, "game_state");
    memset(gs, 0, sizeof(*gs));
    gs->gameplay = arena_alloc_subarena(a, 4 * 1024 * 1024, 16, "gameplay");
    gs->root_arena = a;
    gs->scene_primary_skinned_entity = -1;
    gs->scene_camera_entity = -1;
    gs->dt = 1.0f / 60.0f;
    gs->editor_play_mode = 1;

    /* Allocate component arrays */
    gs->scene_entity_capacity = 32;
    gs->scene_entities = (entity *)arena_alloc(gs->gameplay,
        (uint32_t)(32 * sizeof(entity)), 8, "entities");
    gs->mesh_components = (mesh_component *)arena_alloc(gs->gameplay,
        (uint32_t)(32 * sizeof(mesh_component)), 8, "mesh_comps");
    gs->mesh_component_capacity = 32;
    gs->animation_components = (animation_component *)arena_alloc(gs->gameplay,
        (uint32_t)(8 * sizeof(animation_component)), 8, "anim_comps");
    gs->animation_component_capacity = 8;
    gs->velocity_components = (velocity_component *)arena_alloc(gs->gameplay,
        (uint32_t)(8 * sizeof(velocity_component)), 8, "vel_comps");
    gs->velocity_component_capacity = 8;
    gs->animation_transition_entries = (animation_transition_entry *)arena_alloc(gs->gameplay,
        (uint32_t)(8 * sizeof(animation_transition_entry)), 8, "anim_trans");
    gs->animation_transition_capacity = 8;
    gs->transform_components = (transform_component *)arena_alloc(gs->gameplay,
        (uint32_t)(32 * sizeof(transform_component)), 8, "transform_comps");
    gs->transform_component_capacity = 32;
    gs->parent_transform_components = (parent_transform_component *)arena_alloc(gs->gameplay,
        (uint32_t)(32 * sizeof(parent_transform_component)), 8, "ptc_comps");
    gs->parent_transform_component_capacity = 32;
    gs->character_controller_components = (character_controller_component *)arena_alloc(gs->gameplay,
        (uint32_t)(8 * sizeof(character_controller_component)), 8, "cc_comps");
    gs->character_controller_component_capacity = 8;
    gs->rigid_body_components = (rigid_body_component *)arena_alloc(gs->gameplay,
        (uint32_t)(8 * sizeof(rigid_body_component)), 8, "rb_comps");
    gs->rigid_body_component_capacity = 8;
    gs->capsule_collider_components = (capsule_collider_component *)arena_alloc(gs->gameplay,
        (uint32_t)(8 * sizeof(capsule_collider_component)), 8, "cap_comps");
    gs->capsule_collider_component_capacity = 8;
    gs->box_collider_components = (box_collider_component *)arena_alloc(gs->gameplay,
        (uint32_t)(32 * sizeof(box_collider_component)), 8, "box_comps");
    gs->box_collider_component_capacity = 32;
    gs->rotation_components = (rotation_component *)arena_alloc(gs->gameplay,
        (uint32_t)(8 * sizeof(rotation_component)), 8, "rot_comps");
    gs->rotation_component_capacity = 8;
    gs->health_components = (health_component *)arena_alloc(gs->gameplay,
        (uint32_t)(8 * sizeof(health_component)), 8, "health_comps");
    gs->health_component_capacity = 8;
    gs->trigger_components = (trigger_component *)arena_alloc(gs->gameplay,
        (uint32_t)(8 * sizeof(trigger_component)), 8, "trig_comps");
    gs->trigger_component_capacity = 8;

    /* Init entity->component index lookup arrays */
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

    /* Draw list */
    gs->dl.meshes = (mesh_draw_command *)arena_alloc(gs->gameplay,
        (uint32_t)(DRAW_LIST_MAX_MESH_COMMANDS * sizeof(mesh_draw_command)), 8, "dl_meshes");
    gs->dl.mesh_capacity = DRAW_LIST_MAX_MESH_COMMANDS;
    gs->dl.sprites = (draw_command *)arena_alloc(gs->gameplay,
        (uint32_t)(8 * sizeof(draw_command)), 8, "dl_sprites");
    gs->dl.sprite_capacity = 8;
    gs->dl.lines = (debug_line_command *)arena_alloc(gs->gameplay,
        (uint32_t)(8 * sizeof(debug_line_command)), 8, "dl_lines");
    gs->dl.line_capacity = 8;

    return gs;
}

/* Allocate a bare anim_sm from the arena (also too large for stack) */
static anim_sm *setup_bare_sm(void) {
    arena *a = reset_arena();
    anim_sm *sm = (anim_sm *)arena_alloc(a,
        (uint32_t)sizeof(anim_sm), 16, "anim_sm");
    memset(sm, 0, sizeof(*sm));
    return sm;
}

/* Register entity 0 with mesh + animation components */
static void add_animated_entity(game_state *gs, int entity_index, int model_asset,
                                 int clip, float speed) {
    int mi, ai;

    gs->scene_entity_count = entity_index + 1;

    mi = gs->mesh_component_count++;
    gs->mesh_components[mi].entity_index = entity_index;
    gs->mesh_components[mi].visible = 1;
    gs->mesh_components[mi].model_asset_index = model_asset;
    gs->mesh_index[entity_index] = mi;

    ai = gs->animation_component_count++;
    gs->animation_components[ai].entity_index = entity_index;
    gs->animation_components[ai].playing = 1;
    gs->animation_components[ai].active_clip = clip;
    gs->animation_components[ai].anim_time = 0.0f;
    gs->animation_components[ai].speed = speed;
    gs->animation_index[entity_index] = ai;
}

/* ── Tests: Pool allocation ──────────────────────────────────────────── */

TEST(pool_init_succeeds) {
    game_state *gs = setup_game_state();
    anim_sm_init_pool(&gs->anim, gs->gameplay);
    ASSERT_EQ(gs->anim.pool_initialized, 1);
    ASSERT(gs->anim.pool.skin_mats != NULL);
    ASSERT(gs->anim.pool.pose_trans != NULL);
    ASSERT(gs->anim.pool.pose_rot != NULL);
    ASSERT(gs->anim.pool.pose_scale != NULL);
    ASSERT(gs->anim.pool.world_mats != NULL);
}

TEST(pool_init_fails_on_tiny_arena) {
    /* arena header is ~3KB, so buffer must be larger than that but still
       too small for the 2MB skin_mats pool */
    game_state *gs = setup_game_state();
    uint8_t tiny_buf[4096];
    arena *tiny;
    memset(tiny_buf, 0, sizeof(tiny_buf));
    tiny = (arena *)tiny_buf;
    tiny->base = tiny_buf + sizeof(arena);
    tiny->capacity = (uint32_t)(sizeof(tiny_buf) - sizeof(arena));
    anim_sm_init_pool(&gs->anim, tiny);
    ASSERT_EQ(gs->anim.pool_initialized, 0);
    ASSERT(gs->anim.pool.skin_mats == NULL);
}

/* ── Tests: State management ─────────────────────────────────────────── */

TEST(add_state_returns_sequential_indices) {
    anim_sm *sm = setup_bare_sm();
    int s0, s1, s2;
    s0 = anim_sm_add_state(sm, "idle", 0, 1);
    s1 = anim_sm_add_state(sm, "walk", 1, 1);
    s2 = anim_sm_add_state(sm, "run", 2, 1);
    ASSERT_EQ(s0, 0);
    ASSERT_EQ(s1, 1);
    ASSERT_EQ(s2, 2);
    ASSERT_EQ(sm->state_count, 3);
}

TEST(add_state_stores_clip_and_name) {
    anim_sm *sm = setup_bare_sm();
    anim_sm_add_state(sm, "walk", 5, 0);
    ASSERT_EQ(sm->states[0].clip_index, 5);
    ASSERT_EQ(sm->states[0].looping, 0);
    ASSERT(strcmp(sm->states[0].name, "walk") == 0);
}

TEST(add_state_overflow_returns_minus_one) {
    anim_sm *sm = setup_bare_sm();
    int i, result;
    for (i = 0; i < ANIM_SM_MAX_STATES; i++)
        anim_sm_add_state(sm, "s", i, 1);
    result = anim_sm_add_state(sm, "overflow", 99, 1);
    ASSERT_EQ(result, -1);
}

/* ── Tests: Entity registration ──────────────────────────────────────── */

TEST(register_entity_assigns_skin_mats_offset) {
    game_state *gs = setup_game_state();
    anim_sm *sm = &gs->anim;
    anim_sm_init_pool(sm, gs->gameplay);
    anim_sm_add_state(sm, "idle", 0, 1);

    anim_sm_register_entity(sm, 0, 0, 0, 50);
    anim_sm_register_entity(sm, 1, 0, 0, 30);

    ASSERT_EQ(sm->instance_count, 2);
    ASSERT_EQ(sm->instances[0].skin_mats_offset, 0);
    ASSERT_EQ(sm->instances[1].skin_mats_offset, 50);
    ASSERT_EQ(sm->pool.skin_mats_count, 80);
}

TEST(register_entity_inserts_into_state_table) {
    game_state *gs = setup_game_state();
    anim_sm *sm = &gs->anim;
    anim_sm_init_pool(sm, gs->gameplay);
    anim_sm_add_state(sm, "idle", 0, 1);
    anim_sm_add_state(sm, "walk", 1, 1);

    anim_sm_register_entity(sm, 5, 0, 0, 10);
    anim_sm_register_entity(sm, 7, 0, 1, 10);

    ASSERT_EQ(sm->states[0].count, 1);
    ASSERT_EQ(sm->states[0].entity_indices[0], 5);
    ASSERT_EQ(sm->states[1].count, 1);
    ASSERT_EQ(sm->states[1].entity_indices[0], 7);
}

/* ── Tests: State table insert/remove ────────────────────────────────── */

TEST(insert_and_remove_entity_from_state) {
    anim_sm *sm = setup_bare_sm();
    anim_sm_add_state(sm, "idle", 0, 1);

    anim_sm_insert_entity_into_state(sm, 0, 10);
    anim_sm_insert_entity_into_state(sm, 0, 20);
    anim_sm_insert_entity_into_state(sm, 0, 30);
    ASSERT_EQ(sm->states[0].count, 3);

    anim_sm_remove_entity_from_state(sm, 0, 20);
    ASSERT_EQ(sm->states[0].count, 2);
    /* After swap-remove of middle, last moves into gap */
    ASSERT(sm->states[0].entity_indices[0] == 10 ||
           sm->states[0].entity_indices[1] == 10);
    ASSERT(sm->states[0].entity_indices[0] == 30 ||
           sm->states[0].entity_indices[1] == 30);
}

TEST(remove_nonexistent_entity_is_noop) {
    anim_sm *sm = setup_bare_sm();
    anim_sm_add_state(sm, "idle", 0, 1);
    anim_sm_insert_entity_into_state(sm, 0, 10);
    anim_sm_remove_entity_from_state(sm, 0, 99);
    ASSERT_EQ(sm->states[0].count, 1);
}

/* ── Tests: Find instance ────────────────────────────────────────────── */

TEST(find_instance_returns_correct_entity) {
    anim_sm *sm = setup_bare_sm();
    anim_instance *inst;
    sm->instances[0].entity_index = 3;
    sm->instances[1].entity_index = 7;
    sm->instances[2].entity_index = 12;
    sm->instance_count = 3;

    inst = anim_sm_find_instance(sm, 7);
    ASSERT(inst != NULL);
    ASSERT_EQ(inst->entity_index, 7);
}

TEST(find_instance_returns_null_for_missing) {
    anim_sm *sm = setup_bare_sm();
    sm->instances[0].entity_index = 3;
    sm->instance_count = 1;
    ASSERT(anim_sm_find_instance(sm, 99) == NULL);
}

/* ── Tests: Transition rules ─────────────────────────────────────────── */

TEST(add_rule_stores_correctly) {
    anim_sm *sm = setup_bare_sm();
    anim_sm_add_rule(sm, 0, 1, ANIM_COND_VELOCITY_ABOVE, 0.5f, 0.2f);
    ASSERT_EQ(sm->rule_count, 1);
    ASSERT_EQ(sm->rules[0].from_state, 0);
    ASSERT_EQ(sm->rules[0].to_state, 1);
    ASSERT_EQ((int)sm->rules[0].condition, (int)ANIM_COND_VELOCITY_ABOVE);
    ASSERT_FLOAT_NEAR(sm->rules[0].threshold, 0.5f, 0.001f);
    ASSERT_FLOAT_NEAR(sm->rules[0].blend_duration, 0.2f, 0.001f);
}

/* ── Tests: Phase 1 — advance times ──────────────────────────────────── */

TEST(phase1_advances_anim_times) {
    game_state *gs = setup_game_state();
    anim_sm *sm;
    init_test_clips();
    setup_test_model_asset(gs);
    sm = &gs->anim;
    anim_sm_init_pool(sm, gs->gameplay);
    anim_sm_add_state(sm, "idle", 0, 1);
    anim_sm_register_entity(sm, 0, 0, 0, 2);

    ASSERT_FLOAT_NEAR(sm->states[0].anim_times[0], 0.0f, 0.001f);

    /* Run one update at dt=1/60 */
    gs->dt = 1.0f / 60.0f;
    anim_sm_update(sm, gs, gs->dt);

    /* Time should have advanced by dt * speed (speed defaults to 1.0) */
    ASSERT(sm->states[0].anim_times[0] > 0.0f);
    ASSERT_FLOAT_NEAR(sm->states[0].anim_times[0], 1.0f / 60.0f, 0.001f);
}

/* ── Tests: Phase 2 — condition evaluation triggers blend ────────────── */

TEST(velocity_above_triggers_transition_to_blend) {
    game_state *gs = setup_game_state();
    anim_sm *sm;
    int vi;
    init_test_clips();
    setup_test_model_asset(gs);
    sm = &gs->anim;
    anim_sm_init_pool(sm, gs->gameplay);
    anim_sm_add_state(sm, "idle", 0, 1);
    anim_sm_add_state(sm, "walk", 1, 1);
    anim_sm_add_rule(sm, 0, 1, ANIM_COND_VELOCITY_ABOVE, 0.1f, 0.2f);

    add_animated_entity(gs, 0, 0, 0, 1.0f);
    anim_sm_register_entity(sm, 0, 0, 0, 2);

    /* Give entity velocity above threshold */
    vi = gs->velocity_component_count++;
    gs->velocity_components[vi].entity_index = 0;
    gs->velocity_components[vi].velocity = VEC3(1.0f, 0.0f, 0.0f);
    gs->velocity_index[0] = vi;

    ASSERT_EQ(sm->states[0].count, 1);
    ASSERT_EQ(sm->blend_count, 0);

    anim_sm_update(sm, gs, 1.0f / 60.0f);

    /* Entity should have moved from idle state to blend table */
    ASSERT_EQ(sm->states[0].count, 0);
    ASSERT_EQ(sm->blend_count, 1);
    ASSERT_EQ(sm->blends[0].entity_index, 0);
    ASSERT_EQ(sm->blends[0].from_clip, 0);
    ASSERT_EQ(sm->blends[0].to_clip, 1);
    ASSERT_EQ(sm->blends[0].to_state, 1);
}

TEST(velocity_below_threshold_no_transition) {
    game_state *gs = setup_game_state();
    anim_sm *sm;
    int vi;
    init_test_clips();
    setup_test_model_asset(gs);
    sm = &gs->anim;
    anim_sm_init_pool(sm, gs->gameplay);
    anim_sm_add_state(sm, "idle", 0, 1);
    anim_sm_add_state(sm, "walk", 1, 1);
    anim_sm_add_rule(sm, 0, 1, ANIM_COND_VELOCITY_ABOVE, 0.5f, 0.2f);

    add_animated_entity(gs, 0, 0, 0, 1.0f);
    anim_sm_register_entity(sm, 0, 0, 0, 2);

    /* Velocity below threshold */
    vi = gs->velocity_component_count++;
    gs->velocity_components[vi].entity_index = 0;
    gs->velocity_components[vi].velocity = VEC3(0.1f, 0.0f, 0.0f);
    gs->velocity_index[0] = vi;

    anim_sm_update(sm, gs, 1.0f / 60.0f);

    ASSERT_EQ(sm->states[0].count, 1);
    ASSERT_EQ(sm->blend_count, 0);
}

/* ── Tests: Phase 3 — blend completion ───────────────────────────────── */

TEST(blend_completes_and_entity_moves_to_dest_state) {
    game_state *gs = setup_game_state();
    anim_sm *sm;
    int vi;
    float total_time;
    int frame;
    init_test_clips();
    setup_test_model_asset(gs);
    sm = &gs->anim;
    anim_sm_init_pool(sm, gs->gameplay);
    anim_sm_add_state(sm, "idle", 0, 1);
    anim_sm_add_state(sm, "walk", 1, 1);
    anim_sm_add_rule(sm, 0, 1, ANIM_COND_VELOCITY_ABOVE, 0.1f, 0.18f);

    add_animated_entity(gs, 0, 0, 0, 1.0f);
    anim_sm_register_entity(sm, 0, 0, 0, 2);

    vi = gs->velocity_component_count++;
    gs->velocity_components[vi].entity_index = 0;
    gs->velocity_components[vi].velocity = VEC3(1.0f, 0.0f, 0.0f);
    gs->velocity_index[0] = vi;

    /* First frame: triggers blend */
    anim_sm_update(sm, gs, 1.0f / 60.0f);
    ASSERT_EQ(sm->blend_count, 1);

    /* Run enough frames to complete the 0.18s blend */
    total_time = 1.0f / 60.0f;
    for (frame = 0; frame < 15 && sm->blend_count > 0; frame++) {
        anim_sm_update(sm, gs, 1.0f / 60.0f);
        total_time += 1.0f / 60.0f;
    }

    /* Blend should be complete, entity in walk state */
    ASSERT_EQ(sm->blend_count, 0);
    ASSERT_EQ(sm->states[1].count, 1);
    ASSERT_EQ(sm->states[1].entity_indices[0], 0);
    ASSERT_EQ(sm->states[0].count, 0);
}

/* ── Tests: Phase 4 — skinning output ────────────────────────────────── */

TEST(phase4_writes_skin_mats_for_state_entities) {
    game_state *gs = setup_game_state();
    anim_sm *sm;
    Mat4 *dst;
    int all_zero, k;
    init_test_clips();
    setup_test_model_asset(gs);
    sm = &gs->anim;
    anim_sm_init_pool(sm, gs->gameplay);
    anim_sm_add_state(sm, "idle", 0, 1);

    add_animated_entity(gs, 0, 0, 0, 1.0f);
    anim_sm_register_entity(sm, 0, 0, 0, 2);

    /* Clear skin_mats to detect writes */
    dst = sm->pool.skin_mats + sm->instances[0].skin_mats_offset;
    memset(dst, 0, 2 * sizeof(Mat4));

    anim_sm_update(sm, gs, 1.0f / 60.0f);

    /* Verify skin_mats were written (not all zero) */
    all_zero = 1;
    for (k = 0; k < 32; k++) {
        if (dst[0].m[k % 16] != 0.0f) { all_zero = 0; break; }
    }
    ASSERT(!all_zero);
}

/* ── Tests: Phase 5 — blend skinning ─────────────────────────────────── */

TEST(phase5_writes_blended_skin_mats) {
    game_state *gs = setup_game_state();
    anim_sm *sm;
    Mat4 *dst;
    int vi, all_zero, k;
    init_test_clips();
    setup_test_model_asset(gs);
    sm = &gs->anim;
    anim_sm_init_pool(sm, gs->gameplay);
    anim_sm_add_state(sm, "idle", 0, 1);
    anim_sm_add_state(sm, "walk", 1, 1);
    anim_sm_add_rule(sm, 0, 1, ANIM_COND_VELOCITY_ABOVE, 0.1f, 0.5f);

    add_animated_entity(gs, 0, 0, 0, 1.0f);
    anim_sm_register_entity(sm, 0, 0, 0, 2);

    vi = gs->velocity_component_count++;
    gs->velocity_components[vi].entity_index = 0;
    gs->velocity_components[vi].velocity = VEC3(1.0f, 0.0f, 0.0f);
    gs->velocity_index[0] = vi;

    /* First frame triggers blend */
    anim_sm_update(sm, gs, 1.0f / 60.0f);
    ASSERT_EQ(sm->blend_count, 1);

    /* Clear and run one more frame while blending */
    dst = sm->pool.skin_mats + sm->instances[0].skin_mats_offset;
    memset(dst, 0, 2 * sizeof(Mat4));

    anim_sm_update(sm, gs, 1.0f / 60.0f);

    /* Skin mats should have been written by blend phase */
    all_zero = 1;
    for (k = 0; k < 16; k++) {
        if (dst[0].m[k] != 0.0f) { all_zero = 0; break; }
    }
    ASSERT(!all_zero);
}

/* ── Tests: Multi-entity independence ────────────────────────────────── */

TEST(multiple_entities_have_independent_poses) {
    game_state *gs = setup_game_state();
    anim_sm *sm;
    Mat4 *dst0, *dst1;
    init_test_clips();
    setup_test_model_asset(gs);
    sm = &gs->anim;
    anim_sm_init_pool(sm, gs->gameplay);
    anim_sm_add_state(sm, "idle", 0, 1);
    anim_sm_add_state(sm, "walk", 1, 1);

    add_animated_entity(gs, 0, 0, 0, 1.0f);
    add_animated_entity(gs, 1, 0, 1, 1.0f);
    anim_sm_register_entity(sm, 0, 0, 0, 2); /* entity 0 in idle */
    anim_sm_register_entity(sm, 1, 0, 1, 2); /* entity 1 in walk */

    /* Advance a few frames so clips diverge */
    anim_sm_update(sm, gs, 0.1f);
    anim_sm_update(sm, gs, 0.1f);
    anim_sm_update(sm, gs, 0.1f);

    dst0 = sm->pool.skin_mats + sm->instances[0].skin_mats_offset;
    dst1 = sm->pool.skin_mats + sm->instances[1].skin_mats_offset;

    /* They play different clips so joint 0 translation should differ */
    ASSERT(memcmp(dst0, dst1, sizeof(Mat4)) != 0);
}

/* ── Tests: Draw commands get bone_buffer_offset ─────────────────────── */

TEST(build_mesh_draw_commands_sets_bone_offset) {
    game_state *gs = setup_game_state();
    anim_sm *sm;
    init_test_clips();
    setup_test_model_asset(gs);
    sm = &gs->anim;
    anim_sm_init_pool(sm, gs->gameplay);
    anim_sm_add_state(sm, "idle", 0, 1);

    add_animated_entity(gs, 0, 0, 0, 1.0f);
    add_animated_entity(gs, 1, 0, 0, 1.0f);
    anim_sm_register_entity(sm, 0, 0, 0, 2);
    anim_sm_register_entity(sm, 1, 0, 0, 2);

    build_mesh_draw_commands(gs);

    ASSERT_EQ(gs->dl.mesh_count, 2);
    ASSERT_EQ(gs->dl.meshes[0].use_skinned_bones, 1);
    ASSERT_EQ(gs->dl.meshes[0].bone_buffer_offset, 0);
    ASSERT_EQ(gs->dl.meshes[1].use_skinned_bones, 1);
    ASSERT_EQ(gs->dl.meshes[1].bone_buffer_offset, 2);
}

/* ── Tests: Bidirectional transitions (idle <-> walk) ────────────────── */

TEST(bidirectional_transition_idle_to_walk_and_back) {
    game_state *gs = setup_game_state();
    anim_sm *sm;
    int vi, frame;
    init_test_clips();
    setup_test_model_asset(gs);
    sm = &gs->anim;
    anim_sm_init_pool(sm, gs->gameplay);
    anim_sm_add_state(sm, "idle", 0, 1);
    anim_sm_add_state(sm, "walk", 1, 1);
    anim_sm_add_rule(sm, 0, 1, ANIM_COND_VELOCITY_ABOVE, 0.1f, 0.1f);
    anim_sm_add_rule(sm, 1, 0, ANIM_COND_VELOCITY_BELOW, 0.1f, 0.1f);

    add_animated_entity(gs, 0, 0, 0, 1.0f);
    anim_sm_register_entity(sm, 0, 0, 0, 2);

    /* Give velocity to trigger idle -> walk */
    vi = gs->velocity_component_count++;
    gs->velocity_components[vi].entity_index = 0;
    gs->velocity_components[vi].velocity = VEC3(1.0f, 0.0f, 0.0f);
    gs->velocity_index[0] = vi;

    /* Run until blend completes */
    for (frame = 0; frame < 20; frame++)
        anim_sm_update(sm, gs, 1.0f / 60.0f);

    ASSERT_EQ(sm->states[1].count, 1); /* in walk */
    ASSERT_EQ(sm->states[0].count, 0);

    /* Stop velocity to trigger walk -> idle */
    gs->velocity_components[vi].velocity = VEC3(0.0f, 0.0f, 0.0f);

    for (frame = 0; frame < 20; frame++)
        anim_sm_update(sm, gs, 1.0f / 60.0f);

    ASSERT_EQ(sm->states[0].count, 1); /* back in idle */
    ASSERT_EQ(sm->states[1].count, 0);
}

/* ── Tests: Frame pipeline (catches ordering bugs) ───────────────────── */

TEST(velocity_on_same_entity_triggers_transition) {
    /* CC writes velocity on the same entity that has the animation.
       SM reads it directly without needing parent chain walk. */
    game_state *gs = setup_game_state();
    anim_sm *sm;
    int vi;
    init_test_clips();
    setup_test_model_asset(gs);
    sm = &gs->anim;
    anim_sm_init_pool(sm, gs->gameplay);
    anim_sm_add_state(sm, "idle", 0, 1);
    anim_sm_add_state(sm, "run", 1, 1);
    anim_sm_add_rule(sm, 0, 1, ANIM_COND_VELOCITY_ABOVE, 0.1f, 0.2f);

    add_animated_entity(gs, 0, 0, 0, 1.0f);
    anim_sm_register_entity(sm, 0, 0, 0, 2);

    /* Velocity on the same entity */
    vi = gs->velocity_component_count++;
    gs->velocity_components[vi].entity_index = 0;
    gs->velocity_components[vi].velocity = VEC3(5.0f, 0.0f, 0.0f);
    gs->velocity_index[0] = vi;

    anim_sm_update(sm, gs, 1.0f / 60.0f);

    ASSERT_EQ(sm->states[0].count, 0);
    ASSERT_EQ(sm->blend_count, 1);
}

TEST(zero_velocity_stays_idle) {
    /* velocity = 0 — entity stays in idle */
    game_state *gs = setup_game_state();
    anim_sm *sm;
    int vi;
    init_test_clips();
    setup_test_model_asset(gs);
    sm = &gs->anim;
    anim_sm_init_pool(sm, gs->gameplay);
    anim_sm_add_state(sm, "idle", 0, 1);
    anim_sm_add_state(sm, "run", 1, 1);
    anim_sm_add_rule(sm, 0, 1, ANIM_COND_VELOCITY_ABOVE, 0.1f, 0.2f);

    add_animated_entity(gs, 0, 0, 0, 1.0f);
    anim_sm_register_entity(sm, 0, 0, 0, 2);

    vi = gs->velocity_component_count++;
    gs->velocity_components[vi].entity_index = 0;
    gs->velocity_components[vi].velocity = VEC3(0.0f, 0.0f, 0.0f);
    gs->velocity_index[0] = vi;

    anim_sm_update(sm, gs, 1.0f / 60.0f);

    ASSERT_EQ(sm->states[0].count, 1);
    ASSERT_EQ(sm->blend_count, 0);
}

/* ── Scene setup matching project.toml ────────────────────────────────
   Entity 0  = "player":     transform, rotation, velocity, rigid_body,
                              CC(5.0, 8.5), health, capsule_collider
   Entity 1  = "camera":     transform, parent_transform(parent=0)
   Entity 2  = "floor_root": transform(0,-1,0)
   Entity 3-18 = floor tiles: transform, parent_transform(parent=2),
                               mesh(floor model), box_collider
   Entity 19 = "player_mesh": transform, parent_transform(parent=0),
                               mesh(knight), animation
   ──────────────────────────────────────────────────────────────────── */

static void setup_project_scene(game_state *gs) {
    int i, ti, pti, mi, bi, vi, cci, rbi, ri, hi, ci;
    /* Floor tile positions (4x4 grid) */
    static const float floor_pos[16][3] = {
        {-6,0,-6},{-2,0,-6},{ 2,0,-6},{ 6,0,-6},
        {-6,0,-2},{-2,0,-2},{ 2,0,-2},{ 6,0,-2},
        {-6,0, 2},{-2,0, 2},{ 2,0, 2},{ 6,0, 2},
        {-6,0, 6},{-2,0, 6},{ 2,0, 6},{ 6,0, 6}
    };

    gs->scene_entity_count = 20;

    /* Entity 0: player — transform, rotation, velocity, rigid_body, CC,
       health, capsule_collider */
    ti = gs->transform_component_count++;
    gs->transform_components[ti].entity_index = 0;
    gs->transform_components[ti].position = VEC3(1.57f, -0.10f, -2.58f);
    gs->transform_index[0] = ti;

    ri = gs->rotation_component_count++;
    gs->rotation_components[ri].entity_index = 0;
    gs->rotation_components[ri].rotation_y_deg = -180.0f;
    gs->rotation_index[0] = ri;

    vi = gs->velocity_component_count++;
    gs->velocity_components[vi].entity_index = 0;
    gs->velocity_components[vi].velocity = VEC3(0, 0, 0);
    gs->velocity_index[0] = vi;

    rbi = gs->rigid_body_component_count++;
    gs->rigid_body_components[rbi].entity_index = 0;
    gs->rigid_body_components[rbi].use_gravity = 1;
    gs->rigid_body_index[0] = rbi;

    cci = gs->character_controller_component_count++;
    gs->character_controller_components[cci].entity_index = 0;
    gs->character_controller_components[cci].move_speed = 5.0f;
    gs->character_controller_components[cci].jump_speed = 8.5f;
    gs->character_controller_index[0] = cci;

    hi = gs->health_component_count++;
    gs->health_components[hi].entity_index = 0;
    gs->health_components[hi].health = 100.0f;
    gs->health_components[hi].max_health = 100.0f;
    gs->health_index[0] = hi;

    ci = gs->capsule_collider_component_count++;
    gs->capsule_collider_components[ci].entity_index = 0;
    gs->capsule_collider_components[ci].radius = 0.38f;
    gs->capsule_collider_components[ci].half_height = 0.46f;
    gs->capsule_collider_index[0] = ci;

    /* Entity 1: camera — transform, parent_transform(parent=0) */
    ti = gs->transform_component_count++;
    gs->transform_components[ti].entity_index = 1;
    gs->transform_components[ti].position = VEC3(0, 8, 10);
    gs->transform_index[1] = ti;

    pti = gs->parent_transform_component_count++;
    gs->parent_transform_components[pti].entity_index = 1;
    gs->parent_transform_components[pti].parent_entity_index = 0;
    gs->parent_transform_index[1] = pti;

    /* Entity 2: floor_root — transform(0,-1,0) */
    ti = gs->transform_component_count++;
    gs->transform_components[ti].entity_index = 2;
    gs->transform_components[ti].position = VEC3(0, -1, 0);
    gs->transform_index[2] = ti;

    /* Entities 3-18: floor tiles — transform, parent_transform(parent=2),
       mesh, box_collider */
    for (i = 0; i < 16; i++) {
        int eidx = 3 + i;

        ti = gs->transform_component_count++;
        gs->transform_components[ti].entity_index = eidx;
        gs->transform_components[ti].position = VEC3(
            floor_pos[i][0], floor_pos[i][1], floor_pos[i][2]);
        gs->transform_index[eidx] = ti;

        pti = gs->parent_transform_component_count++;
        gs->parent_transform_components[pti].entity_index = eidx;
        gs->parent_transform_components[pti].parent_entity_index = 2;
        gs->parent_transform_index[eidx] = pti;

        mi = gs->mesh_component_count++;
        gs->mesh_components[mi].entity_index = eidx;
        gs->mesh_components[mi].visible = 1;
        gs->mesh_components[mi].model_asset_index = -1; /* no model in test */
        gs->mesh_index[eidx] = mi;

        bi = gs->box_collider_component_count++;
        gs->box_collider_components[bi].entity_index = eidx;
        gs->box_collider_components[bi].rect = (rect){0, 0, 4.0f, 4.0f};
        gs->box_collider_components[bi].half_height = 0.2f;
        gs->box_collider_index[eidx] = bi;
    }

    /* Entity 19: player_mesh — transform, parent_transform(parent=0),
       mesh(knight), animation */
    add_animated_entity(gs, 19, 0, 0, 1.0f);
    pti = gs->parent_transform_component_count++;
    gs->parent_transform_components[pti].entity_index = 19;
    gs->parent_transform_components[pti].parent_entity_index = 0;
    gs->parent_transform_index[19] = pti;

    ti = gs->transform_component_count++;
    gs->transform_components[ti].entity_index = 19;
    gs->transform_components[ti].position = VEC3(0, -0.86f, 0);
    gs->transform_index[19] = ti;

    /* Entities 20-23: south wall segments at z=-8, parented to floor_root.
       4 units wide (X), 0.5 thick (Z), 4 tall (Y). */
    {
        static const float wall_x[4] = { -6.0f, -2.0f, 2.0f, 6.0f };
        int w;
        for (w = 0; w < 4; w++) {
            int eidx = 20 + w;
            ti = gs->transform_component_count++;
            gs->transform_components[ti].entity_index = eidx;
            gs->transform_components[ti].position = VEC3(wall_x[w], 0.0f, -8.0f);
            gs->transform_index[eidx] = ti;

            pti = gs->parent_transform_component_count++;
            gs->parent_transform_components[pti].entity_index = eidx;
            gs->parent_transform_components[pti].parent_entity_index = 2;
            gs->parent_transform_index[eidx] = pti;

            bi = gs->box_collider_component_count++;
            gs->box_collider_components[bi].entity_index = eidx;
            gs->box_collider_components[bi].rect = (rect){0, 0, 4.0f, 0.5f};
            gs->box_collider_components[bi].half_height = 2.0f;
            gs->box_collider_index[eidx] = bi;
        }
        gs->scene_entity_count = 24;
    }
}

/* ── Tests: Full project scene (matches project.toml) ────────────────── */

TEST(parent_child_velocity_triggers_transition) {
    game_state *gs = setup_game_state();
    anim_sm *sm;
    init_test_clips();
    setup_test_model_asset(gs);
    setup_project_scene(gs);
    sm = &gs->anim;
    anim_sm_init_pool(sm, gs->gameplay);
    anim_sm_add_state(sm, "idle", 0, 1);
    anim_sm_add_state(sm, "run", 1, 1);
    anim_sm_add_rule(sm, 0, 1, ANIM_COND_VELOCITY_ABOVE, 0.1f, 0.2f);
    anim_sm_register_entity(sm, 19, 0, 0, 2);

    /* CC writes velocity along forward (z-axis) on entity 0 */
    gs->velocity_components[0].velocity = VEC3(0.0f, 0.0f, 5.0f);

    anim_sm_update(sm, gs, 1.0f / 60.0f);

    /* SM walks 19→0, reads horizontal speed=5.0 > 0.1 threshold → transition */
    ASSERT_EQ(sm->states[0].count, 0);
    ASSERT_EQ(sm->blend_count, 1);
    ASSERT_EQ(sm->blends[0].entity_index, 19);
}

TEST(parent_child_no_velocity_stays_idle) {
    game_state *gs = setup_game_state();
    anim_sm *sm;
    init_test_clips();
    setup_test_model_asset(gs);
    setup_project_scene(gs);
    sm = &gs->anim;
    anim_sm_init_pool(sm, gs->gameplay);
    anim_sm_add_state(sm, "idle", 0, 1);
    anim_sm_add_state(sm, "run", 1, 1);
    anim_sm_add_rule(sm, 0, 1, ANIM_COND_VELOCITY_ABOVE, 0.1f, 0.2f);
    anim_sm_register_entity(sm, 19, 0, 0, 2);

    /* velocity = 0 (not moving) */
    anim_sm_update(sm, gs, 1.0f / 60.0f);

    ASSERT_EQ(sm->states[0].count, 1);
    ASSERT_EQ(sm->blend_count, 0);
}

TEST(full_scene_bidirectional_transition) {
    game_state *gs = setup_game_state();
    anim_sm *sm;
    int frame;
    init_test_clips();
    setup_test_model_asset(gs);
    setup_project_scene(gs);
    sm = &gs->anim;
    anim_sm_init_pool(sm, gs->gameplay);
    anim_sm_add_state(sm, "idle", 0, 1);
    anim_sm_add_state(sm, "run", 1, 1);
    anim_sm_add_rule(sm, 0, 1, ANIM_COND_VELOCITY_ABOVE, 0.1f, 0.1f);
    anim_sm_add_rule(sm, 1, 0, ANIM_COND_VELOCITY_BELOW, 0.1f, 0.1f);
    anim_sm_register_entity(sm, 19, 0, 0, 2);

    /* Start moving — idle → run */
    gs->velocity_components[0].velocity = VEC3(0.0f, 0.0f, 5.0f);
    for (frame = 0; frame < 20; frame++)
        anim_sm_update(sm, gs, 1.0f / 60.0f);
    ASSERT_EQ(sm->states[1].count, 1); /* in run */
    ASSERT_EQ(sm->states[0].count, 0);

    /* Stop moving — run → idle */
    gs->velocity_components[0].velocity = VEC3(0.0f, 0.0f, 0.0f);
    for (frame = 0; frame < 20; frame++)
        anim_sm_update(sm, gs, 1.0f / 60.0f);
    ASSERT_EQ(sm->states[0].count, 1); /* back in idle */
    ASSERT_EQ(sm->states[1].count, 0);
}

/* ── Tests: Wall collision + sliding ─────────────────────────────────── */

TEST(wall_blocks_movement_into_wall) {
    game_state *gs = setup_game_state();
    float start_z;
    setup_project_scene(gs);
    /* Place player close to south wall (wall world z = -1 + -8 = -9,
       wall near edge = -9 + 0.25 = -8.75).  Capsule radius=0.38.
       At z=-7.3, capsule edge = -7.68, gap to wall = 0.07.
       One frame at v=-5: move = -0.083, edge = -7.76 → overlaps wall. */
    gs->transform_components[0].position = VEC3(0.0f, -0.10f, -7.3f);
    gs->dt = 1.0f / 60.0f;
    /* Push toward wall (negative Z) */
    gs->velocity_components[0].velocity = VEC3(0.0f, 0.0f, -5.0f);
    start_z = gs->transform_components[0].position.z;
    apply_movement(gs);
    /* Z should NOT have moved (blocked by wall) */
    ASSERT_FLOAT_EQ(gs->transform_components[0].position.z, start_z);
}

TEST(wall_allows_sliding_along_wall) {
    game_state *gs = setup_game_state();
    float start_x, start_z;
    setup_project_scene(gs);
    /* Same position near south wall */
    gs->transform_components[0].position = VEC3(0.0f, -0.10f, -7.3f);
    gs->dt = 1.0f / 60.0f;
    /* Push diagonally into wall: X slides, Z blocked */
    gs->velocity_components[0].velocity = VEC3(5.0f, 0.0f, -5.0f);
    start_x = gs->transform_components[0].position.x;
    start_z = gs->transform_components[0].position.z;
    apply_movement(gs);
    /* X should slide (not blocked), Z should be blocked */
    ASSERT(gs->transform_components[0].position.x > start_x);
    ASSERT_FLOAT_EQ(gs->transform_components[0].position.z, start_z);
}

TEST(no_wall_allows_free_movement) {
    game_state *gs = setup_game_state();
    float start_x, start_z;
    setup_project_scene(gs);
    /* Player in center, far from walls */
    gs->transform_components[0].position = VEC3(0.0f, -0.10f, 0.0f);
    gs->dt = 1.0f / 60.0f;
    gs->velocity_components[0].velocity = VEC3(3.0f, 0.0f, 3.0f);
    start_x = gs->transform_components[0].position.x;
    start_z = gs->transform_components[0].position.z;
    apply_movement(gs);
    /* Both axes should move freely */
    ASSERT(gs->transform_components[0].position.x > start_x);
    ASSERT(gs->transform_components[0].position.z > start_z);
}

/* ── Tests: Edge cases ───────────────────────────────────────────────── */

TEST(update_with_zero_instances_is_noop) {
    game_state *gs = setup_game_state();
    gs->anim.pool_initialized = 0;
    gs->anim.instance_count = 0;
    /* Should not crash */
    anim_sm_update(&gs->anim, gs, 1.0f / 60.0f);
    ASSERT_EQ(gs->anim.blend_count, 0);
}

TEST(anim_time_wraps_for_looping_clips) {
    game_state *gs = setup_game_state();
    anim_sm *sm;
    init_test_clips();
    setup_test_model_asset(gs);
    sm = &gs->anim;
    anim_sm_init_pool(sm, gs->gameplay);
    anim_sm_add_state(sm, "idle", 0, 1); /* clip 0 duration = 1.0s */

    add_animated_entity(gs, 0, 0, 0, 1.0f);
    anim_sm_register_entity(sm, 0, 0, 0, 2);

    /* Advance past clip duration */
    anim_sm_update(sm, gs, 1.5f);

    /* Time should have wrapped */
    ASSERT(sm->states[0].anim_times[0] >= 0.0f);
    ASSERT(sm->states[0].anim_times[0] < 1.0f);
}

/* ── Trigger system tests ─────────────────────────────────────────────── */

TEST(key_pickup_opens_door) {
    game_state *gs = setup_game_state();
    int ti, mi, tgi, bci;

    /* 3 entities: player (0), key (1), door (2) */
    gs->scene_entity_count = 3;

    /* Entity 0: player — transform, velocity, CC, capsule collider */
    ti = gs->transform_component_count++;
    gs->transform_components[ti].entity_index = 0;
    gs->transform_components[ti].position = VEC3(0.0f, 0.0f, -4.0f);
    gs->transform_index[0] = ti;
    gs->velocity_component_count++;
    gs->velocity_components[0].entity_index = 0;
    gs->velocity_components[0].velocity = VEC3(0, 0, 0);
    gs->velocity_index[0] = 0;
    gs->character_controller_component_count++;
    gs->character_controller_components[0].entity_index = 0;
    gs->character_controller_components[0].move_speed = 5.0f;
    gs->character_controller_components[0].jump_speed = 8.5f;
    gs->character_controller_index[0] = 0;
    gs->capsule_collider_component_count++;
    gs->capsule_collider_components[0].entity_index = 0;
    gs->capsule_collider_components[0].radius = 0.38f;
    gs->capsule_collider_components[0].half_height = 0.46f;
    gs->capsule_collider_index[0] = 0;

    /* Entity 1: key — transform, mesh (visible), PICKUP trigger targeting entity 2 */
    ti = gs->transform_component_count++;
    gs->transform_components[ti].entity_index = 1;
    gs->transform_components[ti].position = VEC3(2.0f, 0.0f, -4.0f);
    gs->transform_index[1] = ti;
    mi = gs->mesh_component_count++;
    gs->mesh_components[mi].entity_index = 1;
    gs->mesh_components[mi].visible = 1;
    gs->mesh_components[mi].model_asset_index = 0;
    gs->mesh_index[1] = mi;
    tgi = gs->trigger_component_count++;
    gs->trigger_components[tgi].entity_index = 1;
    gs->trigger_components[tgi].type = TRIGGER_PICKUP;
    gs->trigger_components[tgi].target_entity = 2;
    gs->trigger_components[tgi].radius = 1.0f;
    gs->trigger_components[tgi].activated = 0;
    gs->trigger_index[1] = tgi;

    /* Entity 2: door — transform, mesh (visible), box collider, DOOR trigger */
    ti = gs->transform_component_count++;
    gs->transform_components[ti].entity_index = 2;
    gs->transform_components[ti].position = VEC3(-2.0f, 0.0f, -8.0f);
    gs->transform_index[2] = ti;
    mi = gs->mesh_component_count++;
    gs->mesh_components[mi].entity_index = 2;
    gs->mesh_components[mi].visible = 1;
    gs->mesh_components[mi].model_asset_index = 0;
    gs->mesh_index[2] = mi;
    bci = gs->box_collider_component_count++;
    gs->box_collider_components[bci].entity_index = 2;
    gs->box_collider_components[bci].rect.w = 4.0f;
    gs->box_collider_components[bci].rect.h = 0.5f;
    gs->box_collider_components[bci].half_height = 2.0f;
    gs->box_collider_index[2] = bci;
    tgi = gs->trigger_component_count++;
    gs->trigger_components[tgi].entity_index = 2;
    gs->trigger_components[tgi].type = TRIGGER_DOOR;
    gs->trigger_components[tgi].target_entity = 0;
    gs->trigger_components[tgi].radius = 0.0f;
    gs->trigger_components[tgi].activated = 0;
    gs->trigger_index[2] = tgi;

    /* Player at (0,0,-4), key at (2,0,-4) — distance = 2.0, radius = 1.0 → not triggered */
    update_triggers(gs);
    ASSERT_EQ(gs->trigger_component_count, 2);
    ASSERT_EQ(gs->mesh_components[0].visible, 1); /* key mesh still visible */
    ASSERT_EQ(gs->mesh_components[1].visible, 1); /* door mesh still visible */
    ASSERT(gs->box_collider_components[0].rect.w > 0.0f); /* door collider active */

    /* Move player to key position */
    gs->transform_components[0].position = VEC3(2.0f, 0.0f, -4.0f);
    update_triggers(gs);

    /* Both triggers removed from array by swap-and-pop */
    ASSERT_EQ(gs->trigger_component_count, 0);
    ASSERT_EQ(gs->trigger_index[1], -1); /* key entity no longer indexed */
    ASSERT_EQ(gs->trigger_index[2], -1); /* door entity no longer indexed */

    /* Key mesh hidden */
    ASSERT_EQ(gs->mesh_components[0].visible, 0);

    /* Door mesh hidden, door collider disabled */
    ASSERT_EQ(gs->mesh_components[1].visible, 0);
    ASSERT_FLOAT_EQ(gs->box_collider_components[0].rect.w, 0.0f);
    ASSERT_FLOAT_EQ(gs->box_collider_components[0].rect.h, 0.0f);
}

/* ── Tests: build_scene_from_project ──────────────────────────────────── */

TEST(build_scene_from_project_creates_runtime_components) {
    game_state *gs = setup_game_state();

    /* 5 entities */
    gs->project.scene_entity_count = 5;
    gs->project_loaded = 1;

    /* transforms: entity 0 at (1,2,3), entity 2 at (4,5,6) */
    gs->project.transforms[0] = (project_transform){0, {1.0f, 2.0f, 3.0f}};
    gs->project.transforms[1] = (project_transform){2, {4.0f, 5.0f, 6.0f}};
    gs->project.transform_count = 2;

    /* rotation: entity 1 at y=1.57 */
    gs->project.rotations[0] = (project_rotation){1, 1.57f};
    gs->project.rotation_count = 1;

    /* scale: entity 0 at (2,2,2) */
    gs->project.scales[0] = (project_scale){0, {2.0f, 2.0f, 2.0f}};
    gs->project.scale_count = 1;

    /* parent_transform: entity 1 parented to entity 0 */
    gs->project.parent_transforms[0] = (project_parent_transform){1, 0};
    gs->project.parent_transform_count = 1;

    /* parent_rotation: entity 1 */
    gs->project.parent_rotations[0] = (project_parent_rotation){1};
    gs->project.parent_rotation_count = 1;

    /* velocity: entity 0 at (1.5, 0) */
    gs->project.velocities[0] = (project_velocity){0, {1.5f, 0.0f}};
    gs->project.velocity_count = 1;

    /* rigid_body: entity 0, use_gravity=1 */
    gs->project.rigid_bodies[0] = (project_rigid_body){0, 1};
    gs->project.rigid_body_count = 1;

    /* character_controller: entity 0, move_speed=5, jump_speed=8 */
    gs->project.character_controllers[0] = (project_character_controller){0, 5.0f, 8.0f};
    gs->project.character_controller_count = 1;

    /* health: entity 0, current=80, max=100 */
    gs->project.healths[0] = (project_health){0, 80.0f, 100.0f};
    gs->project.health_count = 1;

    /* box_collider: entity 2, half_extents=(0.5, 1.0, 0.5) */
    gs->project.box_colliders[0] = (project_box_collider){2, {0.5f, 1.0f, 0.5f}};
    gs->project.box_collider_count = 1;

    /* capsule_collider: entity 0, radius=0.3, half_height=0.9 */
    gs->project.capsule_colliders[0] = (project_capsule_collider){0, 0.3f, 0.9f};
    gs->project.capsule_collider_count = 1;

    /* camera: entity 3, fov=60, near=0.1, far=100 */
    {
        project_cam cam = {0};
        cam.entity = 3;
        cam.fov = 60.0f;
        cam.near_plane = 0.1f;
        cam.far_plane = 100.0f;
        cam.target[1] = 0.5f;
        cam.up[1] = 1.0f;
        gs->project.cameras[0] = cam;
        gs->project.camera_count = 1;
    }

    /* trigger: entity 4, pickup targeting entity 2 */
    {
        project_trigger tr = {0};
        tr.entity = 4;
        snprintf(tr.type_str, sizeof(tr.type_str), "pickup");
        tr.target = 2;
        tr.radius = 1.5f;
        gs->project.triggers[0] = tr;
        gs->project.trigger_count = 1;
    }

    ensure_scene_storage(gs, 5);
    build_scene_from_project(gs);

    /* Verify counts */
    ASSERT_EQ(gs->transform_component_count, 2);
    ASSERT_EQ(gs->rotation_component_count, 1);
    ASSERT_EQ(gs->scale_component_count, 1);
    ASSERT_EQ(gs->parent_transform_component_count, 1);
    ASSERT_EQ(gs->parent_rotation_component_count, 1);
    ASSERT_EQ(gs->velocity_component_count, 1);
    ASSERT_EQ(gs->rigid_body_component_count, 1);
    ASSERT_EQ(gs->character_controller_component_count, 1);
    ASSERT_EQ(gs->health_component_count, 1);
    ASSERT_EQ(gs->box_collider_component_count, 1);
    ASSERT_EQ(gs->capsule_collider_component_count, 1);
    ASSERT_EQ(gs->camera_component_count, 1);
    ASSERT_EQ(gs->trigger_component_count, 1);

    /* Verify transform values */
    ASSERT_EQ(gs->transform_components[0].entity_index, 0);
    ASSERT_FLOAT_EQ(gs->transform_components[0].position.x, 1.0f);
    ASSERT_FLOAT_EQ(gs->transform_components[0].position.y, 2.0f);
    ASSERT_FLOAT_EQ(gs->transform_components[0].position.z, 3.0f);
    ASSERT_EQ(gs->transform_components[1].entity_index, 2);
    ASSERT_FLOAT_EQ(gs->transform_components[1].position.x, 4.0f);

    /* Verify rotation */
    ASSERT_EQ(gs->rotation_components[0].entity_index, 1);
    ASSERT_FLOAT_EQ(gs->rotation_components[0].rotation_y_deg, 1.57f);

    /* Verify character controller */
    ASSERT_FLOAT_EQ(gs->character_controller_components[0].move_speed, 5.0f);
    ASSERT_FLOAT_EQ(gs->character_controller_components[0].jump_speed, 8.0f);

    /* Verify health */
    ASSERT_FLOAT_EQ(gs->health_components[0].health, 80.0f);
    ASSERT_FLOAT_EQ(gs->health_components[0].max_health, 100.0f);

    /* Verify capsule collider */
    ASSERT_FLOAT_EQ(gs->capsule_collider_components[0].radius, 0.3f);
    ASSERT_FLOAT_EQ(gs->capsule_collider_components[0].half_height, 0.9f);

    /* Verify camera and scene_camera_entity */
    ASSERT_EQ(gs->scene_camera_entity, 3);
    ASSERT_FLOAT_EQ(gs->camera_components[0].fov_deg, 60.0f);

    /* Verify trigger */
    ASSERT_EQ(gs->trigger_components[0].entity_index, 4);
    ASSERT_EQ(gs->trigger_components[0].type, TRIGGER_PICKUP);
    ASSERT_EQ(gs->trigger_components[0].target_entity, 2);
    ASSERT_FLOAT_EQ(gs->trigger_components[0].radius, 1.5f);
}

TEST(build_scene_fallback_camera_when_no_project_camera) {
    game_state *gs = setup_game_state();

    gs->project.scene_entity_count = 2;
    gs->project_loaded = 1;

    /* Only a transform, no camera */
    gs->project.transforms[0] = (project_transform){0, {1.0f, 0.0f, 0.0f}};
    gs->project.transform_count = 1;
    gs->project.camera_count = 0;

    ensure_scene_storage(gs, 3);
    build_scene_from_project(gs);

    /* Fallback camera should be created at entity index 2 (scene_entity_count was 2) */
    ASSERT_EQ(gs->camera_component_count, 1);
    ASSERT(gs->scene_camera_entity >= 0);
    ASSERT_EQ(gs->scene_entity_count, 3); /* grew by 1 for fallback camera */
}

/* ── Index table tests ────────────────────────────────────────────────── */

TEST(lookup_table_after_push) {
    game_state *gs = setup_game_state();
    ensure_scene_storage(gs, 8);
    gs->scene_entity_count = 4;

    /* Push components to entities 0, 2, 3 (skip 1) */
    push_transform_component(gs, 0, VEC3(1.0f, 0.0f, 0.0f));
    push_transform_component(gs, 2, VEC3(3.0f, 0.0f, 0.0f));
    push_transform_component(gs, 3, VEC3(4.0f, 0.0f, 0.0f));

    push_velocity_component(gs, 0, VEC3(0.0f, 0.0f, 1.0f));
    push_health_component(gs, 2, 100.0f, 100.0f);
    push_mesh_component(gs, 3, 0);

    /* Verify index arrays point to correct slots */
    ASSERT_EQ(gs->transform_index[0], 0);
    ASSERT_EQ(gs->transform_index[1], -1);
    ASSERT_EQ(gs->transform_index[2], 1);
    ASSERT_EQ(gs->transform_index[3], 2);

    ASSERT_EQ(gs->velocity_index[0], 0);
    ASSERT_EQ(gs->velocity_index[1], -1);

    ASSERT_EQ(gs->health_index[2], 0);
    ASSERT_EQ(gs->health_index[0], -1);

    ASSERT_EQ(gs->mesh_index[3], 0);
    ASSERT_EQ(gs->mesh_index[0], -1); /* only from add_animated_entity, not pushed here */

    /* Verify O(1) find returns correct data */
    ASSERT(find_transform_component(gs, 0) != NULL);
    ASSERT_FLOAT_EQ(find_transform_component(gs, 0)->position.x, 1.0f);
    ASSERT(find_transform_component(gs, 1) == NULL);
    ASSERT_FLOAT_EQ(find_transform_component(gs, 2)->position.x, 3.0f);
    ASSERT_FLOAT_EQ(find_transform_component(gs, 3)->position.x, 4.0f);
}

TEST(trigger_swap_and_pop_on_activation) {
    game_state *gs = setup_game_state();
    ensure_scene_storage(gs, 8);
    gs->scene_entity_count = 5;

    /* Entity 0: player with character_controller, velocity, transform, capsule collider */
    push_transform_component(gs, 0, VEC3(0.0f, 0.0f, 0.0f));
    push_velocity_component(gs, 0, VEC3(0.0f, 0.0f, 0.0f));
    push_character_controller_component(gs, 0, 5.0f, 10.0f);
    push_capsule_collider_component(gs, 0, 0.3f, 0.9f);

    /* Entity 1: key pickup trigger (target = entity 2) */
    push_transform_component(gs, 1, VEC3(0.0f, 0.0f, 0.0f));
    push_mesh_component(gs, 1, 0);
    push_trigger_component(gs, 1, TRIGGER_PICKUP, 2, 1.0f);

    /* Entity 2: door trigger */
    push_transform_component(gs, 2, VEC3(0.0f, 0.0f, 0.0f));
    push_mesh_component(gs, 2, 0);
    push_box_collider_component(gs, 2, (rect){0.0f, 0.0f, 1.0f, 1.0f}, 0.5f);
    push_trigger_component(gs, 2, TRIGGER_DOOR, -1, 1.0f);

    /* Entity 3: another trigger (should survive swap-and-pop) */
    push_transform_component(gs, 3, VEC3(10.0f, 0.0f, 0.0f));
    push_mesh_component(gs, 3, 0);
    push_trigger_component(gs, 3, TRIGGER_PICKUP, -1, 1.0f);

    ASSERT_EQ(gs->trigger_component_count, 3);
    ASSERT_EQ(gs->trigger_index[1], 0);
    ASSERT_EQ(gs->trigger_index[2], 1);
    ASSERT_EQ(gs->trigger_index[3], 2);

    /* Run trigger update — entity 0 overlaps entities 1 and 2 (at origin) */
    gs->dt = 1.0f / 60.0f;
    update_triggers(gs);

    /* Key and door triggers removed via swap-and-pop, entity 3 trigger survives */
    ASSERT_EQ(gs->trigger_component_count, 1);
    ASSERT_EQ(gs->trigger_components[0].entity_index, 3);
    ASSERT_EQ(gs->trigger_index[3], 0);
    ASSERT_EQ(gs->trigger_index[1], -1);
    ASSERT_EQ(gs->trigger_index[2], -1);
}

/* ── System table tests ───────────────────────────────────────────────── */

static int g_sys_order[8];
static int g_sys_order_idx;

static void test_sys_a(game_state *gs) { (void)gs; g_sys_order[g_sys_order_idx++] = 0; }
static void test_sys_b(game_state *gs) { (void)gs; g_sys_order[g_sys_order_idx++] = 1; }
static void test_sys_c(game_state *gs) { (void)gs; g_sys_order[g_sys_order_idx++] = 2; }

TEST(system_table_registers_and_runs) {
    game_state gs;
    memset(&gs, 0, sizeof(gs));

    /* Register 3 systems: A (always), B (play only), C (always) */
    gs.system_count = 0;
    register_system(&gs, "a", test_sys_a, 0);
    register_system(&gs, "b", test_sys_b, 1);
    register_system(&gs, "c", test_sys_c, 0);

    ASSERT_EQ(gs.system_count, 3);
    ASSERT(gs.systems[0].fn == test_sys_a);
    ASSERT(gs.systems[1].fn == test_sys_b);
    ASSERT(gs.systems[2].fn == test_sys_c);

    /* Play mode: all 3 run in order */
    g_sys_order_idx = 0;
    gs.editor_play_mode = 1;
    update_engine(&gs);
    ASSERT_EQ(g_sys_order_idx, 3);
    ASSERT_EQ(g_sys_order[0], 0);
    ASSERT_EQ(g_sys_order[1], 1);
    ASSERT_EQ(g_sys_order[2], 2);

    /* Edit mode: B skipped */
    g_sys_order_idx = 0;
    gs.editor_play_mode = 0;
    update_engine(&gs);
    ASSERT_EQ(g_sys_order_idx, 2);
    ASSERT_EQ(g_sys_order[0], 0);
    ASSERT_EQ(g_sys_order[1], 2);
}

/* ── Implicit visibility propagation ──────────────────────────────────── */

TEST(implicit_visibility_propagation) {
    game_state *gs = setup_game_state();
    int mi, pi;

    /* 3 entities: grandparent(0) -> parent(1) -> child(2), all with mesh visible=1 */
    gs->scene_entity_count = 3;

    /* Mesh components */
    mi = gs->mesh_component_count++;
    gs->mesh_components[mi].entity_index = 0;
    gs->mesh_components[mi].visible = 1;
    gs->mesh_components[mi].model_asset_index = 0;
    gs->mesh_index[0] = mi;

    mi = gs->mesh_component_count++;
    gs->mesh_components[mi].entity_index = 1;
    gs->mesh_components[mi].visible = 1;
    gs->mesh_components[mi].model_asset_index = 0;
    gs->mesh_index[1] = mi;

    mi = gs->mesh_component_count++;
    gs->mesh_components[mi].entity_index = 2;
    gs->mesh_components[mi].visible = 1;
    gs->mesh_components[mi].model_asset_index = 0;
    gs->mesh_index[2] = mi;

    /* Parent transform: 1->0, 2->1 */
    pi = gs->parent_transform_component_count++;
    gs->parent_transform_components[pi].entity_index = 1;
    gs->parent_transform_components[pi].parent_entity_index = 0;
    gs->parent_transform_index[1] = pi;

    pi = gs->parent_transform_component_count++;
    gs->parent_transform_components[pi].entity_index = 2;
    gs->parent_transform_components[pi].parent_entity_index = 1;
    gs->parent_transform_index[2] = pi;

    /* All visible */
    ASSERT(is_entity_visible(gs, 0));
    ASSERT(is_entity_visible(gs, 1));
    ASSERT(is_entity_visible(gs, 2));

    /* Hide grandparent -> child should be hidden (propagated) */
    memset(gs->entity_visible, 0, sizeof(gs->entity_visible));
    gs->mesh_components[gs->mesh_index[0]].visible = 0;
    ASSERT(!is_entity_visible(gs, 0));
    ASSERT(!is_entity_visible(gs, 1));
    ASSERT(!is_entity_visible(gs, 2));

    /* Clear cache, show grandparent, hide parent -> child hidden, grandparent visible */
    memset(gs->entity_visible, 0, sizeof(gs->entity_visible));
    gs->mesh_components[gs->mesh_index[0]].visible = 1;
    gs->mesh_components[gs->mesh_index[1]].visible = 0;
    ASSERT(is_entity_visible(gs, 0));
    ASSERT(!is_entity_visible(gs, 1));
    ASSERT(!is_entity_visible(gs, 2));
}

TEST(visibility_cache_cleared_per_frame) {
    game_state *gs = setup_game_state();
    int mi;

    gs->scene_entity_count = 1;

    mi = gs->mesh_component_count++;
    gs->mesh_components[mi].entity_index = 0;
    gs->mesh_components[mi].visible = 1;
    gs->mesh_components[mi].model_asset_index = 0;
    gs->mesh_index[0] = mi;

    /* Evaluate and cache */
    ASSERT(is_entity_visible(gs, 0));
    ASSERT_EQ(gs->entity_visible[0], 1);

    /* system_clear_draw_lists resets cache */
    system_clear_draw_lists(gs);
    ASSERT_EQ(gs->entity_visible[0], 0);

    /* Change visibility, verify new result */
    gs->mesh_components[mi].visible = 0;
    ASSERT(!is_entity_visible(gs, 0));
    ASSERT_EQ(gs->entity_visible[0], -1);
}

TEST(entity_without_mesh_is_passthrough) {
    game_state *gs = setup_game_state();
    int mi, pi;

    /* Parent(0) with no mesh, child(1) with visible mesh */
    gs->scene_entity_count = 2;

    mi = gs->mesh_component_count++;
    gs->mesh_components[mi].entity_index = 1;
    gs->mesh_components[mi].visible = 1;
    gs->mesh_components[mi].model_asset_index = 0;
    gs->mesh_index[1] = mi;

    pi = gs->parent_transform_component_count++;
    gs->parent_transform_components[pi].entity_index = 1;
    gs->parent_transform_components[pi].parent_entity_index = 0;
    gs->parent_transform_index[1] = pi;

    /* Child visible — parent is passthrough (no mesh) */
    ASSERT(is_entity_visible(gs, 0));
    ASSERT(is_entity_visible(gs, 1));

    /* Give parent a mesh with visible=0 -> child now hidden */
    memset(gs->entity_visible, 0, sizeof(gs->entity_visible));
    mi = gs->mesh_component_count++;
    gs->mesh_components[mi].entity_index = 0;
    gs->mesh_components[mi].visible = 0;
    gs->mesh_components[mi].model_asset_index = 0;
    gs->mesh_index[0] = mi;

    ASSERT(!is_entity_visible(gs, 0));
    ASSERT(!is_entity_visible(gs, 1));
}

/* ── Tests: Play mode init_engine integration ────────────────────────── */

TEST(init_engine_sets_up_attack_state_from_clips) {
    game_state *gs = setup_game_state();
    scene_model_asset *asset;
    init_test_clips();

    /* Name clips so the auto-setup finds them */
    strncpy(g_clips[0].name, "Idle", sizeof(g_clips[0].name));
    strncpy(g_clips[1].name, "Run_A", sizeof(g_clips[1].name));
    strncpy(g_clips[2].name, "Melee_1H_Attack_Chop", sizeof(g_clips[2].name));

    /* Set up a loaded model asset with skeleton + clips + primitives */
    asset = setup_test_model_asset(gs);

    /* Project: 1 entity with mesh + anim, using a .glb path so
       resolve_project_model_path accepts it without an asset table entry */
    gs->project.scene_entity_count = 1;
    gs->project_loaded = 1;
    {
        project_mesh pm = {0};
        pm.entity = 0;
        pm.visible = 1;
        strncpy(pm.model, "test.glb", sizeof(pm.model));
        gs->project.meshes[0] = pm;
        gs->project.mesh_count = 1;
    }
    {
        project_anim pa = {0};
        pa.entity = 0;
        pa.playing = 1;
        pa.speed = 1.0f;
        gs->project.anims[0] = pa;
        gs->project.anim_count = 1;
    }

    /* setup_test_model_asset already set scene_model_assets[0] with skeleton,
       clips, and primitives.  Just add the path so the dedup lookup works. */
    strncpy(asset->path, "test.glb", sizeof(asset->path));
    strncpy(asset->key, "test.glb", sizeof(asset->key));

    /* Call init_engine — this triggers the full play mode path:
       clear → build_scene_from_project → sync_primary → SM auto-setup */
    init_engine(gs);

    /* SM should have 3 states: idle, run, attack */
    ASSERT_EQ(gs->anim.state_count, 3);
    ASSERT_EQ(gs->anim.states[2].looping, 0); /* attack is non-looping */
    ASSERT_EQ(gs->anim.states[2].clip_index, 2); /* Melee_1H_Attack_Chop = clip 2 */

    /* Should have rules: idle→run, run→idle, idle→attack, run→attack, attack→idle */
    ASSERT(gs->anim.rule_count >= 5);

    /* At least one entity registered */
    ASSERT(gs->anim.instance_count >= 1);

    /* Run a frame to make sure it doesn't crash */
    gs->editor_play_mode = 1;
    gs->dt = 1.0f / 60.0f;
    anim_sm_update(&gs->anim, gs, gs->dt);
}

/* ── Tests: Input button attack transitions ──────────────────────────── */

TEST(input_button_triggers_attack_transition) {
    game_state *gs = setup_game_state();
    anim_sm *sm;
    int frame;
    init_test_clips();
    setup_test_model_asset(gs);
    sm = &gs->anim;
    anim_sm_init_pool(sm, gs->gameplay);

    /* idle=0(clip0), run=1(clip1), attack=2(clip2, non-looping) */
    anim_sm_add_state(sm, "idle", 0, 1);
    anim_sm_add_state(sm, "run", 1, 1);
    anim_sm_add_state(sm, "attack", 2, 0);
    anim_sm_add_rule(sm, 0, 2, ANIM_COND_INPUT_BUTTON, 2.0f, 0.08f);
    anim_sm_add_rule(sm, 2, 0, ANIM_COND_CLIP_FINISHED, 0.0f, 0.15f);

    add_animated_entity(gs, 0, 0, 0, 1.0f);
    anim_sm_register_entity(sm, 0, 0, 0, 2);

    /* Press attack button */
    gs->input.input_mask = INPUT_B;
    anim_sm_update(sm, gs, 1.0f / 60.0f);

    /* Entity should be blending to attack */
    ASSERT_EQ(sm->states[0].count, 0);
    ASSERT_EQ(sm->blend_count, 1);
    ASSERT_EQ(sm->blends[0].to_clip, 2);
    ASSERT_EQ(sm->blends[0].to_state, 2);

    /* Complete the 0.08s blend */
    for (frame = 0; frame < 10 && sm->blend_count > 0; frame++)
        anim_sm_update(sm, gs, 1.0f / 60.0f);
    ASSERT_EQ(sm->blend_count, 0);
    ASSERT_EQ(sm->states[2].count, 1);

    /* Clear input, advance past clip duration (0.4s) — returns to idle */
    gs->input.input_mask = 0;
    for (frame = 0; frame < 40; frame++)
        anim_sm_update(sm, gs, 1.0f / 60.0f);

    /* Should have blended back or completed blend to idle */
    for (frame = 0; frame < 15 && sm->blend_count > 0; frame++)
        anim_sm_update(sm, gs, 1.0f / 60.0f);
    ASSERT_EQ(sm->states[0].count, 1);
    ASSERT_EQ(sm->states[2].count, 0);
}

TEST(attack_does_not_retrigger_during_blend) {
    game_state *gs = setup_game_state();
    anim_sm *sm;
    init_test_clips();
    setup_test_model_asset(gs);
    sm = &gs->anim;
    anim_sm_init_pool(sm, gs->gameplay);

    anim_sm_add_state(sm, "idle", 0, 1);
    anim_sm_add_state(sm, "run", 1, 1);
    anim_sm_add_state(sm, "attack", 2, 0);
    anim_sm_add_rule(sm, 0, 2, ANIM_COND_INPUT_BUTTON, 2.0f, 0.08f);
    anim_sm_add_rule(sm, 2, 0, ANIM_COND_CLIP_FINISHED, 0.0f, 0.15f);

    add_animated_entity(gs, 0, 0, 0, 1.0f);
    anim_sm_register_entity(sm, 0, 0, 0, 2);

    /* Press attack button — triggers blend */
    gs->input.input_mask = INPUT_B;
    anim_sm_update(sm, gs, 1.0f / 60.0f);
    ASSERT_EQ(sm->blend_count, 1);

    /* Second update with button still held — blend count stays at 1 */
    anim_sm_update(sm, gs, 1.0f / 60.0f);
    ASSERT_EQ(sm->blend_count, 1);
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(void) {
    printf("\n=== Anim SM Integration Tests ===\n\n");

    init_test_clips();

    /* Pool allocation */
    run_pool_init_succeeds();
    run_pool_init_fails_on_tiny_arena();

    /* State management */
    run_add_state_returns_sequential_indices();
    run_add_state_stores_clip_and_name();
    run_add_state_overflow_returns_minus_one();

    /* Entity registration */
    run_register_entity_assigns_skin_mats_offset();
    run_register_entity_inserts_into_state_table();

    /* State table operations */
    run_insert_and_remove_entity_from_state();
    run_remove_nonexistent_entity_is_noop();

    /* Find instance */
    run_find_instance_returns_correct_entity();
    run_find_instance_returns_null_for_missing();

    /* Rules */
    run_add_rule_stores_correctly();

    /* 5-phase pipeline */
    run_phase1_advances_anim_times();
    run_velocity_above_triggers_transition_to_blend();
    run_velocity_below_threshold_no_transition();
    run_blend_completes_and_entity_moves_to_dest_state();
    run_phase4_writes_skin_mats_for_state_entities();
    run_phase5_writes_blended_skin_mats();
    run_multiple_entities_have_independent_poses();

    /* Draw commands */
    run_build_mesh_draw_commands_sets_bone_offset();

    /* Bidirectional */
    run_bidirectional_transition_idle_to_walk_and_back();

    /* Velocity-driven transitions */
    run_velocity_on_same_entity_triggers_transition();
    run_zero_velocity_stays_idle();

    /* Full project scene (matches project.toml with floor entities) */
    run_parent_child_velocity_triggers_transition();
    run_parent_child_no_velocity_stays_idle();
    run_full_scene_bidirectional_transition();

    /* Wall collision + sliding */
    run_wall_blocks_movement_into_wall();
    run_wall_allows_sliding_along_wall();
    run_no_wall_allows_free_movement();

    /* Edge cases */
    run_update_with_zero_instances_is_noop();
    run_anim_time_wraps_for_looping_clips();

    /* Trigger system */
    run_key_pickup_opens_door();

    /* Index table correctness */
    run_lookup_table_after_push();
    run_trigger_swap_and_pop_on_activation();

    /* Scene building from project */
    run_build_scene_from_project_creates_runtime_components();
    run_build_scene_fallback_camera_when_no_project_camera();

    /* System table */
    run_system_table_registers_and_runs();

    /* Play mode / init_engine integration */
    run_init_engine_sets_up_attack_state_from_clips();

    /* Input-button attack transitions */
    run_input_button_triggers_attack_transition();
    run_attack_does_not_retrigger_during_blend();

    /* Implicit visibility propagation */
    run_implicit_visibility_propagation();
    run_visibility_cache_cleared_per_frame();
    run_entity_without_mesh_is_passthrough();

    printf("\n  ── Results: %d passed, %d failed, %d total ──\n\n",
           tests_passed, tests_failed, tests_run);

    return tests_failed > 0 ? 1 : 0;
}
