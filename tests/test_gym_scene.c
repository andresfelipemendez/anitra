/* ── tests/test_gym_scene.c ─────────────────────────────────────────────────
   Integration / play-mode gym scene test.

   Spawns GYM_AGENT_COUNT character instances each driven by an independent
   emulated_input_component, then steps a lightweight ECS physics pipeline
   for N frames to verify per-agent movement, isolation, and correctness.

   Uses the real engine systems end-to-end:
     • Each bot writes its input_state into cc->own_input (per-entity slot).
     • run_character_controller_system reads cc->own_input → sets velocity.
     • apply_movement integrates velocity → updates position.
     • update_triggers checks TRIGGER_ZONE proximity.
   If any system is broken the trigger-based tests will fail.

   Build (Windows):
     .\tcc.exe -Blib/tcc-windows -g -o build/Debug/test_gym_scene.exe ^
       -Isrc -Isrc/engine -Isrc/editor -Ilib/SDL3/include -Ilib/cgltf ^
       -Ilib/toml-c tests/test_gym_scene.c

   Run:
     build\Debug\test_gym_scene.exe
   ──────────────────────────────────────────────────────────────────────────*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Suppress DLL export decorators when compiling engine sources directly.
   On Windows, export.h defines EXPORT as __declspec(dllexport); override
   it so that engine symbols compile as plain functions in this TU. */
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>
#define EXPORT_H        /* prevent export.h from running */
#define EXPORT          /* no dllexport in the test binary */

#include "game.h"
#include "state_migration.gen.h"

/* ── Stubs for GPU / asset / debug dependencies ────────────────────────── */

void debug_draw_rect(debug_renderer *dr, vec2 c, float w, float h, debug_color col)
    { (void)dr;(void)c;(void)w;(void)h;(void)col; }
void debug_draw_line(debug_renderer *dr, vec2 s, vec2 e, debug_color col)
    { (void)dr;(void)s;(void)e;(void)col; }

GltfModel load_glb(const char *p, arena *a)
    { GltfModel m; memset(&m,0,sizeof(m)); (void)p;(void)a; return m; }
void load_animations_glb(const char *p, GltfModel *m, arena *a)
    { (void)p;(void)m;(void)a; }
void gltf_set_gpu_device(void *d)        { (void)d; }
void gltf_set_boot_profiler(void (*f)(int)) { (void)f; }
void gltf_tex_cache_init(void)           {}
void gltf_tex_cache_free(void)           {}

int project_load(const char *path, project_data *out)
    { (void)path; memset(out,0,sizeof(*out)); return -1; }

const project_mesh *project_find_mesh(const project_data *p, int e) {
    int i; if (!p) return NULL;
    for (i = 0; i < p->mesh_count; i++)
        if (p->meshes[i].entity == e) return &p->meshes[i];
    return NULL;
}
const project_anim *project_find_anim(const project_data *p, int e) {
    int i; if (!p) return NULL;
    for (i = 0; i < p->anim_count; i++)
        if (p->anims[i].entity == e) return &p->anims[i];
    return NULL;
}
const project_box_collider *project_find_box_collider(const project_data *p, int e) {
    int i; if (!p) return NULL;
    for (i = 0; i < p->box_collider_count; i++)
        if (p->box_colliders[i].entity == e) return &p->box_colliders[i];
    return NULL;
}
const char *project_find_asset(const project_data *p, const char *key,
                                project_asset_type type) {
    int i; if (!p || !key || !key[0]) return NULL;
    for (i = 0; i < p->asset_count; i++)
        if (p->assets[i].type == type && strcmp(p->assets[i].key, key) == 0)
            return p->assets[i].path;
    return NULL;
}

/* ── SDL stubs ──────────────────────────────────────────────────────────────
   engine.c uses SDL threads and perf counters for parallel model loading.
   load_glb is stubbed above to return an empty model, so task_count is
   always 0 and these functions are never actually called — but the linker
   still needs the symbols.                                               */

Uint64 SDL_GetPerformanceCounter(void)   { return 0; }
Uint64 SDL_GetPerformanceFrequency(void) { return 1000000; }

