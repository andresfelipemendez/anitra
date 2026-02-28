#include <game.h>
#include <export.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

/* Animation functions (from anim.c, same DLL) */
void init_pose_from_bind(Skeleton *skel, Vec3 *trans, Quat *rot, Vec3 *scale);
void sample_clip(AnimClip *clip, float time,
                 Vec3 *out_trans, Quat *out_rot, Vec3 *out_scale,
                 uint32_t joint_count);
void compute_world_transforms(Skeleton *skel,
                               Vec3 *trans, Quat *rot, Vec3 *scales,
                               Mat4 *out_world);
void compute_skinning_matrices(Skeleton *skel, Mat4 *world, Mat4 *out_skinning);

/* Asset loading (from gltf_loader.c, same DLL) */
GltfModel load_glb(const char *path, arena *a);
void load_animations_glb(const char *path, GltfModel *model, arena *a);
void gltf_set_gpu_device(void *dev);

void update_input(game_state* gs) {
    int attack_pressed;
    const float camera_speed = 300.0f;
    const float zoom_speed = 2.0f;
    if (!gs) return;

    attack_pressed = (gs->input.input_mask & INPUT_A);
    if (attack_pressed) {
        vec2 a1 = {0,0}, b1 = {10,20};
        vec2 a2 = {10,10}, b2 = {100,200};
        debug_draw_line(&gs->dbg, a1, b1, DEBUG_GREEN);
        debug_draw_line(&gs->dbg, a2, b2, DEBUG_BLUE);
    }

    if (gs->input.input_mask & INPUT_X) {
        gs->camera.position.x += gs->input.horizontal * camera_speed * gs->dt;
        gs->camera.position.y += gs->input.vertical * camera_speed * gs->dt;
    }
    if (gs->input.input_mask & INPUT_Y) {
        float zoom_delta = gs->input.vertical * zoom_speed * gs->dt;
        gs->camera.zoom = fmaxf(0.1f, fminf(5.0f, gs->camera.zoom + zoom_delta));
    }
}

