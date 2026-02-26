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

void update_input(game* g) {
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

EXPORT void init_engine(game *g) {
    /* Allocate entity storage from the gameplay sub-arena (only on first init, survives hot reload) */
    if (!scene.entities) {
        scene.entity_capacity = 64;
        scene.entities = (entity *)arena_alloc(g->gameplay,
            (uint32_t)(scene.entity_capacity * sizeof(entity)), 16, "entities");
    }
    scene_init();

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

static void update_mesh3d(game *g) {
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

EXPORT void update_engine(game *g) {
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

EXPORT void destroy_engine(game *g) {
}