SDL_Thread *SDL_CreateThreadRuntime(SDL_ThreadFunction fn, const char *name,
                                    void *data,
                                    SDL_FunctionPointer begin_fn,
                                    SDL_FunctionPointer end_fn) {
    (void)fn; (void)name; (void)data; (void)begin_fn; (void)end_fn;
    return NULL;
}
void SDL_WaitThread(SDL_Thread *thread, int *status) {
    (void)thread; if (status) *status = 0;
}

/* ── Include engine sources into this translation unit ─────────────────── */

#include "engine/anim.c"
#include "engine/engine.c"

/* physics.c duplicates several static helpers from engine.c — rename them
   with macros so both can coexist in a single TU. */
#define find_transform_component            phys_find_tc
#define find_parent_transform_component     phys_find_ptc
#define find_health_component               phys_find_hc
#define find_velocity_component             phys_find_vc
#define find_rigid_body_component           phys_find_rbc
#define find_character_controller_component phys_find_ccc
#define find_box_collider_component         phys_find_boxc
#define find_capsule_collider_component     phys_find_capc
#define find_camera_component               phys_find_cam
#define find_mesh_component                 phys_find_mc
#define resolve_world_position              phys_resolve_wp
#define resolve_parent_world_offset         phys_resolve_pwo
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

/* ── Minimal test harness (mirrors test_anim_sm.c style) ───────────────── */

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    static void test_##name(void); \
    static void run_##name(void) { \
        tests_run++; \
        printf("  %-60s ", #name); \
        test_##name(); \
        tests_passed++; \
        printf("PASS\n"); \
    } \
    static void test_##name(void)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("FAIL\n    %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        tests_failed++; tests_passed--; return; \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        printf("FAIL\n    %s:%d: %s == %d, expected %d\n", \
               __FILE__, __LINE__, #a, (int)(a), (int)(b)); \
        tests_failed++; tests_passed--; return; \
    } \
} while(0)

#define ASSERT_FLOAT_GT(a, b) do { \
    float _a = (float)(a), _b = (float)(b); \
    if (!(_a > _b)) { \
        printf("FAIL\n    %s:%d: %s (%.4f) should be > %.4f\n", \
               __FILE__, __LINE__, #a, _a, _b); \
        tests_failed++; tests_passed--; return; \
    } \
} while(0)

#define ASSERT_FLOAT_NEAR(a, b, eps) do { \
    float _a = (float)(a), _b = (float)(b), _e = (float)(eps); \
    if (_a - _b > _e || _b - _a > _e) { \
        printf("FAIL\n    %s:%d: |%s - %.4f| > %.4f  (got %.4f)\n", \
               __FILE__, __LINE__, #a, _b, _e, _a); \
        tests_failed++; tests_passed--; return; \
    } \
} while(0)

/* ── emulated_input_component ──────────────────────────────────────────────
   Lives only in the test — not part of game_state.
   Each agent has its own input_state computed every frame from its behavior.

   3D physics conventions (Y = up):
       horizontal → velocity.x  (strafe)
       vertical   → velocity.z  (forward/back)
       velocity.y is reserved for gravity/jump (not used here)
   ────────────────────────────────────────────────────────────────────────*/

#define GYM_AGENT_COUNT  8
#define GYM_ARENA_SIZE   (8 * 1024 * 1024)

typedef enum {
    BOT_IDLE,           /* stationary */
    BOT_WALK_FORWARD,   /* vertical = +1.0  →  velocity.z = +move_speed */
    BOT_STRAFE_RIGHT,   /* horizontal = +1.0 → velocity.x = +move_speed */
    BOT_CIRCLE,         /* horizontal = cos(t+phase), vertical = sin(t+phase) */
} bot_behavior;

typedef struct {
    int          entity_index;
    bot_behavior behavior;
    float        phase;     /* angular offset used by BOT_CIRCLE */
    input_state  input;     /* current frame's computed input — written by bot_update_input */
} emulated_input_component;

/* ── TODO (your contribution): bot_update_input ────────────────────────────
   Fill in the per-behavior input generation below.

   This function is called once per bot per frame, before the character
   controller runs. It should write values into bot->input based on the
   bot's behavior type and the accumulated simulation time t (in seconds).

   input_state fields:
       horizontal  — [-1, 1]  maps to velocity.x
       vertical    — [-1, 1]  maps to velocity.y  (with use_gravity=0)
       input_mask  — bitfield of InputButton (INPUT_A = jump, etc.)

   Considerations:
     • BOT_CIRCLE should produce a smooth closed orbit — think about how
       to normalize the (cos, sin) vector so speed stays constant.
     • BOT_WALK_FORWARD and BOT_STRAFE_RIGHT are trivial but are they
       always exactly 1.0, or should they ramp up from 0?
     • Should phase affect anything other than BOT_CIRCLE?
   ────────────────────────────────────────────────────────────────────────*/