EXPORT void init_engine(game_state *gs) {
    /* ECS storage for editor/runtime scene graph (survives hot reload via game_state). */
    if (gs->scene_entity_capacity <= 0)
        gs->scene_entity_capacity = 64;
    if (!gs->scene_entities) {
        gs->scene_entities = (entity *)arena_alloc(gs->gameplay,
            (uint32_t)(gs->scene_entity_capacity * sizeof(entity)), 16, "entities");
        gs->scene_entity_count = 0;
    }

    if (gs->parent_component_capacity <= 0)
        gs->parent_component_capacity = gs->scene_entity_capacity;
    if (!gs->parent_components) {
        gs->parent_components = (parent_component *)arena_alloc(gs->gameplay,
            (uint32_t)(gs->parent_component_capacity * sizeof(parent_component)),
            16, "parent_components");
        gs->parent_component_count = 0;
    }

    if (gs->parent_transform_component_capacity <= 0)
        gs->parent_transform_component_capacity = gs->scene_entity_capacity;
    if (!gs->parent_transform_components) {
        gs->parent_transform_components = (parent_transform_component *)arena_alloc(gs->gameplay,
            (uint32_t)(gs->parent_transform_component_capacity * sizeof(parent_transform_component)),
            16, "parent_transform_components");
        gs->parent_transform_component_count = 0;
    }

    if (gs->parent_rotation_component_capacity <= 0)
        gs->parent_rotation_component_capacity = gs->scene_entity_capacity;
    if (!gs->parent_rotation_components) {
        gs->parent_rotation_components = (parent_rotation_component *)arena_alloc(gs->gameplay,
            (uint32_t)(gs->parent_rotation_component_capacity * sizeof(parent_rotation_component)),
            16, "parent_rotation_components");
        gs->parent_rotation_component_count = 0;
    }

    if (gs->mesh_component_capacity <= 0)
        gs->mesh_component_capacity = gs->scene_entity_capacity;
    if (!gs->mesh_components) {
        gs->mesh_components = (mesh_component *)arena_alloc(gs->gameplay,
            (uint32_t)(gs->mesh_component_capacity * sizeof(mesh_component)),
            16, "mesh_components");
        gs->mesh_component_count = 0;
    }

    if (gs->scene_entities && gs->scene_entity_count != 1) {
        memset(gs->scene_entities, 0, (size_t)gs->scene_entity_capacity * sizeof(entity));
        gs->scene_entities[0].type = PLAYER;
        gs->scene_entities[0].health = 100.0f;
        gs->scene_entities[0].pos.x = 0.0f;
        gs->scene_entities[0].pos.y = 0.0f;
        gs->scene_entity_count = 1;
        gs->parent_component_count = 0;
        gs->parent_transform_component_count = 0;
        gs->parent_rotation_component_count = 0;
    }

    if (gs->mesh_components && gs->mesh_component_capacity > 0) {
        if (gs->mesh_component_count <= 0 ||
            gs->mesh_components[0].entity_index != 0) {
            gs->mesh_components[0].entity_index = 0;
            gs->mesh_components[0].visible = 1;
            gs->mesh_component_count = 1;
        }
    }

    /* Load 3D model assets.
       Only runs on first init when no model is loaded yet. */
    if (gs->loaded_model.mesh.primitive_count == 0 && gs->gpu_device) {
        uint32_t jc;
        arena *model_arena = arena_alloc_subarena(gs->root_arena, 2 * 1024 * 1024, 16, "gltf_models");
gltf_set_gpu_device(gs->gpu_device);

            const char *model_path = gs->default_model_path ?
                gs->default_model_path : "assets/models/Knight.glb";
            const char *anim_path = gs->default_animation_path ?
                gs->default_animation_path : "assets/animations/Rig_Medium_General.glb";

            gs->loaded_model = load_glb(model_path, model_arena);

            if (gs->loaded_model.mesh.primitive_count > 0) {
                if (gs->loaded_model.clip_count == 0) {
                    load_animations_glb(anim_path, &gs->loaded_model, model_arena);
                }

                /* Populate gs->mesh3d so engine can drive animation, externals can render */
                jc = gs->loaded_model.skeleton.joint_count;
                gs->mesh3d.skeleton        = gs->loaded_model.skeleton;
                gs->mesh3d.clips           = gs->loaded_model.clips;
                gs->mesh3d.clip_count      = gs->loaded_model.clip_count;
                gs->mesh3d.primitive_count  = gs->loaded_model.mesh.primitive_count;

                gs->mesh3d.pose_trans  = (Vec3*)arena_alloc(model_arena, jc * sizeof(Vec3), 16, "pose_trans");
                gs->mesh3d.pose_rot    = (Quat*)arena_alloc(model_arena, jc * sizeof(Quat), 16, "pose_rot");
                gs->mesh3d.pose_scale  = (Vec3*)arena_alloc(model_arena, jc * sizeof(Vec3), 16, "pose_scale");
                gs->mesh3d.world_mats  = (Mat4*)arena_alloc(model_arena, jc * sizeof(Mat4), 16, "world_mats");
                gs->mesh3d.skin_mats   = (Mat4*)arena_alloc(model_arena, jc * sizeof(Mat4), 16, "skin_mats");

                gs->mesh3d.visible = 1;
                gs->mesh3d.active_clip = gs->loaded_model.clip_count > 6 ? 6 : 0;
                gs->mesh3d.anim_time = 0.0f;
                if (!gs->mesh3d.camera_set_by_project) {
                    gs->mesh3d.camera_eye    = VEC3(0.0f, 1.0f, 3.0f);
                    gs->mesh3d.camera_target = VEC3(0.0f, 0.5f, 0.0f);
                    gs->mesh3d.camera_up     = VEC3(0.0f, 1.0f, 0.0f);
                }
                gs->mesh3d.model_transform = gs->loaded_model.armature_transform;
            } else {
                fprintf(stderr, "Warning: Knight.glb loaded but has no primitives\n");
            }
    }

    /* Compute initial bind-pose skinning matrices so the mesh renders correctly
       even before the first animation frame */
    if (gs->mesh3d.skeleton.joint_count > 0 && gs->mesh3d.skin_mats) {
        init_pose_from_bind(&gs->mesh3d.skeleton,
                            gs->mesh3d.pose_trans, gs->mesh3d.pose_rot, gs->mesh3d.pose_scale);
        compute_world_transforms(&gs->mesh3d.skeleton,
                                  gs->mesh3d.pose_trans, gs->mesh3d.pose_rot, gs->mesh3d.pose_scale,
                                  gs->mesh3d.world_mats);
        compute_skinning_matrices(&gs->mesh3d.skeleton, gs->mesh3d.world_mats, gs->mesh3d.skin_mats);
    }
}

