#include <game.h>
#include <export.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>
#include "state_migration.gen.h"
#include "boot_profiler.h"

#define ENG_BOOT_PROF(gs, id)                do { if ((gs)->boot_prof_begin)    (gs)->boot_prof_begin(id); } while(0)
#define ENG_BOOT_PROF_REG(gs, id, level, nm) do { if ((gs)->boot_prof_register) (gs)->boot_prof_register(id, level, nm); } while(0)

/* Animation functions (from anim.c, same DLL) */
void init_pose_from_bind(Skeleton *skel, Vec3 *trans, Quat *rot, Vec3 *scale);
void sample_clip(AnimClip *clip, float time,
                 Vec3 *out_trans, Quat *out_rot, Vec3 *out_scale,
                 uint32_t joint_count);
void compute_world_transforms(Skeleton *skel,
                              Vec3 *trans, Quat *rot, Vec3 *scales,
                              Mat4 *out_world);
void compute_skinning_matrices(Skeleton *skel, Mat4 *world, Mat4 *out_skinning);

/* ECS physics update (from physics.c, same DLL) */
void collision(game_state *gs);
void apply_movement(game_state *gs);

/* Asset loading (from gltf_loader.c, same DLL) */
GltfModel load_glb(const char *path, arena *a);
void load_animations_glb(const char *path, GltfModel *model, arena *a);
void gltf_set_gpu_device(void *dev);
void gltf_set_boot_profiler(void (*begin_fn)(int));
void gltf_tex_cache_init(void);
void gltf_tex_cache_free(void);

#define SCENE_MIN_CAPACITY 32
#define MODEL_ARENA_BYTES (64 * 1024 * 1024)
#define WORLD_CHAIN_MAX 512
#define ANIMATION_TRANSITION_DEFAULT_DURATION 0.18f

/* Forward declarations for anim SM helpers (used before definition) */
static anim_instance *anim_sm_find_instance(anim_sm *sm, int entity_index);
static scene_model_asset *find_scene_model_asset(game_state *gs, int asset_index);

/* Forward declarations for system table (defined after update_engine) */
static void system_clear_draw_lists(game_state *gs);
static void system_animation_sm(game_state *gs);
static void system_mesh_sync(game_state *gs);
static void system_animation_legacy(game_state *gs);
static void system_flush_debug_lines(game_state *gs);
static void register_system(game_state *gs, const char *name,
                            system_fn fn, int play_mode_only);
static void update_triggers(game_state *gs);
static void update_bone_attachments(game_state *gs);

typedef struct camera_query_result {
    int entity_index;
    transform_component *transform;
    camera_component *camera;
} camera_query_result;

static Quat quat_from_y_deg(float degrees) {
    float half = (degrees * 3.14159265f / 180.0f) * 0.5f;
    return QUAT(0.0f, sinf(half), 0.0f, cosf(half));
}

static Vec3 normalize_scale(Vec3 s) {
    if (s.x == 0.0f) s.x = 1.0f;
    if (s.y == 0.0f) s.y = 1.0f;
    if (s.z == 0.0f) s.z = 1.0f;
    return s;
}