static void bot_update_input(emulated_input_component *bot, float t) {
    memset(&bot->input, 0, sizeof(bot->input));
    switch (bot->behavior) {
    case BOT_IDLE:
        /* TODO: nothing to set — or add a small idle drift if you want */
        break;
    case BOT_WALK_FORWARD:
        bot->input.vertical = 1.0f;
        break;
    case BOT_STRAFE_RIGHT:
        bot->input.horizontal = 1.0f;
        break;
    case BOT_CIRCLE:
        bot->input.horizontal = cosf(t + bot->phase);
        bot->input.vertical   = sinf(t + bot->phase);
        break;
    }
}

/* ── Gym helpers ────────────────────────────────────────────────────────── */

/* Write the bot's computed input into its character controller component so
   run_character_controller_system can process it through the real engine path. */
static void bot_apply_to_character_controller(game_state *gs,
                                              emulated_input_component *bot) {
    int entity = bot->entity_index;
    int idx;
    if (entity < 0 || entity >= gs->scene_entity_count) return;
    idx = gs->character_controller_index[entity];
    if (idx < 0) return;
    gs->character_controller_components[idx].own_input    = bot->input;
    gs->character_controller_components[idx].use_own_input = 1;
}

/* Step the gym simulation for one frame:
   1. Bots write their computed input into cc->own_input.
   2. run_character_controller_system reads cc->own_input → sets velocity (real path).
   3. apply_movement integrates velocity → moves positions.
   4. update_triggers evaluates TRIGGER_ZONE proximity. */
static void gym_tick(game_state *gs, emulated_input_component *bots,
                     int bot_count, float t) {
    int i;
    for (i = 0; i < bot_count; i++) {
        bot_update_input(&bots[i], t);
        bot_apply_to_character_controller(gs, &bots[i]);
    }
    run_character_controller_system(gs);
    apply_movement(gs);
    update_triggers(gs);
}

/* Spawn a TRIGGER_ZONE entity: fires when any CC entity enters its radius.
   The test then checks trig->activated and trig->activated_by_entity. */
static void gym_spawn_trigger_zone(game_state *gs, int entity_index,
                                   Vec3 pos, float radius) {
    int i;
    if (entity_index >= gs->scene_entity_capacity) return;
    if (entity_index >= gs->scene_entity_count)
        gs->scene_entity_count = entity_index + 1;
    memset(&gs->scene_entities[entity_index], 0, sizeof(entity));

    /* transform */
    i = gs->transform_component_count++;
    gs->transform_components[i].entity_index = entity_index;
    gs->transform_components[i].position     = pos;
    gs->transform_index[entity_index]        = i;

    /* trigger */
    i = gs->trigger_component_count++;
    gs->trigger_components[i].entity_index        = entity_index;
    gs->trigger_components[i].type                = TRIGGER_ZONE;
    gs->trigger_components[i].target_entity       = -1;
    gs->trigger_components[i].radius              = radius;
    gs->trigger_components[i].activated           = 0;
    gs->trigger_components[i].activated_by_entity = -1;
    gs->trigger_index[entity_index]               = i;
}

/* ── game_state bootstrap (mirrors setup_game_state from test_anim_sm.c) ── */

static uint8_t g_arena_buf[GYM_ARENA_SIZE];