static void update_mesh3d(game_state *gs) {
    mesh3d_state *m = &gs->mesh3d;
    if (!m->visible || m->skeleton.joint_count == 0 || !m->skin_mats) return;

    if (m->clip_count > 0 && m->clips) {
        uint32_t ci = m->active_clip < m->clip_count ? m->active_clip : 0;
        AnimClip *clip = &m->clips[ci];

        m->anim_time += gs->dt;
        if (clip->duration > 0.0f && m->anim_time > clip->duration)
            m->anim_time -= clip->duration;

        init_pose_from_bind(&m->skeleton, m->pose_trans, m->pose_rot, m->pose_scale);
        sample_clip(clip, m->anim_time, m->pose_trans, m->pose_rot, m->pose_scale,
                    m->skeleton.joint_count);
        compute_world_transforms(&m->skeleton, m->pose_trans, m->pose_rot, m->pose_scale,
                                  m->world_mats);
        compute_skinning_matrices(&m->skeleton, m->world_mats, m->skin_mats);
    }
}

EXPORT void update_engine(game_state *gs) {
    int i;
    int mi;
    int mesh_visible_set = 0;
    if (!gs) return;

    gs->dl.sprite_count = 0;
    gs->dl.line_count = 0;
    gs->dbg.current_line_count = 0;

    update_input(gs);
    /* Legacy 2D scene systems removed: runtime rendering is mesh-component-driven. */

    /* Drive runtime mesh visibility from ECS mesh component (entity 0 for now). */
    if (gs->mesh_components) {
        for (mi = 0; mi < gs->mesh_component_count; mi++) {
            mesh_component *mc = &gs->mesh_components[mi];
            if (mc->entity_index == 0) {
                gs->mesh3d.visible = mc->visible ? 1 : 0;
                mesh_visible_set = 1;
                break;
            }
        }
    }
    if (!mesh_visible_set && gs->mesh3d.primitive_count > 0) {
        gs->mesh3d.visible = 1;
    }

    /* 3D mesh animation (driven by engine, rendered by externals) */
    update_mesh3d(gs);

    /* Copy debug lines from debug_renderer to draw_list */
    for (i = 0; i < gs->dbg.current_line_count; i++) {
        int idx;
        debug_line_command* line;
        if (gs->dl.line_count >= gs->dl.line_capacity) break;
        idx = i * 10;
        line = &gs->dl.lines[gs->dl.line_count++];
        line->x1 = gs->dbg.vertex_buffer[idx];
        line->y1 = gs->dbg.vertex_buffer[idx+1];
        line->r = gs->dbg.vertex_buffer[idx+2];
        line->g = gs->dbg.vertex_buffer[idx+3];
        line->b = gs->dbg.vertex_buffer[idx+4];
        line->x2 = gs->dbg.vertex_buffer[idx+5];
        line->y2 = gs->dbg.vertex_buffer[idx+6];
    }
}

EXPORT void destroy_engine(game_state *gs) {
    (void)gs;
}