static transform_component *find_transform_component(game_state *gs, int entity_index) {
    int idx;
    if (!gs || entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return NULL;
    idx = gs->transform_index[entity_index];
    return idx >= 0 ? &gs->transform_components[idx] : NULL;
}

static rotation_component *find_rotation_component(game_state *gs, int entity_index) {
    int idx;
    if (!gs || entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return NULL;
    idx = gs->rotation_index[entity_index];
    return idx >= 0 ? &gs->rotation_components[idx] : NULL;
}

static scale_component *find_scale_component(game_state *gs, int entity_index) {
    int idx;
    if (!gs || entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return NULL;
    idx = gs->scale_index[entity_index];
    return idx >= 0 ? &gs->scale_components[idx] : NULL;
}

static parent_transform_component *find_parent_transform_component(game_state *gs, int entity_index) {
    int idx;
    if (!gs || entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return NULL;
    idx = gs->parent_transform_index[entity_index];
    return idx >= 0 ? &gs->parent_transform_components[idx] : NULL;
}

/* Hierarchical visibility: walks parent chain, caches per-frame */
static int is_entity_visible(game_state *gs, int entity_index) {
    int own_visible, mi;
    parent_transform_component *pt;
    if (entity_index < 0 || entity_index >= gs->scene_entity_count) return 1;
    if (gs->entity_visible[entity_index]) return gs->entity_visible[entity_index] > 0;

    /* Entities without mesh are visible by default (passthrough nodes) */
    own_visible = 1;
    mi = gs->mesh_index[entity_index];
    if (mi >= 0) own_visible = gs->mesh_components[mi].visible;

    /* AND with parent visibility (recursive, cached) */
    if (own_visible) {
        pt = find_parent_transform_component(gs, entity_index);
        if (pt && pt->parent_entity_index != entity_index) {
            own_visible = is_entity_visible(gs, pt->parent_entity_index);
        }
    }

    gs->entity_visible[entity_index] = own_visible ? 1 : -1;
    return own_visible;
}

static mesh_component *find_mesh_component(game_state *gs, int entity_index) {
    int idx;
    if (!gs || entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return NULL;
    idx = gs->mesh_index[entity_index];
    return idx >= 0 ? &gs->mesh_components[idx] : NULL;
}

static velocity_component *find_velocity_component(game_state *gs, int entity_index) {
    int idx;
    if (!gs || entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return NULL;
    idx = gs->velocity_index[entity_index];
    return idx >= 0 ? &gs->velocity_components[idx] : NULL;
}

static rigid_body_component *find_rigid_body_component(game_state *gs, int entity_index) {
    int idx;
    if (!gs || entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return NULL;
    idx = gs->rigid_body_index[entity_index];
    return idx >= 0 ? &gs->rigid_body_components[idx] : NULL;
}

static character_controller_component *find_character_controller_component(game_state *gs, int entity_index) {
    int idx;
    if (!gs || entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return NULL;
    idx = gs->character_controller_index[entity_index];
    return idx >= 0 ? &gs->character_controller_components[idx] : NULL;
}

static box_collider_component *find_box_collider_component(game_state *gs, int entity_index) {
    int idx;
    if (!gs || entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return NULL;
    idx = gs->box_collider_index[entity_index];
    return idx >= 0 ? &gs->box_collider_components[idx] : NULL;
}

static capsule_collider_component *find_capsule_collider_component(game_state *gs, int entity_index) {
    int idx;
    if (!gs || entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return NULL;
    idx = gs->capsule_collider_index[entity_index];
    return idx >= 0 ? &gs->capsule_collider_components[idx] : NULL;
}

static trigger_component *find_trigger_component(game_state *gs, int entity_index) {
    int idx;
    if (!gs || entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return NULL;
    idx = gs->trigger_index[entity_index];
    return idx >= 0 ? &gs->trigger_components[idx] : NULL;
}

static scene_model_asset *find_scene_model_asset(game_state *gs, int asset_index) {
    if (!gs || asset_index < 0 || asset_index >= gs->scene_model_asset_count) return NULL;
    return &gs->scene_model_assets[asset_index];
}

static int find_joint_by_name(Skeleton *skel, const char *name) {
    uint32_t i;
    if (!skel || !skel->joint_names || !name) return -1;
    for (i = 0; i < skel->joint_count; i++) {
        if (skel->joint_names[i] && strcmp(skel->joint_names[i], name) == 0)
            return (int)i;
    }
    return -1;
}

static animation_transition_entry *find_animation_transition_entry(game_state *gs,
                                                                   int entity_index,
                                                                   int *out_index) {
    int idx;
    if (!gs || entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return NULL;
    idx = gs->animation_transition_index[entity_index];
    if (idx < 0) return NULL;
    if (out_index) *out_index = idx;
    return &gs->animation_transition_entries[idx];
}

static animation_transition_entry *upsert_animation_transition_entry(game_state *gs, int entity_index) {
    animation_transition_entry *entry = find_animation_transition_entry(gs, entity_index, NULL);
    int i;
    if (entry) return entry;
    if (!gs) return NULL;
    if (gs->animation_transition_count >= gs->animation_transition_capacity) return NULL;
    i = gs->animation_transition_count++;
    entry = &gs->animation_transition_entries[i];
    memset(entry, 0, sizeof(*entry));
    entry->entity_index = entity_index;
    gs->animation_transition_index[entity_index] = i;
    return entry;
}

static void remove_animation_transition_entry_by_index(game_state *gs, int index) {
    int last;
    int removed_entity;
    if (!gs || index < 0 || index >= gs->animation_transition_count) return;
    removed_entity = gs->animation_transition_entries[index].entity_index;
    last = gs->animation_transition_count - 1;
    if (index != last) {
        gs->animation_transition_entries[index] = gs->animation_transition_entries[last];
        gs->animation_transition_index[gs->animation_transition_entries[index].entity_index] = index;
    }
    gs->animation_transition_index[removed_entity] = -1;
    gs->animation_transition_count = last;
}

static Mat4 local_transform_matrix(game_state *gs, int entity_index) {
    transform_component *tc;
    rotation_component *rc;
    scale_component *sc;
    Vec3 position = VEC3(0.0f, 0.0f, 0.0f);
    float rotation_y = 0.0f;
    Vec3 scale = VEC3(1.0f, 1.0f, 1.0f);
    Quat rot;
    if (!gs) return mat4_identity();

    tc = find_transform_component(gs, entity_index);
    if (tc) position = tc->position;

    rc = find_rotation_component(gs, entity_index);
    if (rc) rotation_y = rc->rotation_y_deg;

    sc = find_scale_component(gs, entity_index);
    if (sc) scale = normalize_scale(sc->scale);

    rot = quat_from_y_deg(rotation_y);
    return mat4_from_trs(position, rot, scale);
}

static Mat4 resolve_world_transform(game_state *gs, int entity_index) {
    int chain[WORLD_CHAIN_MAX];
    int chain_count = 0;
    int current = entity_index;
    int guard = 0;
    Mat4 world = mat4_identity();
    if (!gs || !gs->scene_entities) return world;

    while (guard < WORLD_CHAIN_MAX &&
           current >= 0 && current < gs->scene_entity_count) {
        parent_transform_component *pt;
        chain[chain_count++] = current;
        pt = find_parent_transform_component(gs, current);
        if (!pt) break;
        if (pt->parent_entity_index == current) break;
        current = pt->parent_entity_index;
        guard++;
    }

    while (chain_count > 0) {
        int idx = chain[--chain_count];
        world = mat4_mul(world, local_transform_matrix(gs, idx));
    }

    return world;
}

static Vec3 resolve_world_position(game_state *gs, int entity_index) {
    Vec3 world = VEC3(0.0f, 0.0f, 0.0f);
    int current = entity_index;
    int guard = 0;
    if (!gs || !gs->scene_entities) return world;

    while (guard <= gs->scene_entity_count && current >= 0 && current < gs->scene_entity_count) {
        transform_component *tc = find_transform_component(gs, current);
        parent_transform_component *pt;
        if (tc) {
            world = vec3_add(world, tc->position);
        }

        pt = find_parent_transform_component(gs, current);
        if (!pt) break;
        if (pt->parent_entity_index == current) break;
        current = pt->parent_entity_index;
        guard++;
    }
    return world;
}

static Vec3 transform_point(Mat4 m, Vec3 p) {
    return VEC3(
        m.m[0] * p.x + m.m[4] * p.y + m.m[8]  * p.z + m.m[12],
        m.m[1] * p.x + m.m[5] * p.y + m.m[9]  * p.z + m.m[13],
        m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z + m.m[14]
    );
}

static int query_active_camera(game_state *gs, camera_query_result *out_camera) {
    int i;
    if (!gs || !out_camera) return 0;

    for (i = 0; i < gs->camera_component_count; i++) {
        camera_component *cc = &gs->camera_components[i];
        int entity_index = cc->entity_index;
        transform_component *tc;

        if (entity_index < 0 || entity_index >= gs->scene_entity_count) continue;
        tc = find_transform_component(gs, entity_index);
        if (!tc) continue;

        out_camera->entity_index = entity_index;
        out_camera->transform = tc;
        out_camera->camera = cc;
        return 1;
    }

    return 0;
}

static void resolve_camera_world_pose(game_state *gs,
                                      const camera_query_result *camera_view,
                                      Vec3 *out_eye,
                                      Vec3 *out_target) {
    Vec3 world_eye = VEC3(0.0f, 0.0f, 0.0f);
    Vec3 world_target = VEC3(0.0f, 0.0f, 0.0f);
    parent_transform_component *pt;
    if (!gs || !camera_view || !camera_view->transform || !camera_view->camera) {
        if (out_eye) *out_eye = world_eye;
        if (out_target) *out_target = world_target;
        return;
    }

    pt = find_parent_transform_component(gs, camera_view->entity_index);
    if (pt && pt->parent_entity_index >= 0 &&
        pt->parent_entity_index < gs->scene_entity_count) {
        Mat4 parent_world = resolve_world_transform(gs, pt->parent_entity_index);
        world_eye = transform_point(parent_world, camera_view->transform->position);
        world_target = transform_point(parent_world, camera_view->camera->target);
        if (gs->editor_play_mode) {
            /* Third-person follow: use the opposite orbit side so camera stays behind actor. */
            Vec3 target_to_eye = vec3_sub(world_eye, world_target);
            world_eye = vec3_add(world_target,
                                 VEC3(-target_to_eye.x, target_to_eye.y, -target_to_eye.z));
        }
    } else {
        world_eye = camera_view->transform->position;
        world_target = camera_view->camera->target;
    }

    if (out_eye) *out_eye = world_eye;
    if (out_target) *out_target = world_target;
}

static mesh_component *query_primary_mesh_component(game_state *gs) {
    int i;
    if (!gs) return NULL;
    for (i = 0; i < gs->mesh_component_count; i++) {
        mesh_component *mc = &gs->mesh_components[i];
        scene_model_asset *asset;
        if (mc->entity_index < 0 || mc->entity_index >= gs->scene_entity_count) continue;
        asset = find_scene_model_asset(gs, mc->model_asset_index);
        if (asset && asset->loaded && asset->has_skeleton) return mc;
    }
    for (i = 0; i < gs->mesh_component_count; i++) {
        mesh_component *mc = &gs->mesh_components[i];
        if (mc->entity_index < 0 || mc->entity_index >= gs->scene_entity_count) continue;
        return mc;
    }
    return NULL;
}

static animation_component *query_primary_animation_component_for_mesh(game_state *gs) {
    int i;
    if (!gs) return NULL;

    for (i = 0; i < gs->animation_component_count; i++) {
        animation_component *ac = &gs->animation_components[i];
        int entity_index = ac->entity_index;
        mesh_component *mc;
        scene_model_asset *asset;
        if (entity_index < 0 || entity_index >= gs->scene_entity_count) continue;
        mc = find_mesh_component(gs, entity_index);
        if (!mc) continue;
        asset = find_scene_model_asset(gs, mc->model_asset_index);
        if (asset && asset->loaded && asset->has_skeleton) return ac;
    }

    for (i = 0; i < gs->animation_component_count; i++) {
        animation_component *ac = &gs->animation_components[i];
        int entity_index = ac->entity_index;
        mesh_component *mc;
        if (entity_index < 0 || entity_index >= gs->scene_entity_count) continue;
        mc = find_mesh_component(gs, entity_index);
        if (mc) return ac;
    }

    return NULL;
}

static void run_character_controller_system(game_state *gs) {
    const float deadzone = 0.1f;
    const float turn_speed_deg = 180.0f;
    Vec3 camera_forward = VEC3(0.0f, 0.0f, 1.0f);
    int i;
    if (!gs || !gs->editor_play_mode) return;

    {
        camera_query_result active_camera;
        if (query_active_camera(gs, &active_camera)) {
            Vec3 world_eye;
            Vec3 world_target;
            Vec3 planar_forward;
            resolve_camera_world_pose(gs, &active_camera, &world_eye, &world_target);
            planar_forward = vec3_sub(world_target, world_eye);
            planar_forward.y = 0.0f;
            if (vec3_len(planar_forward) > 1e-4f) {
                camera_forward = vec3_normalize(planar_forward);
            }
        }
    }

    for (i = 0; i < gs->character_controller_component_count; i++) {
        character_controller_component *cc = &gs->character_controller_components[i];
        transform_component *tc;
        velocity_component *vc;
        rigid_body_component *rb;
        rotation_component *rc;
        float move_speed;
        float jump_speed;
        if (cc->entity_index < 0 || cc->entity_index >= gs->scene_entity_count) continue;

        tc = find_transform_component(gs, cc->entity_index);
        vc = find_velocity_component(gs, cc->entity_index);
        if (!tc || !vc) continue;

        rb = find_rigid_body_component(gs, cc->entity_index);
        rc = find_rotation_component(gs, cc->entity_index);
        move_speed = cc->move_speed > 0.01f ? cc->move_speed : 5.0f;
        jump_speed = cc->jump_speed > 0.01f ? cc->jump_speed : 8.5f;

        {
        input_state eff = cc->use_own_input ? cc->own_input : gs->input;

        if (rb && rb->use_gravity) {
            Vec3 move_forward = camera_forward;
            Vec3 move_dir;
            if (rc && fabsf(eff.horizontal) > deadzone) {
                rc->rotation_y_deg -= eff.horizontal * turn_speed_deg * gs->dt;
                if (rc->rotation_y_deg > 180.0f) rc->rotation_y_deg -= 360.0f;
                if (rc->rotation_y_deg < -180.0f) rc->rotation_y_deg += 360.0f;
            }
            if (rc) {
                Mat4 world = resolve_world_transform(gs, cc->entity_index);
                Vec3 world_forward = VEC3(world.m[8], 0.0f, world.m[10]);
                if (vec3_len(world_forward) > 1e-4f) {
                    move_forward = vec3_normalize(world_forward);
                }
            }
            move_dir = vec3_scale(move_forward, eff.vertical);
            vc->velocity.x = move_dir.x * move_speed;
            vc->velocity.z = move_dir.z * move_speed;
            if ((eff.input_mask & INPUT_A) && fabsf(vc->velocity.y) < 0.001f) {
                vc->velocity.y = jump_speed;
            }
        } else {
            /* use_gravity=false: horizontal → X (strafe), vertical → Z (forward) */
            vc->velocity.x = eff.horizontal * move_speed;
            vc->velocity.z = eff.vertical   * move_speed;
            if (rc) {
                float move_h = eff.horizontal;
                float move_v = eff.vertical;
                if (fabsf(move_h) <= deadzone && fabsf(move_v) <= deadzone) continue;
                {
                    float yaw_rad = atan2f(move_h, move_v);
                    rc->rotation_y_deg = -yaw_rad * (180.0f / 3.14159265f);
                }
            }
        }
        }
    }
}

static void sync_mesh_camera_from_components(game_state *gs) {
    camera_query_result camera_view;
    Vec3 desired_eye;
    Vec3 desired_target;
    if (!query_active_camera(gs, &camera_view)) return;
    resolve_camera_world_pose(gs, &camera_view, &desired_eye, &desired_target);

    if (!gs->editor_play_mode) {
        gs->mesh3d.camera_eye = desired_eye;
        gs->mesh3d.camera_target = desired_target;
    } else {
        const float follow_speed = 10.0f;
        const float snap_distance = 12.0f;
        float alpha;
        float eye_delta;
        float target_delta;

        if (gs->dt <= 0.0f || gs->dt > 0.25f) {
            alpha = 1.0f;
        } else {
            alpha = 1.0f - expf(-follow_speed * gs->dt);
            if (alpha < 0.0f) alpha = 0.0f;
            if (alpha > 1.0f) alpha = 1.0f;
        }

        eye_delta = vec3_len(vec3_sub(desired_eye, gs->mesh3d.camera_eye));
        target_delta = vec3_len(vec3_sub(desired_target, gs->mesh3d.camera_target));
        if (eye_delta > snap_distance || target_delta > snap_distance) {
            alpha = 1.0f;
        }

        gs->mesh3d.camera_eye = vec3_lerp(gs->mesh3d.camera_eye, desired_eye, alpha);
        gs->mesh3d.camera_target = vec3_lerp(gs->mesh3d.camera_target, desired_target, alpha);
    }

    gs->mesh3d.camera_up = camera_view.camera->up;
    gs->mesh3d.camera_fov_deg = camera_view.camera->fov_deg;
    gs->mesh3d.camera_near = camera_view.camera->near_plane;
    gs->mesh3d.camera_far = camera_view.camera->far_plane;
}

static int resolve_project_model_path(const project_data *project,
                                      const char *key,
                                      const char **out_path) {
    const char *path;
    if (!project || !key || !key[0] || !out_path) return 0;

    path = project_find_asset(project, key, ASSET_MODEL);
    if (path) { *out_path = path; return 1; }

    path = project_find_asset(project, key, ASSET_DUNGEON_PIECE);
    if (path) { *out_path = path; return 1; }

    if (strstr(key, ".glb") || strstr(key, ".gltf") || strchr(key, '/')) {
        *out_path = key;
        return 1;
    }

    return 0;
}

static int resolve_project_animation_path(const project_data *project,
                                          const char *key,
                                          const char **out_path) {
    const char *path;
    if (!project || !key || !key[0] || !out_path) return 0;

    path = project_find_asset(project, key, ASSET_ANIMATION);
    if (path) { *out_path = path; return 1; }

    if (strstr(key, ".glb") || strstr(key, ".gltf") || strchr(key, '/')) {
        *out_path = key;
        return 1;
    }

    return 0;
}

static int register_scene_model_asset(game_state *gs,
                                      const char *key,
                                      const char *path,
                                      const char *animation_path) {
    int i;
    scene_model_asset *asset;
    if (!gs || !path || !path[0]) return -1;

    for (i = 0; i < gs->scene_model_asset_count; i++) {
        asset = &gs->scene_model_assets[i];
        if (strcmp(asset->path, path) == 0) {
            if (animation_path && animation_path[0] && !asset->has_animation_path) {
                snprintf(asset->animation_path, sizeof(asset->animation_path), "%s", animation_path);
                asset->has_animation_path = 1;
            }
            return i;
        }
    }

    if (gs->scene_model_asset_count >= SCENE_MODEL_ASSET_MAX) {
        fprintf(stderr, "Warning: scene model asset limit (%d) reached\n", SCENE_MODEL_ASSET_MAX);
        return -1;
    }

    i = gs->scene_model_asset_count++;
    asset = &gs->scene_model_assets[i];
    memset(asset, 0, sizeof(*asset));
    if (key && key[0]) {
        snprintf(asset->key, sizeof(asset->key), "%s", key);
    } else {
        snprintf(asset->key, sizeof(asset->key), "asset_%d", i);
    }
    snprintf(asset->path, sizeof(asset->path), "%s", path);
    if (animation_path && animation_path[0]) {
        snprintf(asset->animation_path, sizeof(asset->animation_path), "%s", animation_path);
        asset->has_animation_path = 1;
    }
    return i;
}

/* Register all project browser 3D assets so thumbnails can resolve by key. */
static void register_project_model_assets(game_state *gs) {
    const project_data *project;
    int i;
    if (!gs || !gs->project_loaded) return;

    project = &gs->project;
    for (i = 0; i < project->asset_count; i++) {
        if (project->assets[i].type == ASSET_MODEL ||
            project->assets[i].type == ASSET_DUNGEON_PIECE) {
            register_scene_model_asset(gs, project->assets[i].key, project->assets[i].path, NULL);
        }
    }
}

static rect collider_rect_from_half_extents(const float half_extents[3], float fallback_half_extent) {
    float hx = fallback_half_extent;
    float hz = fallback_half_extent;
    rect r;
    if (half_extents) {
        if (half_extents[0] > 0.0f) hx = half_extents[0];
        if (half_extents[2] > 0.0f) hz = half_extents[2];
        else if (half_extents[1] > 0.0f) hz = half_extents[1];
    }
    r.x = 0.0f;
    r.y = 0.0f;
    r.w = hx * 2.0f;
    r.h = hz * 2.0f;
    return r;
}


static error_value reserve_array(arena *a, void **out_ptr, int *capacity, int needed,
                                  size_t elem_size, const char *tag) {
    int old_capacity;
    int new_capacity;
    void *new_ptr;
    void *ptr;
    size_t copy_count;
    if (!a || !capacity || !out_ptr) ERRV_RETURN_ERR(1, tag);
    ptr = *out_ptr;
    old_capacity = *capacity;
    if (ptr && old_capacity >= needed) ERRV_RETURN_OK();

    new_capacity = old_capacity > 0 ? old_capacity : SCENE_MIN_CAPACITY;
    while (new_capacity < needed) new_capacity *= 2;

    new_ptr = arena_alloc(a, (uint32_t)(new_capacity * (int)elem_size), 16, tag);
    if (!new_ptr) ERRV_RETURN_ERR(1, tag);

    if (ptr && old_capacity > 0) {
        copy_count = (size_t)old_capacity * elem_size;
        memcpy(new_ptr, ptr, copy_count);
    }
    *capacity = new_capacity;
    *out_ptr = new_ptr;
    ERRV_RETURN_OK();
}

static error_value ensure_scene_storage(game_state *gs, int needed_entities) {
    error_value err;
    if (!gs || !gs->gameplay) ERRV_RETURN_ERR(1, "ensure_scene_storage: null gs/gameplay");
    if (needed_entities < SCENE_MIN_CAPACITY) needed_entities = SCENE_MIN_CAPACITY;

    err = reserve_array(gs->gameplay, (void **)&gs->scene_entities,
        &gs->scene_entity_capacity, needed_entities, sizeof(entity), "entities");
    if (!ERRV_IS_OK(err)) return err;

    err = reserve_array(gs->gameplay, (void **)&gs->parent_components,
        &gs->parent_component_capacity, needed_entities, sizeof(parent_component), "parent_components");
    if (!ERRV_IS_OK(err)) return err;

    err = reserve_array(gs->gameplay, (void **)&gs->parent_transform_components,
        &gs->parent_transform_component_capacity, needed_entities,
        sizeof(parent_transform_component), "parent_transform_components");
    if (!ERRV_IS_OK(err)) return err;

    err = reserve_array(gs->gameplay, (void **)&gs->parent_rotation_components,
        &gs->parent_rotation_component_capacity, needed_entities,
        sizeof(parent_rotation_component), "parent_rotation_components");
    if (!ERRV_IS_OK(err)) return err;

    err = reserve_array(gs->gameplay, (void **)&gs->transform_components,
        &gs->transform_component_capacity, needed_entities,
        sizeof(transform_component), "transform_components");
    if (!ERRV_IS_OK(err)) return err;

    err = reserve_array(gs->gameplay, (void **)&gs->rotation_components,
        &gs->rotation_component_capacity, needed_entities,
        sizeof(rotation_component), "rotation_components");
    if (!ERRV_IS_OK(err)) return err;

    err = reserve_array(gs->gameplay, (void **)&gs->scale_components,
        &gs->scale_component_capacity, needed_entities,
        sizeof(scale_component), "scale_components");
    if (!ERRV_IS_OK(err)) return err;

    err = reserve_array(gs->gameplay, (void **)&gs->velocity_components,
        &gs->velocity_component_capacity, needed_entities,
        sizeof(velocity_component), "velocity_components");
    if (!ERRV_IS_OK(err)) return err;

    err = reserve_array(gs->gameplay, (void **)&gs->rigid_body_components,
        &gs->rigid_body_component_capacity, needed_entities,
        sizeof(rigid_body_component), "rigid_body_components");
    if (!ERRV_IS_OK(err)) return err;

    err = reserve_array(gs->gameplay, (void **)&gs->character_controller_components,
        &gs->character_controller_component_capacity, needed_entities,
        sizeof(character_controller_component), "character_controller_components");
    if (!ERRV_IS_OK(err)) return err;

    err = reserve_array(gs->gameplay, (void **)&gs->health_components,
        &gs->health_component_capacity, needed_entities,
        sizeof(health_component), "health_components");
    if (!ERRV_IS_OK(err)) return err;

    err = reserve_array(gs->gameplay, (void **)&gs->box_collider_components,
        &gs->box_collider_component_capacity, needed_entities,
        sizeof(box_collider_component), "box_collider_components");
    if (!ERRV_IS_OK(err)) return err;

    err = reserve_array(gs->gameplay, (void **)&gs->capsule_collider_components,
        &gs->capsule_collider_component_capacity, needed_entities,
        sizeof(capsule_collider_component), "capsule_collider_components");
    if (!ERRV_IS_OK(err)) return err;

    err = reserve_array(gs->gameplay, (void **)&gs->mesh_components,
        &gs->mesh_component_capacity, needed_entities,
        sizeof(mesh_component), "mesh_components");
    if (!ERRV_IS_OK(err)) return err;

    err = reserve_array(gs->gameplay, (void **)&gs->animation_components,
        &gs->animation_component_capacity, needed_entities,
        sizeof(animation_component), "animation_components");
    if (!ERRV_IS_OK(err)) return err;

    err = reserve_array(gs->gameplay, (void **)&gs->animation_transition_entries,
        &gs->animation_transition_capacity, needed_entities,
        sizeof(animation_transition_entry), "animation_transition_entries");
    if (!ERRV_IS_OK(err)) return err;

    err = reserve_array(gs->gameplay, (void **)&gs->camera_components,
        &gs->camera_component_capacity, needed_entities,
        sizeof(camera_component), "camera_components");
    if (!ERRV_IS_OK(err)) return err;

    err = reserve_array(gs->gameplay, (void **)&gs->trigger_components,
        &gs->trigger_component_capacity, needed_entities,
        sizeof(trigger_component), "trigger_components");
    if (!ERRV_IS_OK(err)) return err;

    err = reserve_array(gs->gameplay, (void **)&gs->bot_components,
        &gs->bot_component_capacity, needed_entities,
        sizeof(bot_component), "bot_components");
    if (!ERRV_IS_OK(err)) return err;

    err = reserve_array(gs->gameplay, (void **)&gs->bone_attach_components,
        &gs->bone_attach_component_capacity, needed_entities,
        sizeof(bone_attach_component), "bone_attach_components");
    if (!ERRV_IS_OK(err)) return err;

    ERRV_RETURN_OK();
}

static void clear_scene_storage(game_state *gs) {
    if (!gs) return;
    if (gs->scene_entities && gs->scene_entity_capacity > 0) {
        memset(gs->scene_entities, 0, (size_t)gs->scene_entity_capacity * sizeof(entity));
    }
    if (gs->parent_components && gs->parent_component_capacity > 0) {
        memset(gs->parent_components, 0, (size_t)gs->parent_component_capacity * sizeof(parent_component));
    }
    if (gs->parent_transform_components && gs->parent_transform_component_capacity > 0) {
        memset(gs->parent_transform_components, 0,
               (size_t)gs->parent_transform_component_capacity * sizeof(parent_transform_component));
    }
    if (gs->parent_rotation_components && gs->parent_rotation_component_capacity > 0) {
        memset(gs->parent_rotation_components, 0,
               (size_t)gs->parent_rotation_component_capacity * sizeof(parent_rotation_component));
    }
    if (gs->transform_components && gs->transform_component_capacity > 0) {
        memset(gs->transform_components, 0, (size_t)gs->transform_component_capacity * sizeof(transform_component));
    }
    if (gs->rotation_components && gs->rotation_component_capacity > 0) {
        memset(gs->rotation_components, 0, (size_t)gs->rotation_component_capacity * sizeof(rotation_component));
    }
    if (gs->scale_components && gs->scale_component_capacity > 0) {
        memset(gs->scale_components, 0, (size_t)gs->scale_component_capacity * sizeof(scale_component));
    }
    if (gs->velocity_components && gs->velocity_component_capacity > 0) {
        memset(gs->velocity_components, 0, (size_t)gs->velocity_component_capacity * sizeof(velocity_component));
    }
    if (gs->rigid_body_components && gs->rigid_body_component_capacity > 0) {
        memset(gs->rigid_body_components, 0,
               (size_t)gs->rigid_body_component_capacity * sizeof(rigid_body_component));
    }
    if (gs->character_controller_components && gs->character_controller_component_capacity > 0) {
        memset(gs->character_controller_components, 0,
               (size_t)gs->character_controller_component_capacity * sizeof(character_controller_component));
    }
    if (gs->health_components && gs->health_component_capacity > 0) {
        memset(gs->health_components, 0, (size_t)gs->health_component_capacity * sizeof(health_component));
    }
    if (gs->box_collider_components && gs->box_collider_component_capacity > 0) {
        memset(gs->box_collider_components, 0,
               (size_t)gs->box_collider_component_capacity * sizeof(box_collider_component));
    }
    if (gs->capsule_collider_components && gs->capsule_collider_component_capacity > 0) {
        memset(gs->capsule_collider_components, 0,
               (size_t)gs->capsule_collider_component_capacity * sizeof(capsule_collider_component));
    }
    if (gs->mesh_components && gs->mesh_component_capacity > 0) {
        memset(gs->mesh_components, 0, (size_t)gs->mesh_component_capacity * sizeof(mesh_component));
    }
    if (gs->animation_components && gs->animation_component_capacity > 0) {
        memset(gs->animation_components, 0,
               (size_t)gs->animation_component_capacity * sizeof(animation_component));
    }
    if (gs->animation_transition_entries && gs->animation_transition_capacity > 0) {
        memset(gs->animation_transition_entries, 0,
               (size_t)gs->animation_transition_capacity * sizeof(animation_transition_entry));
    }
    if (gs->camera_components && gs->camera_component_capacity > 0) {
        memset(gs->camera_components, 0, (size_t)gs->camera_component_capacity * sizeof(camera_component));
    }
    if (gs->trigger_components && gs->trigger_component_capacity > 0) {
        memset(gs->trigger_components, 0, (size_t)gs->trigger_component_capacity * sizeof(trigger_component));
    }
    if (gs->bone_attach_components && gs->bone_attach_component_capacity > 0) {
        memset(gs->bone_attach_components, 0,
               (size_t)gs->bone_attach_component_capacity * sizeof(bone_attach_component));
    }

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
    memset(gs->bot_index, 0xFF, sizeof(gs->bot_index));
    memset(gs->bone_attach_index, 0xFF, sizeof(gs->bone_attach_index));

    gs->scene_entity_count = 0;
    gs->parent_component_count = 0;
    gs->parent_transform_component_count = 0;
    gs->parent_rotation_component_count = 0;
    gs->transform_component_count = 0;
    gs->rotation_component_count = 0;
    gs->scale_component_count = 0;
    gs->velocity_component_count = 0;
    gs->rigid_body_component_count = 0;
    gs->character_controller_component_count = 0;
    gs->health_component_count = 0;
    gs->box_collider_component_count = 0;
    gs->capsule_collider_component_count = 0;
    gs->mesh_component_count = 0;
    gs->animation_component_count = 0;
    gs->animation_transition_count = 0;
    gs->camera_component_count = 0;
    gs->trigger_component_count = 0;
    gs->bot_component_count = 0;
    gs->bone_attach_component_count = 0;
    /* Keep registered model assets so scene resets can reuse loaded meshes/animations. */
    gs->scene_primary_skinned_entity = -1;
    gs->scene_camera_entity = -1;
}

static void push_transform_component(game_state *gs, int entity_index, Vec3 position) {
    int i;
    if (!gs) return;
    if (entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return;
    if (gs->transform_component_count >= gs->transform_component_capacity) return;
    i = gs->transform_component_count++;
    gs->transform_components[i].entity_index = entity_index;
    gs->transform_components[i].position = position;
    gs->transform_index[entity_index] = i;
}

static void push_rotation_component(game_state *gs, int entity_index, float rotation_y_deg) {
    int i;
    if (!gs) return;
    if (entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return;
    if (gs->rotation_component_count >= gs->rotation_component_capacity) return;
    i = gs->rotation_component_count++;
    gs->rotation_components[i].entity_index = entity_index;
    gs->rotation_components[i].rotation_y_deg = rotation_y_deg;
    gs->rotation_index[entity_index] = i;
}

static void push_scale_component(game_state *gs, int entity_index, Vec3 scale) {
    int i;
    if (!gs) return;
    if (entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return;
    if (gs->scale_component_count >= gs->scale_component_capacity) return;
    i = gs->scale_component_count++;
    gs->scale_components[i].entity_index = entity_index;
    gs->scale_components[i].scale = normalize_scale(scale);
    gs->scale_index[entity_index] = i;
}

static void push_parent_transform_component(game_state *gs, int entity_index, int parent_entity_index) {
    int i;
    if (!gs) return;
    if (entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return;
    if (gs->parent_transform_component_count >= gs->parent_transform_component_capacity) return;
    i = gs->parent_transform_component_count++;
    gs->parent_transform_components[i].entity_index = entity_index;
    gs->parent_transform_components[i].parent_entity_index = parent_entity_index;
    gs->parent_transform_index[entity_index] = i;
}

static void push_parent_rotation_component(game_state *gs, int entity_index) {
    int i;
    if (!gs) return;
    if (entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return;
    if (gs->parent_rotation_component_count >= gs->parent_rotation_component_capacity) return;
    i = gs->parent_rotation_component_count++;
    gs->parent_rotation_components[i].entity_index = entity_index;
    gs->parent_rotation_index[entity_index] = i;
}

static void push_velocity_component(game_state *gs, int entity_index, Vec3 velocity) {
    int i;
    if (!gs) return;
    if (entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return;
    if (gs->velocity_component_count >= gs->velocity_component_capacity) return;
    i = gs->velocity_component_count++;
    gs->velocity_components[i].entity_index = entity_index;
    gs->velocity_components[i].velocity = velocity;
    gs->velocity_index[entity_index] = i;
}

static void push_rigid_body_component(game_state *gs, int entity_index, int use_gravity) {
    int i;
    if (!gs) return;
    if (entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return;
    if (gs->rigid_body_component_count >= gs->rigid_body_component_capacity) return;
    i = gs->rigid_body_component_count++;
    gs->rigid_body_components[i].entity_index = entity_index;
    gs->rigid_body_components[i].use_gravity = use_gravity ? 1 : 0;
    gs->rigid_body_index[entity_index] = i;
}

static void push_character_controller_component(game_state *gs, int entity_index,
                                                float move_speed, float jump_speed) {
    int idx;
    int i;
    if (!gs) return;
    idx = (entity_index >= 0 && entity_index < PROJECT_COMP_MAX)
        ? gs->character_controller_index[entity_index] : -1;
    if (idx >= 0) {
        gs->character_controller_components[idx].move_speed = move_speed > 0.01f ? move_speed : 5.0f;
        gs->character_controller_components[idx].jump_speed = jump_speed > 0.01f ? jump_speed : 8.5f;
        return;
    }
    if (entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return;
    if (gs->character_controller_component_count >= gs->character_controller_component_capacity) return;
    i = gs->character_controller_component_count++;
    gs->character_controller_components[i].entity_index = entity_index;
    gs->character_controller_components[i].move_speed = move_speed > 0.01f ? move_speed : 5.0f;
    gs->character_controller_components[i].jump_speed = jump_speed > 0.01f ? jump_speed : 8.5f;
    gs->character_controller_index[entity_index] = i;
}

static void push_health_component(game_state *gs, int entity_index, float health, float max_health) {
    int i;
    if (!gs) return;
    if (entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return;
    if (gs->health_component_count >= gs->health_component_capacity) return;
    i = gs->health_component_count++;
    gs->health_components[i].entity_index = entity_index;
    gs->health_components[i].health = health;
    gs->health_components[i].max_health = max_health;
    gs->health_index[entity_index] = i;
}

static void push_box_collider_component(game_state *gs, int entity_index, rect box, float half_height) {
    int i;
    if (!gs) return;
    if (entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return;
    if (gs->box_collider_component_count >= gs->box_collider_component_capacity) return;
    i = gs->box_collider_component_count++;
    gs->box_collider_components[i].entity_index = entity_index;
    gs->box_collider_components[i].rect = box;
    gs->box_collider_components[i].half_height = half_height;
    gs->box_collider_index[entity_index] = i;
}

static void push_capsule_collider_component(game_state *gs, int entity_index,
                                            float radius, float half_height) {
    int i;
    if (!gs) return;
    if (entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return;
    if (gs->capsule_collider_component_count >= gs->capsule_collider_component_capacity) return;
    i = gs->capsule_collider_component_count++;
    gs->capsule_collider_components[i].entity_index = entity_index;
    gs->capsule_collider_components[i].radius = radius > 0.0f ? radius : 0.5f;
    gs->capsule_collider_components[i].half_height = half_height > 0.0f ? half_height : gs->capsule_collider_components[i].radius;
    gs->capsule_collider_index[entity_index] = i;
}

static void push_mesh_component(game_state *gs, int entity_index, int asset_index) {
    int i;
    if (!gs) return;
    if (entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return;
    if (gs->mesh_component_count >= gs->mesh_component_capacity) return;
    i = gs->mesh_component_count++;
    gs->mesh_components[i].entity_index = entity_index;
    gs->mesh_components[i].visible = 1;
    gs->mesh_components[i].model_asset_index = asset_index;
    gs->mesh_index[entity_index] = i;
}

static void push_animation_component(game_state *gs, int entity_index, int active_clip) {
    int i;
    if (!gs) return;
    if (entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return;
    if (gs->animation_component_count >= gs->animation_component_capacity) return;
    i = gs->animation_component_count++;
    gs->animation_components[i].entity_index = entity_index;
    gs->animation_components[i].playing = 1;
    gs->animation_components[i].active_clip = active_clip;
    gs->animation_components[i].anim_time = 0.0f;
    gs->animation_components[i].speed = 1.0f;
    gs->animation_index[entity_index] = i;
}

static void push_camera_component(game_state *gs, int entity_index, float fov, float near_plane, float far_plane,
                                  Vec3 target, Vec3 up) {
    int i;
    if (!gs) return;
    if (entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return;
    if (gs->camera_component_count >= gs->camera_component_capacity) return;
    i = gs->camera_component_count++;
    gs->camera_components[i].entity_index = entity_index;
    gs->camera_components[i].fov_deg = fov;
    gs->camera_components[i].near_plane = near_plane;
    gs->camera_components[i].far_plane = far_plane;
    gs->camera_components[i].target = target;
    gs->camera_components[i].up = up;
    gs->camera_index[entity_index] = i;
}

static void push_trigger_component(game_state *gs, int entity_index,
                                   trigger_type type, int target_entity,
                                   float radius, const char *joint_name) {
    int i;
    if (!gs) return;
    if (entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return;
    if (gs->trigger_component_count >= gs->trigger_component_capacity) return;
    i = gs->trigger_component_count++;
    gs->trigger_components[i].entity_index = entity_index;
    gs->trigger_components[i].type = type;
    gs->trigger_components[i].target_entity = target_entity;
    gs->trigger_components[i].radius = radius > 0.0f ? radius : 1.0f;
    gs->trigger_components[i].activated = 0;
    gs->trigger_components[i].joint_name[0] = '\0';
    if (joint_name && joint_name[0])
        strncpy(gs->trigger_components[i].joint_name, joint_name,
                sizeof(gs->trigger_components[i].joint_name) - 1);
    gs->trigger_index[entity_index] = i;
}

static void push_bot_component(game_state *gs, int entity_index,
                               int behavior, float phase) {
    int i;
    if (!gs) return;
    if (gs->bot_component_count >= gs->bot_component_capacity) return;
    if (entity_index < 0 || entity_index >= gs->scene_entity_count) return;
    i = gs->bot_component_count++;
    gs->bot_components[i].entity_index = entity_index;
    gs->bot_components[i].behavior     = behavior;
    gs->bot_components[i].phase        = phase;
    gs->bot_index[entity_index]        = i;
}

static void run_bot_system(game_state *gs) {
    int i;
    float t;
    if (!gs || !gs->editor_play_mode) return;
    t = gs->elapsed_time;
    for (i = 0; i < gs->bot_component_count; i++) {
        bot_component *bot = &gs->bot_components[i];
        int idx = gs->character_controller_index[bot->entity_index];
        character_controller_component *cc;
        input_state inp;
        if (idx < 0) continue;
        cc = &gs->character_controller_components[idx];
        memset(&inp, 0, sizeof(inp));
        switch (bot->behavior) {
        case BOT_BEHAVIOR_WALK_FORWARD:
            inp.vertical = 1.0f;
            break;
        case BOT_BEHAVIOR_CIRCLE:
            inp.horizontal = cosf(t + bot->phase);
            inp.vertical   = sinf(t + bot->phase);
            break;
        case BOT_BEHAVIOR_PATROL:
            inp.vertical = sinf(t * 0.5f + bot->phase) > 0.0f ? 1.0f : -1.0f;
            break;
        default: /* BOT_BEHAVIOR_IDLE */ break;
        }
        cc->own_input    = inp;
        cc->use_own_input = 1;
    }
}

static void push_bone_attach_component(game_state *gs, int entity_index,
                                        int target_entity, int joint_index,
                                        Vec3 offset_pos, Quat offset_rot) {
    int i;
    if (!gs) return;
    if (entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return;
    i = gs->bone_attach_component_count;
    if (i >= gs->bone_attach_component_capacity) return;
    gs->bone_attach_components[i].entity_index = entity_index;
    gs->bone_attach_components[i].target_entity = target_entity;
    gs->bone_attach_components[i].joint_index = joint_index;
    gs->bone_attach_components[i].offset_pos = offset_pos;
    gs->bone_attach_components[i].offset_rot = offset_rot;
    gs->bone_attach_index[entity_index] = i;
    gs->bone_attach_component_count++;
}

static void update_bone_attachments(game_state *gs) {
    int i;
    if (!gs || gs->bone_attach_component_count <= 0) return;

    for (i = 0; i < gs->bone_attach_component_count; i++) {
        bone_attach_component *ba = &gs->bone_attach_components[i];
        anim_instance *inst;
        mesh_component *mc;
        scene_model_asset *asset;
        transform_component *tc;
        parent_transform_component *pt;
        Mat4 skin_mat, inv_bind, joint_model, entity_world, joint_world, offset_mat, final_mat;

        mc = find_mesh_component(gs, ba->target_entity);
        if (!mc) continue;
        asset = find_scene_model_asset(gs, mc->model_asset_index);
        if (!asset || !asset->loaded || !asset->has_skeleton) continue;
        if (ba->joint_index < 0 || (uint32_t)ba->joint_index >= asset->model.skeleton.joint_count) continue;

        inst = anim_sm_find_instance(&gs->anim, ba->target_entity);
        if (!inst) continue;

        skin_mat = gs->anim.pool.skin_mats[inst->skin_mats_offset + ba->joint_index];
        inv_bind = mat4_affine_inverse(asset->model.skeleton.inverse_bind[ba->joint_index]);
        joint_model = mat4_mul(skin_mat, inv_bind);

        entity_world = resolve_world_transform(gs, ba->target_entity);
        joint_world = mat4_mul(mat4_mul(entity_world, asset->model.armature_transform), joint_model);

        offset_mat = mat4_from_trs(ba->offset_pos, ba->offset_rot, VEC3(1.0f, 1.0f, 1.0f));
        final_mat = mat4_mul(joint_world, offset_mat);

        tc = find_transform_component(gs, ba->entity_index);
        if (tc) {
            tc->position.x = final_mat.m[12];
            tc->position.y = final_mat.m[13];
            tc->position.z = final_mat.m[14];
        }

        /* Clear parent_transform so resolve_world_transform doesn't double-add */
        pt = find_parent_transform_component(gs, ba->entity_index);
        if (pt) {
            pt->parent_entity_index = -1;
        }
    }
}

static int scene_name_has_player_token(const char *name) {
    const char token[] = "player";
    size_t token_len = sizeof(token) - 1;
    size_t i;
    if (!name || !name[0]) return 0;

    for (i = 0; name[i]; i++) {
        size_t j;
        for (j = 0; j < token_len; j++) {
            char c = name[i + j];
            if (!c) return 0;
            if ((char)tolower((unsigned char)c) != token[j]) break;
        }
        if (j == token_len) return 1;
    }
    return 0;
}

static int entity_is_controller_candidate(game_state *gs, int entity_index) {
    if (!gs) return 0;
    if (entity_index < 0 || entity_index >= gs->scene_entity_count) return 0;
    if (!find_transform_component(gs, entity_index)) return 0;
    if (!find_velocity_component(gs, entity_index)) return 0;
    return 1;
}

static void ensure_default_character_controller(game_state *gs) {
    int i;
    int player_named_candidate = -1;
    int fallback_candidate = -1;
    int chosen = -1;
    if (!gs) return;
    if (gs->character_controller_component_count > 0) return;

    /* First pass: look for player-named entity with transform (velocity optional) */
    for (i = 0; i < gs->scene_entity_count; i++) {
        const char *name = NULL;
        if (!find_transform_component(gs, i)) continue;

        if (i >= 0 && i < gs->project.scene_entity_count && gs->project.scene_entity_names[i][0]) {
            name = gs->project.scene_entity_names[i];
        }
        if (name && scene_name_has_player_token(name)) {
            player_named_candidate = i;
            break;
        }
        if (fallback_candidate < 0 && find_velocity_component(gs, i))
            fallback_candidate = i;
    }

    chosen = (player_named_candidate >= 0) ? player_named_candidate : fallback_candidate;
    if (chosen < 0) return;

    /* Ensure velocity component exists on the chosen entity */
    if (!find_velocity_component(gs, chosen)) {
        push_velocity_component(gs, chosen, VEC3(0.0f, 0.0f, 0.0f));
    }
    push_character_controller_component(gs, chosen, 5.0f, 8.5f);
}

static void build_fallback_scene(game_state *gs) {
    const char *model_path = NULL;
    const char *anim_path = NULL;
    int player_asset = -1;
    int entity_index = 0;
    int camera_index;

    if (!gs) return;

    resolve_project_model_path(&gs->project, "knight", &model_path);
    if (!model_path) model_path = gs->default_model_path;
    resolve_project_animation_path(&gs->project, "general", &anim_path);
    if (!anim_path) anim_path = gs->default_animation_path;

    player_asset = register_scene_model_asset(gs, "player", model_path, anim_path);

    push_transform_component(gs, entity_index, VEC3(0.0f, 0.0f, 0.0f));
    push_velocity_component(gs, entity_index, VEC3(0.0f, 0.0f, 0.0f));
    push_rigid_body_component(gs, entity_index, 1);
    push_character_controller_component(gs, entity_index, 5.0f, 8.5f);
    push_health_component(gs, entity_index, 100.0f, 100.0f);
    push_capsule_collider_component(gs, entity_index, 0.4f, 0.4f);
    push_mesh_component(gs, entity_index, player_asset);
    push_animation_component(gs, entity_index, 0);
    gs->scene_primary_skinned_entity = entity_index;
    entity_index++;

    camera_index = entity_index;
    push_transform_component(gs, camera_index, VEC3(0.0f, 1.0f, 3.0f));
    push_camera_component(gs, camera_index, 60.0f, 0.1f, 100.0f,
                          VEC3(0.0f, 0.5f, 0.0f), VEC3(0.0f, 1.0f, 0.0f));
    gs->scene_camera_entity = camera_index;
    entity_index++;

    gs->scene_entity_count = entity_index;
}

static void build_scene_from_project(game_state *gs) {
    const project_data *project;
    int i;
    if (!gs) return;

    project = &gs->project;
    register_project_model_assets(gs);
    if (project->scene_entity_count <= 0) {
        build_fallback_scene(gs);
        return;
    }

    gs->scene_entity_count = project->scene_entity_count;
    if (gs->scene_entity_count > gs->scene_entity_capacity) {
        gs->scene_entity_count = gs->scene_entity_capacity;
    }

    /* Per-component table iteration */
    for (i = 0; i < project->transform_count; i++) {
        const project_transform *t = &project->transforms[i];
        if (t->entity >= gs->scene_entity_count) continue;
        push_transform_component(gs, t->entity,
                                 VEC3(t->position[0], t->position[1], t->position[2]));
    }

    for (i = 0; i < project->rotation_count; i++) {
        const project_rotation *r = &project->rotations[i];
        if (r->entity >= gs->scene_entity_count) continue;
        push_rotation_component(gs, r->entity, r->y);
    }

    for (i = 0; i < project->scale_count; i++) {
        const project_scale *s = &project->scales[i];
        if (s->entity >= gs->scene_entity_count) continue;
        push_scale_component(gs, s->entity,
                             VEC3(s->value[0], s->value[1], s->value[2]));
    }

    for (i = 0; i < project->parent_transform_count; i++) {
        const project_parent_transform *pt = &project->parent_transforms[i];
        if (pt->entity >= gs->scene_entity_count) continue;
        push_parent_transform_component(gs, pt->entity, pt->parent);
    }

    for (i = 0; i < project->parent_rotation_count; i++) {
        const project_parent_rotation *pr = &project->parent_rotations[i];
        if (pr->entity >= gs->scene_entity_count) continue;
        push_parent_rotation_component(gs, pr->entity);
    }

    for (i = 0; i < project->velocity_count; i++) {
        const project_velocity *v = &project->velocities[i];
        if (v->entity >= gs->scene_entity_count) continue;
        push_velocity_component(gs, v->entity, VEC3(v->value[0], v->value[1], 0.0f));
    }

    for (i = 0; i < project->rigid_body_count; i++) {
        const project_rigid_body *rb = &project->rigid_bodies[i];
        if (rb->entity >= gs->scene_entity_count) continue;
        push_rigid_body_component(gs, rb->entity, rb->use_gravity);
    }

    for (i = 0; i < project->character_controller_count; i++) {
        const project_character_controller *cc = &project->character_controllers[i];
        if (cc->entity >= gs->scene_entity_count) continue;
        push_character_controller_component(gs, cc->entity, cc->move_speed, cc->jump_speed);
    }

    for (i = 0; i < project->health_count; i++) {
        const project_health *h = &project->healths[i];
        if (h->entity >= gs->scene_entity_count) continue;
        push_health_component(gs, h->entity, h->current, h->max);
    }

    for (i = 0; i < project->box_collider_count; i++) {
        const project_box_collider *bc = &project->box_colliders[i];
        rect collider_box;
        float box_hh;
        if (bc->entity >= gs->scene_entity_count) continue;
        collider_box = collider_rect_from_half_extents(bc->half_extents, 0.5f);
        box_hh = bc->half_extents[1] > 0.0f ? bc->half_extents[1] : 0.5f;
        push_box_collider_component(gs, bc->entity, collider_box, box_hh);
    }

    for (i = 0; i < project->capsule_collider_count; i++) {
        const project_capsule_collider *cap = &project->capsule_colliders[i];
        if (cap->entity >= gs->scene_entity_count) continue;
        push_capsule_collider_component(gs, cap->entity, cap->radius, cap->half_height);
    }

    for (i = 0; i < project->mesh_count; i++) {
        const project_mesh *m = &project->meshes[i];
        const char *model_path = NULL;
        const char *anim_path = NULL;
        const project_anim *pa;
        int model_asset;
        if (m->entity >= gs->scene_entity_count) continue;
        if (!resolve_project_model_path(project, m->model, &model_path)) continue;

        pa = project_find_anim(project, m->entity);
        if (pa && pa->asset[0])
            resolve_project_animation_path(project, pa->asset, &anim_path);

        model_asset = register_scene_model_asset(gs, m->model, model_path, anim_path);
        if (model_asset >= 0) {
            push_mesh_component(gs, m->entity, model_asset);
            if (gs->mesh_component_count > 0)
                gs->mesh_components[gs->mesh_component_count - 1].visible = m->visible;
        }
    }

    for (i = 0; i < project->anim_count; i++) {
        const project_anim *a = &project->anims[i];
        animation_component *ac;
        if (a->entity >= gs->scene_entity_count) continue;
        push_animation_component(gs, a->entity, a->clip);
        if (gs->animation_component_count > 0) {
            ac = &gs->animation_components[gs->animation_component_count - 1];
            ac->playing = a->playing;
            ac->anim_time = a->time;
            if (a->speed > 0.0f) ac->speed = a->speed;
        }
    }

    for (i = 0; i < project->camera_count; i++) {
        const project_cam *c = &project->cameras[i];
        if (c->entity >= gs->scene_entity_count) continue;
        push_camera_component(gs, c->entity,
                              c->fov, c->near_plane, c->far_plane,
                              VEC3(c->target[0], c->target[1], c->target[2]),
                              VEC3(c->up[0], c->up[1], c->up[2]));
        gs->scene_camera_entity = c->entity;
    }

    for (i = 0; i < project->bot_count; i++) {
        const project_bot *b = &project->bots[i];
        int beh = BOT_BEHAVIOR_IDLE;
        if (strcmp(b->behavior, "walk_forward") == 0) beh = BOT_BEHAVIOR_WALK_FORWARD;
        else if (strcmp(b->behavior, "circle")   == 0) beh = BOT_BEHAVIOR_CIRCLE;
        else if (strcmp(b->behavior, "patrol")   == 0) beh = BOT_BEHAVIOR_PATROL;
        push_bot_component(gs, b->entity, beh, b->phase);
    }

    for (i = 0; i < project->trigger_count; i++) {
        const project_trigger *tr = &project->triggers[i];
        trigger_type ttype = TRIGGER_DOOR;
        if (tr->entity >= gs->scene_entity_count) continue;
        if (strcmp(tr->type_str, "pickup") == 0) ttype = TRIGGER_PICKUP;
        else if (strcmp(tr->type_str, "weapon_pickup") == 0) ttype = TRIGGER_WEAPON_PICKUP;
        else if (strcmp(tr->type_str, "zone") == 0) ttype = TRIGGER_ZONE;
        push_trigger_component(gs, tr->entity, ttype, tr->target, tr->radius,
                               tr->joint[0] ? tr->joint : NULL);
        fprintf(stderr, "[build_scene] trigger: entity=%d type='%s' ttype=%d target=%d radius=%.1f joint='%s'\n",
                tr->entity, tr->type_str, (int)ttype, tr->target, tr->radius,
                tr->joint[0] ? tr->joint : "");
    }

    if (gs->camera_component_count <= 0 && gs->scene_entity_count < gs->scene_entity_capacity) {
        int camera_index = gs->scene_entity_count++;
        float cam_fov = gs->mesh3d.camera_fov_deg > 0.0f ? gs->mesh3d.camera_fov_deg : 60.0f;
        float cam_near = gs->mesh3d.camera_near > 0.0f ? gs->mesh3d.camera_near : 0.1f;
        float cam_far = gs->mesh3d.camera_far > cam_near ? gs->mesh3d.camera_far : 100.0f;
        Vec3 cam_eye = gs->mesh3d.camera_set_by_project ? gs->mesh3d.camera_eye : VEC3(0.0f, 1.0f, 3.0f);
        Vec3 cam_target = gs->mesh3d.camera_set_by_project ? gs->mesh3d.camera_target : VEC3(0.0f, 0.5f, 0.0f);
        Vec3 cam_up = gs->mesh3d.camera_set_by_project ? gs->mesh3d.camera_up : VEC3(0.0f, 1.0f, 0.0f);
        push_transform_component(gs, camera_index, cam_eye);
        push_camera_component(gs, camera_index, cam_fov, cam_near, cam_far, cam_target, cam_up);
        gs->scene_camera_entity = camera_index;
    }
}

static int ensure_mesh3d_pose_buffers(game_state *gs, uint32_t joint_count) {
    if (!gs || !gs->root_arena) return 0;
    if (joint_count == 0) return 1;

    if (gs->mesh3d.skin_mats &&
        gs->mesh3d.skeleton.joint_count == joint_count &&
        gs->mesh3d.pose_trans && gs->mesh3d.pose_rot &&
        gs->mesh3d.pose_scale &&
        gs->mesh3d.blend_from_trans && gs->mesh3d.blend_from_rot && gs->mesh3d.blend_from_scale &&
        gs->mesh3d.blend_to_trans && gs->mesh3d.blend_to_rot && gs->mesh3d.blend_to_scale &&
        gs->mesh3d.world_mats) {
        return 1;
    }

    gs->mesh3d.pose_trans = (Vec3 *)arena_alloc(gs->root_arena, joint_count * sizeof(Vec3), 16, "pose_trans");
    gs->mesh3d.pose_rot = (Quat *)arena_alloc(gs->root_arena, joint_count * sizeof(Quat), 16, "pose_rot");
    gs->mesh3d.pose_scale = (Vec3 *)arena_alloc(gs->root_arena, joint_count * sizeof(Vec3), 16, "pose_scale");
    gs->mesh3d.blend_from_trans = (Vec3 *)arena_alloc(gs->root_arena, joint_count * sizeof(Vec3), 16, "blend_from_trans");
    gs->mesh3d.blend_from_rot = (Quat *)arena_alloc(gs->root_arena, joint_count * sizeof(Quat), 16, "blend_from_rot");
    gs->mesh3d.blend_from_scale = (Vec3 *)arena_alloc(gs->root_arena, joint_count * sizeof(Vec3), 16, "blend_from_scale");
    gs->mesh3d.blend_to_trans = (Vec3 *)arena_alloc(gs->root_arena, joint_count * sizeof(Vec3), 16, "blend_to_trans");
    gs->mesh3d.blend_to_rot = (Quat *)arena_alloc(gs->root_arena, joint_count * sizeof(Quat), 16, "blend_to_rot");
    gs->mesh3d.blend_to_scale = (Vec3 *)arena_alloc(gs->root_arena, joint_count * sizeof(Vec3), 16, "blend_to_scale");
    gs->mesh3d.world_mats = (Mat4 *)arena_alloc(gs->root_arena, joint_count * sizeof(Mat4), 16, "world_mats");
    gs->mesh3d.skin_mats = (Mat4 *)arena_alloc(gs->root_arena, joint_count * sizeof(Mat4), 16, "skin_mats");
    return gs->mesh3d.pose_trans && gs->mesh3d.pose_rot &&
           gs->mesh3d.pose_scale &&
           gs->mesh3d.blend_from_trans && gs->mesh3d.blend_from_rot && gs->mesh3d.blend_from_scale &&
           gs->mesh3d.blend_to_trans && gs->mesh3d.blend_to_rot && gs->mesh3d.blend_to_scale &&
           gs->mesh3d.world_mats && gs->mesh3d.skin_mats;
}

/* Extract filename from a path (e.g. "assets/models/foo.glb" → "foo.glb") */
static const char *asset_filename(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
    return slash ? slash + 1 : path;
}

/* ── Parallel model loading ─────────────────────────────────────── */

typedef struct {
    /* inputs (read-only from thread) */
    const char          *path;
    const char          *anim_path;
    int                  has_animation;
    const project_data  *project;      /* for extra animation assets (read-only) */
    arena               *ar;           /* per-model sub-arena */
    /* outputs */
    GltfModel            model;
    int                  ok;
    uint64_t             t_start, t_end;  /* perf counter for thread trace */
} model_load_task;

static int model_load_fn(void *userdata) {
    model_load_task *t = (model_load_task *)userdata;
    t->t_start = SDL_GetPerformanceCounter();

    t->model = load_glb(t->path, t->ar);
    t->ok = (t->model.mesh.primitive_count > 0) ? 1 : 0;

    /* Load animations if model has a skeleton */
    if (t->model.skeleton.joint_count > 0 && t->has_animation && t->anim_path[0]) {
        load_animations_glb(t->anim_path, &t->model, t->ar);
        /* Also load all other animation assets from the project */
        if (t->project) {
            int ai;
            for (ai = 0; ai < t->project->asset_count; ai++) {
                if (t->project->assets[ai].type != ASSET_ANIMATION) continue;
                if (strcmp(t->project->assets[ai].path, t->anim_path) == 0)
                    continue;
                load_animations_glb(t->project->assets[ai].path,
                                    &t->model, t->ar);
            }
        }
    }

    t->t_end = SDL_GetPerformanceCounter();
    return 0;
}

static uint64_t s_model_boot_t0;  /* perf counter at model loading start */

static void write_model_thread_traces(const model_load_task *tasks, int count) {
    FILE *f = fopen("build/Debug/model_threads.json", "w");
    uint64_t freq = SDL_GetPerformanceFrequency();
    int i, first = 1;
    if (!f) return;

    fprintf(f, "[\n");
    for (i = 0; i < count; i++) {
        uint64_t ts, dur;
        if (!tasks[i].t_start || !tasks[i].t_end) continue;
        ts  = (tasks[i].t_start - s_model_boot_t0) * 1000000 / freq;
        dur = (tasks[i].t_end - tasks[i].t_start) * 1000000 / freq;
        if (!first) fprintf(f, ",\n");
        fprintf(f, "{\"cat\":\"boot\",\"pid\":\"Anitra\",\"tid\":\"model_%d\","
                    "\"ph\":\"X\",\"name\":\"glb:%s\","
                    "\"ts\":%llu,\"dur\":%llu}",
                i, asset_filename(tasks[i].path),
                (unsigned long long)ts, (unsigned long long)dur);
        first = 0;
    }
    fprintf(f, "\n]\n");
    fclose(f);
    printf("[model_threads] wrote build/Debug/model_threads.json (%d threads)\n", count);
}

static void load_scene_model_assets(game_state *gs) {
    int i, ti;
    int needs_load = 0;
    arena *model_arena;
    uint32_t per_model_bytes;
    model_load_task tasks[SCENE_MODEL_ASSET_MAX];
    SDL_Thread *threads[SCENE_MODEL_ASSET_MAX];
    int task_count = 0;

    if (!gs || !gs->gpu_device || !gs->root_arena) return;

    /* Count models that need loading */
    for (i = 0; i < gs->scene_model_asset_count; i++) {
        if (!gs->scene_model_assets[i].loaded)
            needs_load++;
    }
    if (needs_load == 0) return;

    /* Allocate parent arena, then carve per-model sub-arenas */
    model_arena = arena_alloc_subarena(gs->root_arena, MODEL_ARENA_BYTES, 16, "scene_models");
    if (!model_arena) {
        fprintf(stderr, "Warning: failed to allocate scene model arena\n");
        return;
    }
    per_model_bytes = (MODEL_ARENA_BYTES - (uint32_t)(needs_load * sizeof(arena))) / needs_load;

    gltf_set_gpu_device(gs->gpu_device);
    gltf_set_boot_profiler(NULL);  /* disable nanoprof in threads (single-threaded profiler) */
    gltf_tex_cache_init();

    s_model_boot_t0 = SDL_GetPerformanceCounter();

    /* Build tasks and spawn threads */
    for (i = 0; i < gs->scene_model_asset_count; i++) {
        scene_model_asset *asset = &gs->scene_model_assets[i];
        model_load_task *t;
        char tag[32];
        if (asset->loaded) continue;

        t = &tasks[task_count];
        memset(t, 0, sizeof(*t));
        t->path          = asset->path;
        t->anim_path     = asset->animation_path;
        t->has_animation = asset->has_animation_path;
        t->project       = &gs->project;

        snprintf(tag, sizeof(tag), "model_%d", task_count);
        t->ar = arena_alloc_subarena(model_arena, per_model_bytes, 16, tag);
        if (!t->ar) {
            fprintf(stderr, "Warning: failed to allocate sub-arena for model %d\n", task_count);
            continue;
        }

        threads[task_count] = SDL_CreateThread(model_load_fn, "glb_load", t);
        task_count++;
    }

    /* Join all threads */
    for (i = 0; i < task_count; i++)
        SDL_WaitThread(threads[i], NULL);

    /* Copy results back to scene_model_assets */
    ti = 0;
    for (i = 0; i < gs->scene_model_asset_count; i++) {
        scene_model_asset *asset = &gs->scene_model_assets[i];
        if (asset->loaded) continue;
        if (ti >= task_count) break;

        asset->model       = tasks[ti].model;
        asset->loaded      = 1;
        asset->has_skeleton = asset->model.skeleton.joint_count > 0 ? 1 : 0;

        if (asset->model.mesh.primitive_count == 0) {
            fprintf(stderr, "Warning: model '%s' has no primitives\n", asset->path);
        }
        ti++;
    }

    gltf_tex_cache_free();
    gltf_set_boot_profiler(gs->boot_prof_begin);  /* restore profiler */

    write_model_thread_traces(tasks, task_count);
}

static void sync_primary_mesh3d_asset(game_state *gs) {
    mesh_component *primary = query_primary_mesh_component(gs);
    scene_model_asset *asset = NULL;
    Mat4 entity_world;
    if (!gs || !primary) {
        if (gs) {
            gs->mesh3d.visible = 0;
            gs->mesh3d.primitive_count = 0;
            gs->mesh3d.clip_count = 0;
            gs->scene_primary_skinned_entity = -1;
        }
        return;
    }

    asset = find_scene_model_asset(gs, primary->model_asset_index);
    if (!asset || !asset->loaded || asset->model.mesh.primitive_count == 0) {
        gs->mesh3d.visible = 0;
        gs->mesh3d.primitive_count = 0;
        gs->mesh3d.clip_count = 0;
        gs->scene_primary_skinned_entity = -1;
        return;
    }

    entity_world = resolve_world_transform(gs, primary->entity_index);
    gs->mesh3d.model_transform = mat4_mul(entity_world, asset->model.armature_transform);
    gs->mesh3d.visible = is_entity_visible(gs, primary->entity_index) ? 1 : 0;

    if (!asset->has_skeleton || asset->model.skeleton.joint_count == 0) {
        gs->mesh3d.primitive_count = 0;
        gs->mesh3d.clip_count = 0;
        gs->scene_primary_skinned_entity = -1;
        return;
    }

    gs->scene_primary_skinned_entity = primary->entity_index;

    if (!ensure_mesh3d_pose_buffers(gs, asset->model.skeleton.joint_count)) {
        gs->mesh3d.visible = 0;
        gs->mesh3d.primitive_count = 0;
        gs->mesh3d.clip_count = 0;
        gs->scene_primary_skinned_entity = -1;
        return;
    }

    gs->mesh3d.skeleton = asset->model.skeleton;
    gs->mesh3d.clips = asset->model.clips;
    gs->mesh3d.clip_count = asset->model.clip_count;
    gs->mesh3d.primitive_count = asset->model.mesh.primitive_count;
    if (gs->mesh3d.active_clip >= gs->mesh3d.clip_count) gs->mesh3d.active_clip = 0;
    gs->loaded_model = asset->model;
}

static void build_mesh_draw_commands(game_state *gs) {
    int i;
    if (!gs || !gs->dl.meshes) return;

    gs->dl.mesh_count = 0;
    if (!gs->scene_entities) return;

    for (i = 0; i < gs->mesh_component_count; i++) {
        mesh_component *mc = &gs->mesh_components[i];
        scene_model_asset *asset;
        Mat4 world;
        Mat4 model;
        mesh_draw_command *cmd;

        if (mc->entity_index < 0 || mc->entity_index >= gs->scene_entity_count) continue;
        if (!is_entity_visible(gs, mc->entity_index)) continue;
        asset = find_scene_model_asset(gs, mc->model_asset_index);
        if (!asset || !asset->loaded || asset->model.mesh.primitive_count == 0) continue;
        if (gs->dl.mesh_count >= gs->dl.mesh_capacity) break;

        world = resolve_world_transform(gs, mc->entity_index);
        model = mat4_mul(world, asset->model.armature_transform);

        cmd = &gs->dl.meshes[gs->dl.mesh_count++];
        cmd->model_asset_index = mc->model_asset_index;
        cmd->bone_buffer_offset = 0;

        /* Check SM instance first, then fall back to primary skinned entity */
        if (asset->has_skeleton && gs->anim.instance_count > 0) {
            anim_instance *inst = anim_sm_find_instance(&gs->anim, mc->entity_index);
            if (inst) {
                cmd->use_skinned_bones = 1;
                cmd->bone_buffer_offset = inst->skin_mats_offset;
            } else {
                cmd->use_skinned_bones = 0;
            }
        } else {
            cmd->use_skinned_bones = (asset->has_skeleton &&
                                      mc->entity_index == gs->scene_primary_skinned_entity) ? 1 : 0;
        }
        memcpy(cmd->model, model.m, sizeof(float) * 16);
    }
}

void update_input(game_state* gs) {
    const float camera_speed = 300.0f;
    const float zoom_speed = 2.0f;
    camera_query_result active_camera;
    int has_active_camera = 0;
    if (!gs) return;
    has_active_camera = query_active_camera(gs, &active_camera);

    if (gs->input.input_mask & INPUT_X) {
        if (has_active_camera) {
            active_camera.transform->position.x += gs->input.horizontal * camera_speed * gs->dt;
            active_camera.transform->position.y += gs->input.vertical * camera_speed * gs->dt;
        } else {
            gs->camera.position.x += gs->input.horizontal * camera_speed * gs->dt;
            gs->camera.position.y += gs->input.vertical * camera_speed * gs->dt;
        }
    }
    if (gs->input.input_mask & INPUT_Y) {
        float zoom_delta = gs->input.vertical * zoom_speed * gs->dt;
        if (has_active_camera) {
            active_camera.camera->fov_deg = fmaxf(20.0f, fminf(110.0f, active_camera.camera->fov_deg + zoom_delta * 30.0f));
        } else {
            gs->camera.zoom = fmaxf(0.1f, fminf(5.0f, gs->camera.zoom + zoom_delta));
        }
    }
}

/* ── Animation SM pool allocation ─────────────────────────────── */

static void anim_sm_init_pool(anim_sm *sm, arena *a) {
    int total_mats = ANIM_SM_MAX_ENTITIES * ANIM_SM_MAX_JOINTS;
    sm->pool.skin_mats = (Mat4 *)arena_alloc(a,
        (uint32_t)(total_mats * sizeof(Mat4)), 16, "anim_sm_skin_mats");
    if (!sm->pool.skin_mats) {
        fprintf(stderr, "anim_sm: failed to allocate skin_mats pool (%d bytes)\n",
                (int)(total_mats * sizeof(Mat4)));
        return;
    }
    sm->pool.skin_mats_count = 0;

    sm->pool.pose_trans      = (Vec3 *)arena_alloc(a, (uint32_t)(ANIM_SM_MAX_JOINTS * sizeof(Vec3)), 16, "anim_sm_pose_t");
    sm->pool.pose_rot        = (Quat *)arena_alloc(a, (uint32_t)(ANIM_SM_MAX_JOINTS * sizeof(Quat)), 16, "anim_sm_pose_r");
    sm->pool.pose_scale      = (Vec3 *)arena_alloc(a, (uint32_t)(ANIM_SM_MAX_JOINTS * sizeof(Vec3)), 16, "anim_sm_pose_s");
    sm->pool.blend_from_trans = (Vec3 *)arena_alloc(a, (uint32_t)(ANIM_SM_MAX_JOINTS * sizeof(Vec3)), 16, "anim_sm_bf_t");
    sm->pool.blend_from_rot   = (Quat *)arena_alloc(a, (uint32_t)(ANIM_SM_MAX_JOINTS * sizeof(Quat)), 16, "anim_sm_bf_r");
    sm->pool.blend_from_scale = (Vec3 *)arena_alloc(a, (uint32_t)(ANIM_SM_MAX_JOINTS * sizeof(Vec3)), 16, "anim_sm_bf_s");
    sm->pool.blend_to_trans   = (Vec3 *)arena_alloc(a, (uint32_t)(ANIM_SM_MAX_JOINTS * sizeof(Vec3)), 16, "anim_sm_bt_t");
    sm->pool.blend_to_rot     = (Quat *)arena_alloc(a, (uint32_t)(ANIM_SM_MAX_JOINTS * sizeof(Quat)), 16, "anim_sm_bt_r");
    sm->pool.blend_to_scale   = (Vec3 *)arena_alloc(a, (uint32_t)(ANIM_SM_MAX_JOINTS * sizeof(Vec3)), 16, "anim_sm_bt_s");
    sm->pool.world_mats       = (Mat4 *)arena_alloc(a, (uint32_t)(ANIM_SM_MAX_JOINTS * sizeof(Mat4)), 16, "anim_sm_world");

    if (!sm->pool.pose_trans || !sm->pool.pose_rot || !sm->pool.pose_scale ||
        !sm->pool.world_mats) {
        fprintf(stderr, "anim_sm: failed to allocate scratch buffers\n");
        sm->pool.skin_mats = NULL;
        return;
    }

    sm->pool_initialized = 1;
    memset(sm->entity_to_instance, 0xFF, sizeof(sm->entity_to_instance));
}

static error_value anim_sm_add_state(anim_sm *sm, const char *name, int clip_index,
                                      int looping, int *out_index) {
    anim_state_table *st;
    if (sm->state_count >= ANIM_SM_MAX_STATES)
        ERRV_RETURN_ERR(1, "anim state overflow");
    st = &sm->states[sm->state_count];
    memset(st, 0, sizeof(*st));
    strncpy(st->name, name, ANIM_SM_STATE_NAME_MAX - 1);
    st->clip_index = clip_index;
    st->looping = looping;
    st->count = 0;
    if (out_index) *out_index = sm->state_count;
    sm->state_count++;
    ERRV_RETURN_OK();
}

static error_value anim_sm_add_rule(anim_sm *sm, int from_state, int to_state,
                                     anim_condition_type cond, float threshold,
                                     float blend_duration, int *out_index) {
    anim_transition_rule *r;
    if (sm->rule_count >= ANIM_SM_MAX_RULES)
        ERRV_RETURN_ERR(1, "anim rule overflow");
    r = &sm->rules[sm->rule_count];
    r->from_state = from_state;
    r->to_state = to_state;
    r->condition = cond;
    r->threshold = threshold;
    r->blend_duration = blend_duration;
    if (out_index) *out_index = sm->rule_count;
    sm->rule_count++;
    ERRV_RETURN_OK();
}

static void anim_sm_insert_entity_into_state(anim_sm *sm, int state_index,
                                              int entity_index) {
    anim_state_table *st;
    if (state_index < 0 || state_index >= sm->state_count) return;
    st = &sm->states[state_index];
    if (st->count >= ANIM_SM_MAX_ENTITIES) return;
    st->entity_indices[st->count] = entity_index;
    st->anim_times[st->count] = 0.0f;
    st->count++;
}

static void anim_sm_remove_entity_from_state(anim_sm *sm, int state_index,
                                              int entity_index) {
    anim_state_table *st;
    int i;
    if (state_index < 0 || state_index >= sm->state_count) return;
    st = &sm->states[state_index];
    for (i = 0; i < st->count; i++) {
        if (st->entity_indices[i] == entity_index) {
            st->entity_indices[i] = st->entity_indices[st->count - 1];
            st->anim_times[i] = st->anim_times[st->count - 1];
            st->count--;
            return;
        }
    }
}

static anim_instance *anim_sm_find_instance(anim_sm *sm, int entity_index) {
    int idx;
    if (entity_index < 0 || entity_index >= 512) return NULL;
    idx = sm->entity_to_instance[entity_index];
    if (idx >= 0 && idx < sm->instance_count) return &sm->instances[idx];
    return NULL;
}

static int anim_sm_register_entity(anim_sm *sm, int entity_index,
                                    int model_asset_index, int initial_state,
                                    int joint_count) {
    anim_instance *inst;
    if (sm->instance_count >= ANIM_SM_MAX_ENTITIES) return -1;
    if (!sm->pool_initialized) return -1;

    inst = &sm->instances[sm->instance_count++];
    inst->entity_index = entity_index;
    inst->model_asset_index = model_asset_index;
    if (sm->next_skin_mats_offset + joint_count > ANIM_SM_MAX_ENTITIES * ANIM_SM_MAX_JOINTS) {
        sm->instance_count--;
        fprintf(stderr, "anim_sm: skin_mats pool exhausted (need %d, cap %d)\n",
                sm->next_skin_mats_offset + joint_count, ANIM_SM_MAX_ENTITIES * ANIM_SM_MAX_JOINTS);
        return -1;
    }

    inst->skin_mats_offset = sm->next_skin_mats_offset;
    inst->speed = 1.0f;
    inst->playing = 1;

    sm->next_skin_mats_offset += joint_count;
    sm->pool.skin_mats_count = sm->next_skin_mats_offset;

    /* Populate O(1) lookup */
    if (entity_index >= 0 && entity_index < 512)
        sm->entity_to_instance[entity_index] = sm->instance_count - 1;

    anim_sm_insert_entity_into_state(sm, initial_state, entity_index);
    return sm->instance_count - 1;
}

static void anim_sm_register_scene_entities(game_state *gs) {
    anim_sm *sm = &gs->anim;
    int i;
    if (!sm->pool_initialized || sm->state_count == 0) return;

    for (i = 0; i < gs->animation_component_count; i++) {
        animation_component *ac = &gs->animation_components[i];
        mesh_component *mc = find_mesh_component(gs, ac->entity_index);
        scene_model_asset *asset;
        int initial_state;
        if (!mc) continue;
        asset = find_scene_model_asset(gs, mc->model_asset_index);
        if (!asset || !asset->loaded || !asset->has_skeleton) continue;

        /* Default to first state (idle), or use the active_clip if it matches a state */
        initial_state = 0;
        if (ac->active_clip >= 0 && ac->active_clip < sm->state_count) {
            int s;
            for (s = 0; s < sm->state_count; s++) {
                if (sm->states[s].clip_index == ac->active_clip) {
                    initial_state = s;
                    break;
                }
            }
        }

        anim_sm_register_entity(sm, ac->entity_index, mc->model_asset_index,
                                initial_state, (int)asset->model.skeleton.joint_count);
    }
}

EXPORT void init_engine(game_state *gs) {
    int target_entities;
    if (!gs) return;

    /* ── game_state migration check ──────────────────────────────── */
    ENG_BOOT_PROF(gs, TP_ENG_MIGRATION);
    if (gs->mig_hdr && gs->gameplay) {
        uint64_t old_hash = gs->mig_hdr->layout_hash;
        uint64_t new_hash = mig_compute_hash(mig_game_state_fields,
                                              MIG_game_state_COUNT);
        if (old_hash != new_hash) {
            uint32_t old_size = gs->mig_hdr->struct_size;
            uint32_t copy_size = old_size < sizeof(game_state)
                               ? old_size : (uint32_t)sizeof(game_state);
            void *old_copy = arena_alloc(gs->gameplay, copy_size, 8,
                                          "mig_game_old");
            if (old_copy) {
                mig_header *old_hdr = gs->mig_hdr;
                struct arena *saved_root = gs->root_arena;
                struct arena *saved_gameplay = gs->gameplay;
                memcpy(old_copy, gs, copy_size);
                memset(gs, 0, sizeof(game_state));
                mig_migrate_struct(gs, mig_game_state_fields,
                                   MIG_game_state_COUNT,
                                   (uint32_t)sizeof(game_state),
                                   old_copy, old_hdr);
                gs->root_arena = saved_root;
                gs->gameplay = saved_gameplay;
                gs->mig_hdr = NULL;
                fprintf(stderr, "Migrated game_state (%u -> %u bytes)\n",
                        old_size, (uint32_t)sizeof(game_state));
            }
        }
    }

    /* Store current field table */
    if (!gs->mig_hdr && gs->gameplay) {
        gs->mig_hdr = (mig_header *)mig_store(
            gs->gameplay, mig_game_state_fields,
            MIG_game_state_COUNT, (uint32_t)sizeof(game_state),
            mig_compute_hash(mig_game_state_fields, MIG_game_state_COUNT),
            "mig_game_state");
    }

    /* ── Register system table (re-register every init for hot-reload) ── */
    ENG_BOOT_PROF(gs, TP_ENG_REGISTER_SYSTEMS);
    gs->system_count = 0;
    register_system(gs, "clear_draw_lists",     (system_fn)system_clear_draw_lists,         0);
    register_system(gs, "input",                (system_fn)update_input,                    1);
    register_system(gs, "bots",                 (system_fn)run_bot_system,                  1);
    register_system(gs, "character_controller",  (system_fn)run_character_controller_system, 1);
    register_system(gs, "animation_sm",         (system_fn)system_animation_sm,             1);
    register_system(gs, "bone_attachments",     (system_fn)update_bone_attachments,         0);
    register_system(gs, "movement",             (system_fn)apply_movement,                  1);
    register_system(gs, "collision",            (system_fn)collision,                       1);
    register_system(gs, "triggers",             (system_fn)update_triggers,                 1);
    register_system(gs, "mesh_sync",            (system_fn)system_mesh_sync,                0);
    register_system(gs, "animation_legacy",     (system_fn)system_animation_legacy,         0);
    register_system(gs, "debug_lines",          (system_fn)system_flush_debug_lines,        0);

    target_entities = 8;
    if (gs->project_loaded) {
        if (gs->project.scene_entity_count > 0) {
            target_entities = gs->project.scene_entity_count + 1;
        } else {
            target_entities = gs->project.entity_count + gs->project.piece_count + 1;
        }
        if (target_entities < 8) target_entities = 8;
    }

    ENG_BOOT_PROF(gs, TP_ENG_SCENE_STORAGE);
    {
        error_value err = ensure_scene_storage(gs, target_entities);
        if (!ERRV_IS_OK(err)) { errv_log("ensure_scene_storage", err); return; }
    }
    fprintf(stderr, "[init_engine] scene storage OK (%d entities)\n", target_entities);
    fflush(stderr);

    /* Init anim SM pool (arena pointers don't survive hot-reload) */
    ENG_BOOT_PROF(gs, TP_ENG_ANIM_POOL);
    if (!gs->anim.pool_initialized && gs->gameplay) {
        anim_sm_init_pool(&gs->anim, gs->gameplay);
        fprintf(stderr, "[init_engine] anim pool init: %s\n",
                gs->anim.pool_initialized ? "OK" : "FAILED");
        fflush(stderr);
    }

    /* Reset SM states/instances so they get rebuilt with fresh entities */
    gs->anim.state_count = 0;
    gs->anim.rule_count = 0;
    gs->anim.instance_count = 0;
    gs->anim.blend_count = 0;
    gs->anim.next_skin_mats_offset = 0;

    clear_scene_storage(gs);

    ENG_BOOT_PROF(gs, TP_ENG_BUILD_SCENE);
    if (gs->project_loaded) {
        fprintf(stderr, "[init_engine] building scene from project...\n"); fflush(stderr);
        build_scene_from_project(gs);
        fprintf(stderr, "[init_engine] scene built: %d entities, %d anim comps\n",
                gs->scene_entity_count, gs->animation_component_count); fflush(stderr);
    } else {
        build_fallback_scene(gs);
    }
    ensure_default_character_controller(gs);

    if (gs->mesh3d.camera_fov_deg <= 0.0f) gs->mesh3d.camera_fov_deg = 60.0f;
    if (gs->mesh3d.camera_near <= 0.0f) gs->mesh3d.camera_near = 0.1f;
    if (gs->mesh3d.camera_far <= gs->mesh3d.camera_near) gs->mesh3d.camera_far = 100.0f;

    ENG_BOOT_PROF(gs, TP_ENG_LOAD_MODEL_ASSETS);
    fprintf(stderr, "[init_engine] loading model assets...\n"); fflush(stderr);
    load_scene_model_assets(gs);
    fprintf(stderr, "[init_engine] model assets loaded, %d assets\n",
            gs->scene_model_asset_count); fflush(stderr);
    ENG_BOOT_PROF(gs, TP_ENG_SYNC_MESH);
    sync_primary_mesh3d_asset(gs);
    sync_mesh_camera_from_components(gs);

    ENG_BOOT_PROF(gs, TP_ENG_INIT_POSE);
    if (gs->mesh3d.skeleton.joint_count > 0 && gs->mesh3d.skin_mats) {
        init_pose_from_bind(&gs->mesh3d.skeleton,
                            gs->mesh3d.pose_trans, gs->mesh3d.pose_rot, gs->mesh3d.pose_scale);
        compute_world_transforms(&gs->mesh3d.skeleton,
                                 gs->mesh3d.pose_trans, gs->mesh3d.pose_rot, gs->mesh3d.pose_scale,
                                 gs->mesh3d.world_mats);
        compute_skinning_matrices(&gs->mesh3d.skeleton, gs->mesh3d.world_mats, gs->mesh3d.skin_mats);
    }
    gs->animation_transition_count = 0;

    /* ── Anim SM: set up default states and register entities ──── */
    ENG_BOOT_PROF(gs, TP_ENG_ANIM_SM_SETUP);
    if (gs->anim.pool_initialized && gs->anim.state_count == 0 &&
        gs->mesh3d.clip_count > 0) {
        int idle_clip = -1, run_clip = -1, attack_clip = -1;
        uint32_t ci;

        /* Find clips by name (case-insensitive substring match) */
        for (ci = 0; ci < gs->mesh3d.clip_count; ci++) {
            const char *n = gs->mesh3d.clips[ci].name;
            if (idle_clip < 0 && (strstr(n, "idle") || strstr(n, "Idle")))
                idle_clip = (int)ci;
            else if (run_clip < 0 && (strstr(n, "run") || strstr(n, "Run")))
                run_clip = (int)ci;
            else if (attack_clip < 0 && strstr(n, "Melee_1H_Attack_Chop"))
                attack_clip = (int)ci;
        }
        /* Fallback: first clip = idle, second = run */
        if (idle_clip < 0) idle_clip = 0;
        if (run_clip < 0 && gs->mesh3d.clip_count > 1)
            run_clip = (idle_clip == 0) ? 1 : 0;

        printf("Anim SM: idle=clip %d, run=clip %d, attack=clip %d (of %u)\n",
               idle_clip, run_clip, attack_clip, gs->mesh3d.clip_count);
        fflush(stdout);

        {
            int idle_state = -1, run_state = -1, attack_state = -1;
            error_value err;

            err = anim_sm_add_state(&gs->anim, "idle", idle_clip, 1, &idle_state);
            if (!ERRV_IS_OK(err)) { errv_log("add_state(idle)", err); return; }

            if (run_clip >= 0) {
                err = anim_sm_add_state(&gs->anim, "run", run_clip, 1, &run_state);
                if (!ERRV_IS_OK(err)) { errv_log("add_state(run)", err); return; }
                anim_sm_add_rule(&gs->anim, idle_state, run_state,
                                 ANIM_COND_VELOCITY_ABOVE, 0.1f, 0.18f, NULL);
                anim_sm_add_rule(&gs->anim, run_state, idle_state,
                                 ANIM_COND_VELOCITY_BELOW, 0.1f, 0.18f, NULL);
            }
            if (attack_clip >= 0) {
                err = anim_sm_add_state(&gs->anim, "attack", attack_clip, 0, &attack_state);
                if (!ERRV_IS_OK(err)) { errv_log("add_state(attack)", err); return; }
                anim_sm_add_rule(&gs->anim, idle_state, attack_state,
                                 ANIM_COND_INPUT_BUTTON, (float)INPUT_B, 0.08f, NULL);
                if (run_state >= 0)
                    anim_sm_add_rule(&gs->anim, run_state, attack_state,
                                     ANIM_COND_INPUT_BUTTON, (float)INPUT_B, 0.08f, NULL);
                anim_sm_add_rule(&gs->anim, attack_state, idle_state,
                                 ANIM_COND_CLIP_FINISHED, 0.0f, 0.15f, NULL);
            }
        }
        fprintf(stderr, "[init_engine] anim SM states/rules OK, registering %d anim entities...\n",
                gs->animation_component_count);
        fflush(stderr);
        anim_sm_register_scene_entities(gs);
        fprintf(stderr, "[init_engine] anim SM registered %d instances\n", gs->anim.instance_count);
        fflush(stderr);
    }
    fprintf(stderr, "[init_engine] complete\n");
    fflush(stderr);
}

static void update_mesh3d_from_animation_component(game_state *gs, animation_component *ac) {
    mesh3d_state *m = &gs->mesh3d;
    int transition_index = -1;
    animation_transition_entry *transition;
    if (!ac || !ac->playing) return;
    if (!m->visible || m->skeleton.joint_count == 0 || !m->skin_mats) return;
    if (ac->entity_index < 0 || ac->entity_index >= gs->scene_entity_count) return;

    if (m->clip_count > 0 && m->clips) {
        uint32_t ci;
        AnimClip *clip;
        uint32_t joint_count = m->skeleton.joint_count;

        if (ac->speed <= 0.0f) ac->speed = 1.0f;
        if (ac->active_clip < 0) ac->active_clip = 0;

        ci = (uint32_t)ac->active_clip;
        if (ci >= m->clip_count) ci = 0;
        ac->active_clip = (int)ci;
        clip = &m->clips[ci];

        transition = find_animation_transition_entry(gs, ac->entity_index, &transition_index);
        if (transition && transition->to_clip != ac->active_clip) {
            transition->from_clip = transition->to_clip;
            transition->from_time = transition->to_time;
            transition->to_clip = ac->active_clip;
            transition->to_time = ac->anim_time;
            transition->elapsed = 0.0f;
            transition->duration = ANIMATION_TRANSITION_DEFAULT_DURATION;
        } else if (!transition && (int)m->active_clip != ac->active_clip) {
            transition = upsert_animation_transition_entry(gs, ac->entity_index);
            if (transition) {
                transition->from_clip = (int)m->active_clip;
                transition->to_clip = ac->active_clip;
                transition->from_time = m->anim_time;
                transition->to_time = ac->anim_time;
                transition->elapsed = 0.0f;
                transition->duration = ANIMATION_TRANSITION_DEFAULT_DURATION;
                transition_index = -1;
                find_animation_transition_entry(gs, ac->entity_index, &transition_index);
            }
        }

        if (transition &&
            transition->from_clip >= 0 && transition->from_clip < (int)m->clip_count &&
            transition->to_clip >= 0 && transition->to_clip < (int)m->clip_count) {
            AnimClip *from_clip = &m->clips[(uint32_t)transition->from_clip];
            AnimClip *to_clip = &m->clips[(uint32_t)transition->to_clip];
            float alpha;
            uint32_t j;

            transition->from_time += gs->dt * ac->speed;
            if (from_clip->duration > 0.0f) {
                while (transition->from_time > from_clip->duration) transition->from_time -= from_clip->duration;
                while (transition->from_time < 0.0f) transition->from_time += from_clip->duration;
            }

            transition->to_time += gs->dt * ac->speed;
            if (to_clip->duration > 0.0f) {
                while (transition->to_time > to_clip->duration) transition->to_time -= to_clip->duration;
                while (transition->to_time < 0.0f) transition->to_time += to_clip->duration;
            }

            transition->elapsed += gs->dt;
            if (transition->duration <= 0.0f) transition->duration = ANIMATION_TRANSITION_DEFAULT_DURATION;
            alpha = transition->elapsed / transition->duration;
            if (alpha < 0.0f) alpha = 0.0f;
            if (alpha > 1.0f) alpha = 1.0f;

            init_pose_from_bind(&m->skeleton, m->blend_from_trans, m->blend_from_rot, m->blend_from_scale);
            sample_clip(from_clip, transition->from_time,
                        m->blend_from_trans, m->blend_from_rot, m->blend_from_scale,
                        joint_count);

            init_pose_from_bind(&m->skeleton, m->blend_to_trans, m->blend_to_rot, m->blend_to_scale);
            sample_clip(to_clip, transition->to_time,
                        m->blend_to_trans, m->blend_to_rot, m->blend_to_scale,
                        joint_count);

            for (j = 0; j < joint_count; j++) {
                m->pose_trans[j] = vec3_lerp(m->blend_from_trans[j], m->blend_to_trans[j], alpha);
                m->pose_rot[j] = quat_slerp(m->blend_from_rot[j], m->blend_to_rot[j], alpha);
                m->pose_scale[j] = vec3_lerp(m->blend_from_scale[j], m->blend_to_scale[j], alpha);
            }

            m->active_clip = (uint32_t)transition->to_clip;
            m->anim_time = transition->to_time;
            ac->anim_time = transition->to_time;

            compute_world_transforms(&m->skeleton, m->pose_trans, m->pose_rot, m->pose_scale,
                                     m->world_mats);
            compute_skinning_matrices(&m->skeleton, m->world_mats, m->skin_mats);

            if (transition->elapsed >= transition->duration && transition_index >= 0) {
                remove_animation_transition_entry_by_index(gs, transition_index);
            }
            return;
        }

        ac->anim_time += gs->dt * ac->speed;
        if (clip->duration > 0.0f) {
            while (ac->anim_time > clip->duration) ac->anim_time -= clip->duration;
            while (ac->anim_time < 0.0f) ac->anim_time += clip->duration;
        }

        m->active_clip = ci;
        m->anim_time = ac->anim_time;

        init_pose_from_bind(&m->skeleton, m->pose_trans, m->pose_rot, m->pose_scale);
        sample_clip(clip, ac->anim_time, m->pose_trans, m->pose_rot, m->pose_scale,
                    m->skeleton.joint_count);
        compute_world_transforms(&m->skeleton, m->pose_trans, m->pose_rot, m->pose_scale,
                                 m->world_mats);
        compute_skinning_matrices(&m->skeleton, m->world_mats, m->skin_mats);
    }
}

/* ── Anim SM 5-phase update pipeline ─────────────────────────── */

static float anim_sm_entity_speed(game_state *gs, int entity_index) {
    int cur = entity_index;
    int guard = 0;
    /* Walk up through parents to find velocity or CC */
    while (cur >= 0 && guard < 8) {
        velocity_component *vc = find_velocity_component(gs, cur);
        if (vc) {
            float spd = sqrtf(vc->velocity.x * vc->velocity.x +
                              vc->velocity.z * vc->velocity.z);
            if (spd > 0.0f) return spd;
        }
        {
            parent_transform_component *pt =
                find_parent_transform_component(gs, cur);
            if (!pt || pt->parent_entity_index == cur) break;
            cur = pt->parent_entity_index;
        }
        guard++;
    }
    return 0.0f;
}

static int anim_sm_eval_condition(game_state *gs, anim_transition_rule *rule,
                                  int entity_index, float anim_time,
                                  int clip_index) {
    float speed;
    switch (rule->condition) {
    case ANIM_COND_VELOCITY_ABOVE:
        speed = anim_sm_entity_speed(gs, entity_index);
        return speed > rule->threshold;
    case ANIM_COND_VELOCITY_BELOW:
        speed = anim_sm_entity_speed(gs, entity_index);
        return speed < rule->threshold;
    case ANIM_COND_CLIP_FINISHED: {
        scene_model_asset *asset;
        anim_instance *inst = anim_sm_find_instance(&gs->anim, entity_index);
        if (!inst) return 0;
        asset = find_scene_model_asset(gs, inst->model_asset_index);
        if (!asset || !asset->loaded) return 0;
        if ((uint32_t)clip_index >= asset->model.clip_count) return 0;
        return anim_time >= asset->model.clips[clip_index].duration;
    }
    case ANIM_COND_INPUT_BUTTON:
        return (gs->input.input_mask & (int)rule->threshold) != 0;
    case ANIM_COND_ALWAYS:
        return 1;
    }
    return 0;
}

static void anim_sm_update(anim_sm *sm, game_state *gs, float dt) {
    int s, r, i, j, b;
    anim_pose_pool *pool;

    if (!sm->pool_initialized || sm->instance_count == 0) return;
    pool = &sm->pool;

    /* ── Phase 1: Advance anim_times per state table ──────────── */
    for (s = 0; s < sm->state_count; s++) {
        anim_state_table *st = &sm->states[s];
        for (i = 0; i < st->count; i++) {
            anim_instance *inst = anim_sm_find_instance(sm, st->entity_indices[i]);
            float speed = (inst && inst->speed > 0.0f) ? inst->speed : 1.0f;
            st->anim_times[i] += dt * speed;
        }
    }

    /* ── Phase 2: Evaluate rules, move entities to blend table ── */
    for (r = 0; r < sm->rule_count; r++) {
        anim_transition_rule *rule = &sm->rules[r];
        anim_state_table *from_st;
        if (rule->from_state < 0 || rule->from_state >= sm->state_count) continue;
        if (rule->to_state < 0 || rule->to_state >= sm->state_count) continue;
        from_st = &sm->states[rule->from_state];

        for (i = from_st->count - 1; i >= 0; i--) {
            int eidx = from_st->entity_indices[i];
            float atime = from_st->anim_times[i];
            anim_instance *inst;
            anim_blend_entry *be;
            int already_blending = 0;

            if (!anim_sm_eval_condition(gs, rule, eidx, atime,
                                        from_st->clip_index))
                continue;

            /* Skip if already blending */
            for (b = 0; b < sm->blend_count; b++) {
                if (sm->blends[b].entity_index == eidx) {
                    already_blending = 1;
                    break;
                }
            }
            if (already_blending) continue;
            if (sm->blend_count >= ANIM_SM_MAX_BLENDS) continue;

            inst = anim_sm_find_instance(sm, eidx);

            /* Add blend entry */
            be = &sm->blends[sm->blend_count++];
            be->entity_index = eidx;
            be->from_clip = from_st->clip_index;
            be->to_clip = sm->states[rule->to_state].clip_index;
            be->from_time = atime;
            be->to_time = 0.0f;
            be->elapsed = 0.0f;
            be->duration = rule->blend_duration > 0.0f ? rule->blend_duration : 0.18f;
            be->speed = (inst && inst->speed > 0.0f) ? inst->speed : 1.0f;
            be->to_state = rule->to_state;

            /* Remove from source state table */
            anim_sm_remove_entity_from_state(sm, rule->from_state, eidx);
        }
    }

    /* ── Phase 3: Advance blends, complete finished ones ──────── */
    for (b = sm->blend_count - 1; b >= 0; b--) {
        anim_blend_entry *be = &sm->blends[b];
        be->elapsed += dt;
        be->from_time += dt * be->speed;
        be->to_time += dt * be->speed;

        if (be->elapsed >= be->duration) {
            /* Blend complete: move entity to destination state */
            anim_state_table *to_st;
            if (be->to_state >= 0 && be->to_state < sm->state_count) {
                to_st = &sm->states[be->to_state];
                if (to_st->count < ANIM_SM_MAX_ENTITIES) {
                    to_st->entity_indices[to_st->count] = be->entity_index;
                    to_st->anim_times[to_st->count] = be->to_time;
                    to_st->count++;
                }
            }
            /* Remove blend entry (swap with last) */
            sm->blends[b] = sm->blends[sm->blend_count - 1];
            sm->blend_count--;
        }
    }

    /* ── Phase 4: Sample + skin per state table ───────────────── */
    for (s = 0; s < sm->state_count; s++) {
        anim_state_table *st = &sm->states[s];
        for (i = 0; i < st->count; i++) {
            int eidx = st->entity_indices[i];
            anim_instance *inst = anim_sm_find_instance(sm, eidx);
            scene_model_asset *asset;
            Skeleton *skel;
            AnimClip *clip;
            uint32_t jcount;
            Mat4 *dst;
            float atime;

            if (!inst) continue;
            asset = find_scene_model_asset(gs, inst->model_asset_index);
            if (!asset || !asset->loaded || !asset->has_skeleton) continue;
            skel = &asset->model.skeleton;
            jcount = skel->joint_count;
            if (jcount == 0 || jcount > ANIM_SM_MAX_JOINTS) continue;
            if ((uint32_t)st->clip_index >= asset->model.clip_count) continue;
            clip = &asset->model.clips[st->clip_index];

            /* Wrap anim time */
            atime = st->anim_times[i];
            if (st->looping && clip->duration > 0.0f) {
                while (atime > clip->duration) atime -= clip->duration;
                while (atime < 0.0f) atime += clip->duration;
                st->anim_times[i] = atime;
            }

            dst = pool->skin_mats + inst->skin_mats_offset;

            init_pose_from_bind(skel, pool->pose_trans, pool->pose_rot, pool->pose_scale);
            sample_clip(clip, atime, pool->pose_trans, pool->pose_rot, pool->pose_scale, jcount);
            compute_world_transforms(skel, pool->pose_trans, pool->pose_rot, pool->pose_scale, pool->world_mats);
            compute_skinning_matrices(skel, pool->world_mats, dst);
        }
    }

    /* ── Phase 5: Sample + skin blending entities ─────────────── */
    for (b = 0; b < sm->blend_count; b++) {
        anim_blend_entry *be = &sm->blends[b];
        anim_instance *inst = anim_sm_find_instance(sm, be->entity_index);
        scene_model_asset *asset;
        Skeleton *skel;
        AnimClip *from_clip, *to_clip;
        uint32_t jcount;
        Mat4 *dst;
        float alpha, from_time, to_time;

        if (!inst) continue;
        asset = find_scene_model_asset(gs, inst->model_asset_index);
        if (!asset || !asset->loaded || !asset->has_skeleton) continue;
        skel = &asset->model.skeleton;
        jcount = skel->joint_count;
        if (jcount == 0 || jcount > ANIM_SM_MAX_JOINTS) continue;
        if ((uint32_t)be->from_clip >= asset->model.clip_count) continue;
        if ((uint32_t)be->to_clip >= asset->model.clip_count) continue;

        from_clip = &asset->model.clips[be->from_clip];
        to_clip = &asset->model.clips[be->to_clip];

        from_time = be->from_time;
        if (from_clip->duration > 0.0f) {
            while (from_time > from_clip->duration) from_time -= from_clip->duration;
            while (from_time < 0.0f) from_time += from_clip->duration;
        }
        to_time = be->to_time;
        if (to_clip->duration > 0.0f) {
            while (to_time > to_clip->duration) to_time -= to_clip->duration;
            while (to_time < 0.0f) to_time += to_clip->duration;
        }

        alpha = be->elapsed / be->duration;
        if (alpha < 0.0f) alpha = 0.0f;
        if (alpha > 1.0f) alpha = 1.0f;

        /* Sample from-clip */
        init_pose_from_bind(skel, pool->blend_from_trans, pool->blend_from_rot, pool->blend_from_scale);
        sample_clip(from_clip, from_time,
                    pool->blend_from_trans, pool->blend_from_rot, pool->blend_from_scale, jcount);

        /* Sample to-clip */
        init_pose_from_bind(skel, pool->blend_to_trans, pool->blend_to_rot, pool->blend_to_scale);
        sample_clip(to_clip, to_time,
                    pool->blend_to_trans, pool->blend_to_rot, pool->blend_to_scale, jcount);

        /* Blend per joint */
        for (j = 0; j < (int)jcount; j++) {
            pool->pose_trans[j] = vec3_lerp(pool->blend_from_trans[j], pool->blend_to_trans[j], alpha);
            pool->pose_rot[j] = quat_slerp(pool->blend_from_rot[j], pool->blend_to_rot[j], alpha);
            pool->pose_scale[j] = vec3_lerp(pool->blend_from_scale[j], pool->blend_to_scale[j], alpha);
        }

        dst = pool->skin_mats + inst->skin_mats_offset;
        compute_world_transforms(skel, pool->pose_trans, pool->pose_rot, pool->pose_scale, pool->world_mats);
        compute_skinning_matrices(skel, pool->world_mats, dst);
    }
}

/* Profiler zone function pointers — set by core.dll, call into externals.dll */
static void (*pfn_cache_zone_begin)(const char *name) = NULL;
static void (*pfn_cache_zone_end)(void) = NULL;
static void (*pfn_cpu_zone_begin)(const char *name) = NULL;
static void (*pfn_cpu_zone_end)(void) = NULL;

EXPORT void assign_profiler_fns(
    void (*czb)(const char *), void (*cze)(void),
    void (*cpzb)(const char *), void (*cpze)(void))
{
    pfn_cache_zone_begin = czb;
    pfn_cache_zone_end = cze;
    pfn_cpu_zone_begin = cpzb;
    pfn_cpu_zone_end = cpze;
}

static void swap_and_pop_trigger(game_state *gs, int index) {
    int last = gs->trigger_component_count - 1;
    int removed_entity = gs->trigger_components[index].entity_index;
    if (index != last) {
        gs->trigger_components[index] = gs->trigger_components[last];
        gs->trigger_index[gs->trigger_components[index].entity_index] = index;
    }
    gs->trigger_index[removed_entity] = -1;
    gs->trigger_component_count = last;
}

static void update_triggers(game_state *gs) {
    static int trigger_log_frames = 0;
    int i, player_entity;
    Vec3 player_pos;
    if (!gs || gs->trigger_component_count <= 0) return;

    /* Find player entity (first character controller) */
    if (gs->character_controller_component_count <= 0) return;
    player_entity = gs->character_controller_components[0].entity_index;
    player_pos = resolve_world_position(gs, player_entity);

    if (trigger_log_frames < 3) {
        fprintf(stderr, "[triggers] frame %d: %d triggers, player at (%.2f, %.2f, %.2f)\n",
                trigger_log_frames, gs->trigger_component_count,
                player_pos.x, player_pos.y, player_pos.z);
        for (i = 0; i < gs->trigger_component_count; i++) {
            trigger_component *t = &gs->trigger_components[i];
            Vec3 tp = resolve_world_position(gs, t->entity_index);
            fprintf(stderr, "  [%d] entity=%d type=%d radius=%.1f pos=(%.2f,%.2f,%.2f) joint='%s'\n",
                    i, t->entity_index, (int)t->type, t->radius,
                    tp.x, tp.y, tp.z, t->joint_name);
        }
        fflush(stderr);
        trigger_log_frames++;
    }

    for (i = 0; i < gs->trigger_component_count; ) {
        trigger_component *trig = &gs->trigger_components[i];
        Vec3 trig_pos;
        float dx, dz, dist_sq, r_sq;
        mesh_component *mc;
        int door_trig_idx;
        mesh_component *door_mc;
        box_collider_component *door_bc;

        if (trig->type == TRIGGER_WEAPON_PICKUP) {
            mesh_component *target_mc;
            scene_model_asset *target_asset;
            int joint_idx;
            Vec3 wp;
            float wdx, wdz, wdist_sq, wr_sq;

            wp = resolve_world_position(gs, trig->entity_index);
            wdx = player_pos.x - wp.x;
            wdz = player_pos.z - wp.z;
            wdist_sq = wdx * wdx + wdz * wdz;
            wr_sq = trig->radius * trig->radius;
            if (wdist_sq >= wr_sq) { i++; continue; }

            target_mc = find_mesh_component(gs, trig->target_entity);
            target_asset = target_mc ? find_scene_model_asset(gs, target_mc->model_asset_index) : NULL;
            joint_idx = -1;
            if (target_asset && target_asset->has_skeleton) {
                joint_idx = find_joint_by_name(&target_asset->model.skeleton, trig->joint_name);
            }

            if (joint_idx >= 0) {
                push_bone_attach_component(gs, trig->entity_index, trig->target_entity,
                    joint_idx, VEC3(0,0,0), QUAT(0,0,0,1));
                fprintf(stderr, "Weapon pickup: entity %d attached to joint %d (%s) of entity %d\n",
                        trig->entity_index, joint_idx, trig->joint_name, trig->target_entity);
            } else {
                fprintf(stderr, "Weapon pickup: could not find joint '%s' on entity %d\n",
                        trig->joint_name, trig->target_entity);
            }

            swap_and_pop_trigger(gs, i);
            continue;
        }

        /* TRIGGER_ZONE: fires when any CC entity enters radius; stays in list */
        if (trig->type == TRIGGER_ZONE) {
            int j;
            if (!trig->activated) {
                trig_pos = resolve_world_position(gs, trig->entity_index);
                for (j = 0; j < gs->character_controller_component_count; j++) {
                    int ce = gs->character_controller_components[j].entity_index;
                    Vec3 ce_pos = resolve_world_position(gs, ce);
                    dx = ce_pos.x - trig_pos.x;
                    dz = ce_pos.z - trig_pos.z;
                    if (dx * dx + dz * dz < trig->radius * trig->radius) {
                        trig->activated = 1;
                        trig->activated_by_entity = ce;
                        break;
                    }
                }
            }
            i++;
            continue;
        }

        if (trig->type != TRIGGER_PICKUP) { i++; continue; }

        trig_pos = resolve_world_position(gs, trig->entity_index);
        dx = player_pos.x - trig_pos.x;
        dz = player_pos.z - trig_pos.z;
        dist_sq = dx * dx + dz * dz;
        r_sq = trig->radius * trig->radius;
        if (dist_sq >= r_sq) { i++; continue; }

        /* Activate key: hide mesh */
        trig->activated = 1;
        mc = find_mesh_component(gs, trig->entity_index);
        if (mc) mc->visible = 0;

        /* Activate door: hide mesh, disable collider */
        door_trig_idx = (trig->target_entity >= 0 && trig->target_entity < PROJECT_COMP_MAX)
                       ? gs->trigger_index[trig->target_entity] : -1;

        door_mc = find_mesh_component(gs, trig->target_entity);
        if (door_mc) door_mc->visible = 0;

        door_bc = find_box_collider_component(gs, trig->target_entity);
        if (door_bc) {
            door_bc->rect.w = 0.0f;
            door_bc->rect.h = 0.0f;
        }

        /* Remove activated door trigger first (if it has a higher index) */
        if (door_trig_idx >= 0) {
            gs->trigger_components[door_trig_idx].activated = 1;
            if (door_trig_idx > i) {
                swap_and_pop_trigger(gs, door_trig_idx);
            }
            /* If door_trig_idx < i, it will be removed when we reach it,
               but since door triggers have type DOOR they skip the PICKUP check.
               Mark it and remove after the key trigger. */
        }

        /* Remove the key trigger (current slot) */
        swap_and_pop_trigger(gs, i);
        /* Don't increment i — re-examine the swapped element */

        /* Now remove door trigger if it was at a lower index */
        if (door_trig_idx >= 0 && door_trig_idx < i) {
            swap_and_pop_trigger(gs, door_trig_idx);
            /* Adjust i since an element before us was removed */
            if (i > 0) i--;
        }
    }
}

#define ENGINE_CACHE_ZONE_BEGIN(name) do { if (pfn_cache_zone_begin) pfn_cache_zone_begin(name); } while(0)
#define ENGINE_CACHE_ZONE_END()      do { if (pfn_cache_zone_end) pfn_cache_zone_end(); } while(0)
#define ENGINE_CPU_ZONE_BEGIN(name)  do { if (pfn_cpu_zone_begin) pfn_cpu_zone_begin(name); } while(0)
#define ENGINE_CPU_ZONE_END()        do { if (pfn_cpu_zone_end) pfn_cpu_zone_end(); } while(0)

/* ── System functions (extracted from update_engine) ──────────────────── */

static void system_clear_draw_lists(game_state *gs) {
    gs->dl.sprite_count = 0;
    gs->dl.line_count = 0;
    gs->dl.mesh_count = 0;
    gs->dbg.current_line_count = 0;
    memset(gs->entity_visible, 0, sizeof(gs->entity_visible));
}

static void system_animation_sm(game_state *gs) {
    if (gs->anim.instance_count > 0) {
        anim_sm_update(&gs->anim, gs, gs->dt);
    }
}

static void system_mesh_sync(game_state *gs) {
    sync_primary_mesh3d_asset(gs);
    sync_mesh_camera_from_components(gs);
    build_mesh_draw_commands(gs);
}

static void system_animation_legacy(game_state *gs) {
    if (gs->editor_play_mode) {
        if (gs->anim.instance_count == 0) {
            int anim_updated = 0;
            if (gs->animation_components) {
                animation_component *ac = query_primary_animation_component_for_mesh(gs);
                if (ac) {
                    update_mesh3d_from_animation_component(gs, ac);
                    anim_updated = 1;
                }
            }
            if (!anim_updated) {
                animation_component fallback = {0, 1, (int)gs->mesh3d.active_clip, gs->mesh3d.anim_time, 1.0f};
                update_mesh3d_from_animation_component(gs, &fallback);
            }
        }
    } else {
        gs->animation_transition_count = 0;
    }
}

static void system_flush_debug_lines(game_state *gs) {
    int count = gs->dbg.current_line_count;
    int avail = gs->dl.line_capacity - gs->dl.line_count;
    if (count > avail) count = avail;
    if (count <= 0) return;
    memcpy(&gs->dl.line_verts[gs->dl.line_count * LINE_VERTS_PER_LINE],
           gs->dbg.vertex_buffer,
           (size_t)(count * LINE_VERTS_PER_LINE) * sizeof(line_vert));
    gs->dl.line_count += count;
}

static void register_system(game_state *gs, const char *name,
                            system_fn fn, int play_mode_only) {
    engine_system *sys;
    if (gs->system_count >= ENGINE_MAX_SYSTEMS) return;
    sys = &gs->systems[gs->system_count++];
    sys->name = name;
    sys->fn = fn;
    sys->play_mode_only = play_mode_only;
}

EXPORT void update_engine(game_state *gs) {
    static int update_count = 0;
    int i;
    if (!gs) return;

    if (update_count < 3) {
        fprintf(stderr, "[update_engine] frame %d, %d systems, play_mode=%d\n",
                update_count, gs->system_count, gs->editor_play_mode);
        fflush(stderr);
    }

    ENGINE_CPU_ZONE_BEGIN("update_engine");
    ENGINE_CACHE_ZONE_BEGIN("update_engine");

    for (i = 0; i < gs->system_count; i++) {
        engine_system *sys = &gs->systems[i];
        if (sys->play_mode_only && !gs->editor_play_mode) continue;
        if (update_count < 3) {
            fprintf(stderr, "[update_engine]   running system '%s'\n", sys->name);
            fflush(stderr);
        }
        ENGINE_CPU_ZONE_BEGIN(sys->name);
        sys->fn(gs);
        ENGINE_CPU_ZONE_END();
    }
    update_count++;

    ENGINE_CACHE_ZONE_END();
    ENGINE_CPU_ZONE_END();
}

EXPORT void destroy_engine(game_state *gs) {
    (void)gs;
}