static game_state *setup_game_state(void) {
    arena   *a;
    game_state *gs;

    memset(g_arena_buf, 0, sizeof(g_arena_buf));
    a           = (arena *)g_arena_buf;
    a->base     = g_arena_buf + sizeof(arena);
    a->capacity = GYM_ARENA_SIZE - (uint32_t)sizeof(arena);
    a->used     = 0;

    gs = (game_state *)arena_alloc(a, (uint32_t)sizeof(game_state), 16, "gs");
    memset(gs, 0, sizeof(*gs));
    gs->root_arena               = a;
    gs->gameplay                 = arena_alloc_subarena(a, 4*1024*1024, 16, "gameplay");
    gs->scene_primary_skinned_entity = -1;
    gs->scene_camera_entity          = -1;
    gs->dt                       = 1.0f / 60.0f;
    gs->editor_play_mode         = 1;   /* play mode: systems run */

    /* Component storage */
#define ALLOC_COMP(field, cap_field, type, n, tag) \
    gs->field = (type *)arena_alloc(gs->gameplay, \
        (uint32_t)((n) * sizeof(type)), 8, tag); \
    gs->cap_field = (n)

    ALLOC_COMP(scene_entities,              scene_entity_capacity,              entity,                         GYM_AGENT_COUNT + 4, "entities");
    ALLOC_COMP(transform_components,        transform_component_capacity,       transform_component,            GYM_AGENT_COUNT + 4, "transforms");
    ALLOC_COMP(velocity_components,         velocity_component_capacity,        velocity_component,             GYM_AGENT_COUNT + 4, "velocities");
    ALLOC_COMP(rotation_components,         rotation_component_capacity,        rotation_component,             GYM_AGENT_COUNT + 4, "rotations");
    ALLOC_COMP(rigid_body_components,       rigid_body_component_capacity,      rigid_body_component,           GYM_AGENT_COUNT + 4, "rigidbodies");
    ALLOC_COMP(character_controller_components, character_controller_component_capacity, character_controller_component, GYM_AGENT_COUNT + 4, "ccs");
    ALLOC_COMP(capsule_collider_components, capsule_collider_component_capacity, capsule_collider_component,    GYM_AGENT_COUNT + 4, "capsules");
    ALLOC_COMP(health_components,           health_component_capacity,          health_component,               GYM_AGENT_COUNT + 4, "health");
    ALLOC_COMP(mesh_components,             mesh_component_capacity,            mesh_component,                 GYM_AGENT_COUNT + 4, "meshes");
    ALLOC_COMP(animation_components,        animation_component_capacity,       animation_component,            GYM_AGENT_COUNT + 4, "anims");
    ALLOC_COMP(animation_transition_entries, animation_transition_capacity,     animation_transition_entry,     8,                   "anim_trans");
    ALLOC_COMP(box_collider_components,     box_collider_component_capacity,    box_collider_component,         GYM_AGENT_COUNT + 4, "boxes");
    ALLOC_COMP(parent_transform_components, parent_transform_component_capacity, parent_transform_component,    GYM_AGENT_COUNT + 4, "ptcs");
    ALLOC_COMP(trigger_components,          trigger_component_capacity,         trigger_component,              4,                   "triggers");
    ALLOC_COMP(bone_attach_components,      bone_attach_component_capacity,     bone_attach_component,          4,                   "bone_attaches");
    ALLOC_COMP(scale_components,            scale_component_capacity,           scale_component,                GYM_AGENT_COUNT + 4, "scales");
    ALLOC_COMP(camera_components,           camera_component_capacity,          camera_component,               4,                   "cameras");
#undef ALLOC_COMP

    /* Draw list (needed by system_clear_draw_lists) */
    gs->dl.meshes = (mesh_draw_command *)arena_alloc(gs->gameplay,
        (uint32_t)(DRAW_LIST_MAX_MESH_COMMANDS * sizeof(mesh_draw_command)), 8, "dl_meshes");
    gs->dl.mesh_capacity = DRAW_LIST_MAX_MESH_COMMANDS;
    gs->dl.sprites = (draw_command *)arena_alloc(gs->gameplay,
        (uint32_t)(8 * sizeof(draw_command)), 8, "dl_sprites");
    gs->dl.sprite_capacity = 8;
    gs->dl.lines = (debug_line_command *)arena_alloc(gs->gameplay,
        (uint32_t)(8 * sizeof(debug_line_command)), 8, "dl_lines");
    gs->dl.line_capacity = 8;

    /* All index lookups start at -1 (no component) */
    memset(gs->parent_index,                0xFF, sizeof(gs->parent_index));
    memset(gs->parent_transform_index,      0xFF, sizeof(gs->parent_transform_index));
    memset(gs->parent_rotation_index,       0xFF, sizeof(gs->parent_rotation_index));
    memset(gs->transform_index,             0xFF, sizeof(gs->transform_index));
    memset(gs->rotation_index,              0xFF, sizeof(gs->rotation_index));
    memset(gs->scale_index,                 0xFF, sizeof(gs->scale_index));
    memset(gs->velocity_index,              0xFF, sizeof(gs->velocity_index));
    memset(gs->rigid_body_index,            0xFF, sizeof(gs->rigid_body_index));
    memset(gs->character_controller_index,  0xFF, sizeof(gs->character_controller_index));
    memset(gs->health_index,                0xFF, sizeof(gs->health_index));
    memset(gs->box_collider_index,          0xFF, sizeof(gs->box_collider_index));
    memset(gs->capsule_collider_index,      0xFF, sizeof(gs->capsule_collider_index));
    memset(gs->mesh_index,                  0xFF, sizeof(gs->mesh_index));
    memset(gs->animation_index,             0xFF, sizeof(gs->animation_index));
    memset(gs->animation_transition_index,  0xFF, sizeof(gs->animation_transition_index));
    memset(gs->camera_index,                0xFF, sizeof(gs->camera_index));
    memset(gs->trigger_index,               0xFF, sizeof(gs->trigger_index));
    memset(gs->bone_attach_index,           0xFF, sizeof(gs->bone_attach_index));

    return gs;
}

