#include <game.h>
#include <export.h>
#include <scene.h>
#include <stdio.h>
#include "renderer.h"
#include <physics.h>
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

void update_input(memory* g) {
    entity* player;
    const float move_speed = 200.0f;
    int attack_pressed;
    const float camera_speed = 300.0f;
    const float zoom_speed = 2.0f;
    if (!g || scene.entity_count == 0) return;

    player = &scene.entities[0];

    player->velocity.x = g->input.horizontal * move_speed;
    player->velocity.y = g->input.vertical * move_speed;

    attack_pressed = (g->input.input_mask & INPUT_A);
    if (attack_pressed) {
        vec2 a1 = {0,0}, b1 = {10,20};
        vec2 a2 = {10,10}, b2 = {100,200};
        debug_draw_line(&g->debug_renderer, a1, b1, DEBUG_GREEN);
        debug_draw_line(&g->debug_renderer, a2, b2, DEBUG_BLUE);
    }

    if (g->input.input_mask & INPUT_X) {
        g->camera.position.x += g->input.horizontal * camera_speed * g->dt;
        g->camera.position.y += g->input.vertical * camera_speed * g->dt;
    }
    if (g->input.input_mask & INPUT_Y) {
        float zoom_delta = g->input.vertical * zoom_speed * g->dt;
        g->camera.zoom = fmaxf(0.1f, fminf(5.0f, g->camera.zoom + zoom_delta));
    }
}

EXPORT void init_engine(memory *g) {
    /* Allocate entity storage from the gameplay sub-arena (only on first init, survives hot reload) */
    if (!scene.entities) {
        scene.entity_capacity = 64;
        scene.entities = (entity *)arena_alloc(g->gameplay,
            (uint32_t)(scene.entity_capacity * sizeof(entity)), 16, "entities");
    }
    scene_init();

    /* Load 3D model assets.
       Only runs on first init when no model is loaded yet. */
    if (g->loaded_model.mesh.primitive_count == 0 && g->gpu_device) {
        arena *model_arena = arena_alloc_subarena(&g->arena, 2 * 1024 * 1024, 16, "gltf_models");
        if (model_arena) {
            uint32_t jc;
            gltf_set_gpu_device(g->gpu_device);

            g->loaded_model = load_glb(
                "C:/Users/andres/Downloads/KayKit_Adventurers_2.0_FREE/Characters/gltf/Knight.glb",
                model_arena);

            if (g->loaded_model.mesh.primitive_count > 0) {
                if (g->loaded_model.clip_count == 0) {
                    load_animations_glb(
                        "C:/Users/andres/Downloads/KayKit_Adventurers_2.0_FREE/Animations/gltf/Rig_Medium/Rig_Medium_General.glb",
                        &g->loaded_model, model_arena);
                }

                /* Populate g->mesh3d so engine can drive animation, externals can render */
                jc = g->loaded_model.skeleton.joint_count;
                g->mesh3d.skeleton        = g->loaded_model.skeleton;
                g->mesh3d.clips           = g->loaded_model.clips;
                g->mesh3d.clip_count      = g->loaded_model.clip_count;
                g->mesh3d.primitive_count  = g->loaded_model.mesh.primitive_count;

                g->mesh3d.pose_trans  = (Vec3*)arena_alloc(model_arena, jc * sizeof(Vec3), 16, "pose_trans");
                g->mesh3d.pose_rot    = (Quat*)arena_alloc(model_arena, jc * sizeof(Quat), 16, "pose_rot");
                g->mesh3d.pose_scale  = (Vec3*)arena_alloc(model_arena, jc * sizeof(Vec3), 16, "pose_scale");
                g->mesh3d.world_mats  = (Mat4*)arena_alloc(model_arena, jc * sizeof(Mat4), 16, "world_mats");
                g->mesh3d.skin_mats   = (Mat4*)arena_alloc(model_arena, jc * sizeof(Mat4), 16, "skin_mats");

                g->mesh3d.visible = 1;
                g->mesh3d.active_clip = g->loaded_model.clip_count > 6 ? 6 : 0;
                g->mesh3d.anim_time = 0.0f;
                g->mesh3d.camera_eye    = VEC3(0.0f, 1.0f, 3.0f);
                g->mesh3d.camera_target = VEC3(0.0f, 0.5f, 0.0f);
                g->mesh3d.camera_up     = VEC3(0.0f, 1.0f, 0.0f);
                g->mesh3d.model_transform = g->loaded_model.armature_transform;
            } else {
                fprintf(stderr, "Warning: Knight.glb loaded but has no primitives\n");
            }
        }
    }

    /* Compute initial bind-pose skinning matrices so the mesh renders correctly
       even before the first animation frame */
    if (g->mesh3d.skeleton.joint_count > 0 && g->mesh3d.skin_mats) {
        init_pose_from_bind(&g->mesh3d.skeleton,
                            g->mesh3d.pose_trans, g->mesh3d.pose_rot, g->mesh3d.pose_scale);
        compute_world_transforms(&g->mesh3d.skeleton,
                                  g->mesh3d.pose_trans, g->mesh3d.pose_rot, g->mesh3d.pose_scale,
                                  g->mesh3d.world_mats);
        compute_skinning_matrices(&g->mesh3d.skeleton, g->mesh3d.world_mats, g->mesh3d.skin_mats);
    }
}

static void update_mesh3d(memory *g) {
    mesh3d_state *m = &g->mesh3d;
    if (!m->visible || m->skeleton.joint_count == 0 || !m->skin_mats) return;

    if (m->clip_count > 0 && m->clips) {
        uint32_t ci = m->active_clip < m->clip_count ? m->active_clip : 0;
        AnimClip *clip = &m->clips[ci];

        m->anim_time += g->dt;
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

EXPORT void update_engine(memory *g) {
    int i;
    if (!g) return;

    g->draw_list.sprite_count = 0;
    g->draw_list.line_count = 0;
    g->debug_renderer.current_line_count = 0;

    update_input(g);
    update_animation(g);
    render_tiles(g);
    collision(g);
    apply_movement(g);
    update_camera_matrix(&g->camera, g->draw_list.view_matrix);
    render_entities(g);

    /* 3D mesh animation (driven by engine, rendered by externals) */
    update_mesh3d(g);

    /* Copy debug lines from debug_renderer to draw_list */
    for (i = 0; i < g->debug_renderer.current_line_count; i++) {
        int idx;
        debug_line_command* line;
        if (g->draw_list.line_count >= g->draw_list.line_capacity) break;
        idx = i * 10;
        line = &g->draw_list.lines[g->draw_list.line_count++];
        line->x1 = g->debug_renderer.vertex_buffer[idx];
        line->y1 = g->debug_renderer.vertex_buffer[idx+1];
        line->r = g->debug_renderer.vertex_buffer[idx+2];
        line->g = g->debug_renderer.vertex_buffer[idx+3];
        line->b = g->debug_renderer.vertex_buffer[idx+4];
        line->x2 = g->debug_renderer.vertex_buffer[idx+5];
        line->y2 = g->debug_renderer.vertex_buffer[idx+6];
    }
}

EXPORT void destroy_engine(memory *g) {
}