/* Spawn one character agent into the scene and its matching emulated_input.
   Agents use use_gravity=0 so only X/Z movement (no falling). */
static void gym_spawn_agent(game_state *gs, emulated_input_component *bot,
                            int entity_index, Vec3 start_pos,
                            bot_behavior behavior, float phase) {
    int i;

    /* Grow entity list */
    if (entity_index >= gs->scene_entity_capacity) return;
    if (entity_index >= gs->scene_entity_count)
        gs->scene_entity_count = entity_index + 1;
    memset(&gs->scene_entities[entity_index], 0, sizeof(entity));

    /* transform */
    i = gs->transform_component_count++;
    gs->transform_components[i].entity_index = entity_index;
    gs->transform_components[i].position     = start_pos;
    gs->transform_index[entity_index]        = i;

    /* velocity */
    i = gs->velocity_component_count++;
    gs->velocity_components[i].entity_index = entity_index;
    gs->velocity_components[i].velocity     = VEC3(0,0,0);
    gs->velocity_index[entity_index]        = i;

    /* rotation */
    i = gs->rotation_component_count++;
    gs->rotation_components[i].entity_index   = entity_index;
    gs->rotation_components[i].rotation_y_deg = 0.0f;
    gs->rotation_index[entity_index]          = i;

    /* rigid body (no gravity → 2D movement) */
    i = gs->rigid_body_component_count++;
    gs->rigid_body_components[i].entity_index = entity_index;
    gs->rigid_body_components[i].use_gravity  = 0;
    gs->rigid_body_index[entity_index]        = i;

    /* character controller */
    i = gs->character_controller_component_count++;
    gs->character_controller_components[i].entity_index = entity_index;
    gs->character_controller_components[i].move_speed   = 5.0f;
    gs->character_controller_components[i].jump_speed   = 8.5f;
    gs->character_controller_index[entity_index]        = i;

    /* capsule collider */
    i = gs->capsule_collider_component_count++;
    gs->capsule_collider_components[i].entity_index = entity_index;
    gs->capsule_collider_components[i].radius       = 0.4f;
    gs->capsule_collider_components[i].half_height  = 0.9f;
    gs->capsule_collider_index[entity_index]        = i;

    /* emulated input */
    bot->entity_index = entity_index;
    bot->behavior     = behavior;
    bot->phase        = phase;
    memset(&bot->input, 0, sizeof(bot->input));
}

/* Convenience: return the transform for entity_index (assert-safe) */
static transform_component *agent_transform(game_state *gs, int entity_index) {
    int idx = gs->transform_index[entity_index];
    return (idx >= 0) ? &gs->transform_components[idx] : NULL;
}

static velocity_component *agent_velocity(game_state *gs, int entity_index) {
    int idx = gs->velocity_index[entity_index];
    return (idx >= 0) ? &gs->velocity_components[idx] : NULL;
}

/* ── Tests ──────────────────────────────────────────────────────────────── */

TEST(gym_spawn_N_agents) {
    game_state             *gs   = setup_game_state();
    emulated_input_component bots[GYM_AGENT_COUNT];
    int i;

    for (i = 0; i < GYM_AGENT_COUNT; i++) {
        Vec3 pos = VEC3((float)(i * 3), 0.0f, 0.0f);
        gym_spawn_agent(gs, &bots[i], i, pos, BOT_IDLE, 0.0f);
    }

    ASSERT_EQ(gs->scene_entity_count,              GYM_AGENT_COUNT);
    ASSERT_EQ(gs->transform_component_count,       GYM_AGENT_COUNT);
    ASSERT_EQ(gs->velocity_component_count,        GYM_AGENT_COUNT);
    ASSERT_EQ(gs->character_controller_component_count, GYM_AGENT_COUNT);
    ASSERT_EQ(gs->capsule_collider_component_count,GYM_AGENT_COUNT);

    /* Verify each agent's start position and lookup */
    for (i = 0; i < GYM_AGENT_COUNT; i++) {
        transform_component *tc = agent_transform(gs, i);
        ASSERT(tc != NULL);
        ASSERT_FLOAT_NEAR(tc->position.x, (float)(i * 3), 0.001f);
    }
}

TEST(gym_idle_agent_does_not_move) {
    game_state               *gs  = setup_game_state();
    emulated_input_component  bot;
    transform_component      *tc;
    int frame;

    gym_spawn_agent(gs, &bot, 0, VEC3(0,0,0), BOT_IDLE, 0.0f);

    for (frame = 0; frame < 60; frame++)
        gym_tick(gs, &bot, 1, frame * gs->dt);

    tc = agent_transform(gs, 0);
    ASSERT(tc != NULL);
    ASSERT_FLOAT_NEAR(tc->position.x, 0.0f, 0.001f);
    ASSERT_FLOAT_NEAR(tc->position.y, 0.0f, 0.001f);
    ASSERT_FLOAT_NEAR(tc->position.z, 0.0f, 0.001f);
}

TEST(gym_walk_forward_agent_moves_in_positive_z) {
    game_state               *gs  = setup_game_state();
    emulated_input_component  bot;
    transform_component      *tc;
    int frame;

    gym_spawn_agent(gs, &bot, 0, VEC3(0,0,0), BOT_WALK_FORWARD, 0.0f);

    for (frame = 0; frame < 60; frame++)
        gym_tick(gs, &bot, 1, frame * gs->dt);

    tc = agent_transform(gs, 0);
    ASSERT(tc != NULL);
    /* After 1 second at 5 m/s: displacement.z ≈ 5 m  (vertical → +Z) */
    ASSERT_FLOAT_GT(tc->position.z, 0.1f);
    ASSERT_FLOAT_NEAR(tc->position.x, 0.0f, 0.05f);
    ASSERT_FLOAT_NEAR(tc->position.y, 0.0f, 0.05f);
}

TEST(gym_strafe_agent_moves_in_positive_x) {
    game_state               *gs  = setup_game_state();
    emulated_input_component  bot;
    transform_component      *tc;
    int frame;

    gym_spawn_agent(gs, &bot, 0, VEC3(0,0,0), BOT_STRAFE_RIGHT, 0.0f);

    for (frame = 0; frame < 60; frame++)
        gym_tick(gs, &bot, 1, frame * gs->dt);

    tc = agent_transform(gs, 0);
    ASSERT(tc != NULL);
    ASSERT_FLOAT_GT(tc->position.x, 0.1f);
    ASSERT_FLOAT_NEAR(tc->position.y, 0.0f, 0.05f);
}

TEST(gym_circle_agent_returns_near_origin_after_full_orbit) {
    game_state               *gs     = setup_game_state();
    emulated_input_component  bot;
    transform_component      *tc;
    /* One full orbit = 2π seconds; step at 60 Hz */
    int total_frames = (int)(2.0f * 3.14159265f * 60.0f) + 1;
    int frame;

    gym_spawn_agent(gs, &bot, 0, VEC3(0,0,0), BOT_CIRCLE, 0.0f);

    for (frame = 0; frame < total_frames; frame++)
        gym_tick(gs, &bot, 1, frame * gs->dt);

    tc = agent_transform(gs, 0);
    ASSERT(tc != NULL);
    /* Circular input is periodic: after 2π s the accumulated displacement
       should return near origin (within move_speed/60 ≈ 0.08 m tolerance
       per axis, scaled by orbit radius). Use a generous 1.0 m bound. */
    ASSERT_FLOAT_NEAR(tc->position.x, 0.0f, 1.0f);
    ASSERT_FLOAT_NEAR(tc->position.z, 0.0f, 1.0f);
}

TEST(gym_velocity_component_reflects_bot_input) {
    game_state               *gs  = setup_game_state();
    emulated_input_component  bot;
    velocity_component       *vc;

    gym_spawn_agent(gs, &bot, 0, VEC3(0,0,0), BOT_WALK_FORWARD, 0.0f);
    bot_update_input(&bot, 0.0f);
    bot_apply_to_character_controller(gs, &bot);  /* writes own_input */
    run_character_controller_system(gs);          /* translates input → velocity */

    vc = agent_velocity(gs, 0);
    ASSERT(vc != NULL);
    /* vertical=1.0 → velocity.z = 5.0 (move_speed) via real CC path */
    ASSERT_FLOAT_GT(vc->velocity.z, 0.0f);
    ASSERT_FLOAT_NEAR(vc->velocity.x, 0.0f, 0.001f);
    ASSERT_FLOAT_NEAR(vc->velocity.y, 0.0f, 0.001f);
}

TEST(gym_all_agents_independent_positions_after_60_frames) {
    /* Mix of behaviors; after 60 frames each agent should be at a distinct
       position, proving inputs are isolated (no shared-input pollution). */
    static const struct { bot_behavior beh; float phase; Vec3 start; } cfg[GYM_AGENT_COUNT] = {
        { BOT_IDLE,         0.0f,              {  0, 0, 0 } },
        { BOT_WALK_FORWARD, 0.0f,              {  3, 0, 0 } },
        { BOT_STRAFE_RIGHT, 0.0f,              {  6, 0, 0 } },
        { BOT_CIRCLE,       0.0f,              {  9, 0, 0 } },
        { BOT_CIRCLE,       1.5707963f,        { 12, 0, 0 } }, /* phase=π/2 */
        { BOT_WALK_FORWARD, 0.0f,              { 15, 0, 0 } },
        { BOT_STRAFE_RIGHT, 0.0f,              { 18, 0, 0 } },
        { BOT_IDLE,         0.0f,              { 21, 0, 0 } },
    };

    game_state               *gs = setup_game_state();
    emulated_input_component  bots[GYM_AGENT_COUNT];
    int i, frame;

    for (i = 0; i < GYM_AGENT_COUNT; i++)
        gym_spawn_agent(gs, &bots[i], i, cfg[i].start, cfg[i].beh, cfg[i].phase);

    for (frame = 0; frame < 60; frame++)
        gym_tick(gs, bots, GYM_AGENT_COUNT, frame * gs->dt);

    /* Idle agents must not have moved from their start X */
    ASSERT_FLOAT_NEAR(agent_transform(gs,0)->position.x, cfg[0].start.x, 0.001f);
    ASSERT_FLOAT_NEAR(agent_transform(gs,7)->position.x, cfg[7].start.x, 0.001f);

    /* Walk-forward agents should have positive z displacement */
    ASSERT_FLOAT_GT(agent_transform(gs,1)->position.z, 0.1f);
    ASSERT_FLOAT_GT(agent_transform(gs,5)->position.z, 0.1f);

    /* Strafe agents should have positive x displacement beyond their start */
    ASSERT_FLOAT_GT(agent_transform(gs,2)->position.x, cfg[2].start.x + 0.1f);
    ASSERT_FLOAT_GT(agent_transform(gs,6)->position.x, cfg[6].start.x + 0.1f);

    /* The two circle agents have different phases, so they should be at
       different (x,y) offsets from their starting positions */
    {
        float dx3 = agent_transform(gs,3)->position.x - cfg[3].start.x;
        float dz3 = agent_transform(gs,3)->position.z - cfg[3].start.z;
        float dx4 = agent_transform(gs,4)->position.x - cfg[4].start.x;
        float dz4 = agent_transform(gs,4)->position.z - cfg[4].start.z;
        /* Different phases → different displacements (can't both be (0,0)) */
        float diff = (dx3 - dx4)*(dx3 - dx4) + (dz3 - dz4)*(dz3 - dz4);
        ASSERT_FLOAT_GT(diff, 0.0f);
    }
}

TEST(gym_trigger_zone_fires_for_walk_forward_agent) {
    /* A trigger zone 4 m ahead in +Z must fire when the agent walks into it. */
    game_state               *gs  = setup_game_state();
    emulated_input_component  bot;
    trigger_component        *trig;
    int frame;
    /* Agent entity 0 walks forward (+Z); trigger entity 1 at z=4, radius=1 */
    gym_spawn_agent(gs, &bot, 0, VEC3(0,0,0), BOT_WALK_FORWARD, 0.0f);
    gym_spawn_trigger_zone(gs, 1, VEC3(0,0,4.0f), 1.0f);

    for (frame = 0; frame < 60; frame++)
        gym_tick(gs, &bot, 1, frame * gs->dt);

    trig = &gs->trigger_components[gs->trigger_index[1]];
    ASSERT(trig->activated);
    ASSERT_EQ(trig->activated_by_entity, 0);
}

TEST(gym_trigger_zone_does_not_fire_for_idle_agent) {
    /* An idle agent must never activate a nearby trigger zone. */
    game_state               *gs  = setup_game_state();
    emulated_input_component  bot;
    trigger_component        *trig;
    int frame;
    gym_spawn_agent(gs, &bot, 0, VEC3(0,0,0), BOT_IDLE, 0.0f);
    gym_spawn_trigger_zone(gs, 1, VEC3(0,0,3.0f), 1.0f);

    for (frame = 0; frame < 60; frame++)
        gym_tick(gs, &bot, 1, frame * gs->dt);

    trig = &gs->trigger_components[gs->trigger_index[1]];
    ASSERT(!trig->activated);
}

TEST(gym_trigger_zone_fires_for_correct_agent_only) {
    /* Two agents: one walks (+Z), one is idle.
       Walk trigger fires for the walking agent; idle trigger does not fire. */
    game_state                *gs = setup_game_state();
    emulated_input_component   bots[2];
    trigger_component         *trig_walk, *trig_idle;
    int frame;

    /* Entity 0: walks forward; entity 1: idle (different X so no overlap) */
    gym_spawn_agent(gs, &bots[0], 0, VEC3(0, 0, 0), BOT_WALK_FORWARD, 0.0f);
    gym_spawn_agent(gs, &bots[1], 1, VEC3(5, 0, 0), BOT_IDLE,         0.0f);
    /* Trigger 2: zone in front of walking agent */
    gym_spawn_trigger_zone(gs, 2, VEC3(0, 0, 4.0f), 1.0f);
    /* Trigger 3: zone in front of idle agent — must NOT fire */
    gym_spawn_trigger_zone(gs, 3, VEC3(5, 0, 3.0f), 1.0f);

    for (frame = 0; frame < 60; frame++)
        gym_tick(gs, bots, 2, frame * gs->dt);

    trig_walk = &gs->trigger_components[gs->trigger_index[2]];
    trig_idle = &gs->trigger_components[gs->trigger_index[3]];

    ASSERT(trig_walk->activated);
    ASSERT_EQ(trig_walk->activated_by_entity, 0); /* entity 0 walked through */
    ASSERT(!trig_idle->activated);                 /* idle entity 1 did not */
}

/* ── Entry point ────────────────────────────────────────────────────────── */

int main(void) {
    printf("\n=== Gym Scene Integration Tests ===\n\n");

    run_gym_spawn_N_agents();
    run_gym_idle_agent_does_not_move();
    run_gym_walk_forward_agent_moves_in_positive_z();
    run_gym_strafe_agent_moves_in_positive_x();
    run_gym_circle_agent_returns_near_origin_after_full_orbit();
    run_gym_velocity_component_reflects_bot_input();
    run_gym_all_agents_independent_positions_after_60_frames();
    run_gym_trigger_zone_fires_for_walk_forward_agent();
    run_gym_trigger_zone_does_not_fire_for_idle_agent();
    run_gym_trigger_zone_fires_for_correct_agent_only();

    printf("\n=== Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed) printf(", %d FAILED", tests_failed);
    printf(" ===\n\n");

    return tests_failed > 0 ? 1 : 0;
}
