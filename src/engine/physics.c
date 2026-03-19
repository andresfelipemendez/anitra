#include "physics.h"
#include "game.h"
#include "debug_render.h"

#define PHYSICS_QUERY_MAX_RESULTS 256

typedef struct physics_actor_view {
    int entity_index;
    entity *ent;
    transform_component *transform;
    velocity_component *velocity;
    rigid_body_component *rigid_body;
    health_component *health;
    box_collider_component *box_collider;
    capsule_collider_component *capsule_collider;
} physics_actor_view;

typedef struct collider_target_view {
    int entity_index;
    entity *ent;
    transform_component *transform;
    health_component *health;
    box_collider_component *box_collider;
    capsule_collider_component *capsule_collider;
} collider_target_view;

static transform_component *find_transform_component(game_state *gs, int entity_index) {
    int idx;
    if (!gs || entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return NULL;
    idx = gs->transform_index[entity_index];
    return idx >= 0 ? &gs->transform_components[idx] : NULL;
}

static parent_transform_component *find_parent_transform_component(game_state *gs, int entity_index) {
    int idx;
    if (!gs || entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return NULL;
    idx = gs->parent_transform_index[entity_index];
    return idx >= 0 ? &gs->parent_transform_components[idx] : NULL;
}

static health_component *find_health_component(game_state *gs, int entity_index) {
    int idx;
    if (!gs || entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return NULL;
    idx = gs->health_index[entity_index];
    return idx >= 0 ? &gs->health_components[idx] : NULL;
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

static camera_component *find_camera_component(game_state *gs, int entity_index) {
    int idx;
    if (!gs || entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return NULL;
    idx = gs->camera_index[entity_index];
    return idx >= 0 ? &gs->camera_components[idx] : NULL;
}

static mesh_component *find_mesh_component(game_state *gs, int entity_index) {
    int idx;
    if (!gs || entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return NULL;
    idx = gs->mesh_index[entity_index];
    return idx >= 0 ? &gs->mesh_components[idx] : NULL;
}

static rotation_component *find_rotation_component(game_state *gs, int entity_index) {
    int idx;
    if (!gs || entity_index < 0 || entity_index >= PROJECT_COMP_MAX) return NULL;
    idx = gs->rotation_index[entity_index];
    return idx >= 0 ? &gs->rotation_components[idx] : NULL;
}

/* Rotate a point around Y axis by degrees (for parent-relative positioning) */
static Vec3 rotate_y(Vec3 p, float degrees) {
    float rad = degrees * 3.14159265f / 180.0f;
    float c = cosf(rad), s = sinf(rad);
    return VEC3(p.x * c + p.z * s, p.y, -p.x * s + p.z * c);
}

static Vec3 resolve_world_position(game_state *gs, int entity_index) {
    /* Walk the parent chain collecting (position, rotation_y) pairs,
       then compose root-to-leaf so child positions are rotated by parent rotation. */
    int chain[64];
    int chain_count = 0;
    int current = entity_index;
    int guard = 0;
    Vec3 world = VEC3(0.0f, 0.0f, 0.0f);
    float accumulated_rot_y = 0.0f;
    int i;
    if (!gs) return world;

    /* Collect chain from entity up to root */
    while (guard < 64 && current >= 0 && current < gs->scene_entity_count) {
        parent_transform_component *pt;
        chain[chain_count++] = current;
        pt = find_parent_transform_component(gs, current);
        if (!pt) break;
        if (pt->parent_entity_index == current) break;
        current = pt->parent_entity_index;
        guard++;
    }

    /* Compose root-to-leaf: each child position is rotated by accumulated parent rotation */
    for (i = chain_count - 1; i >= 0; i--) {
        transform_component *tc = find_transform_component(gs, chain[i]);
        rotation_component *rc = find_rotation_component(gs, chain[i]);
        if (tc) {
            Vec3 rotated = rotate_y(tc->position, accumulated_rot_y);
            world = vec3_add(world, rotated);
        }
        if (rc) {
            accumulated_rot_y += rc->rotation_y_deg;
        }
    }
    return world;
}

static Vec3 resolve_parent_world_offset(game_state *gs, int entity_index) {
    transform_component *tc = find_transform_component(gs, entity_index);
    Vec3 world = resolve_world_position(gs, entity_index);
    if (!tc) return world;
    return vec3_sub(world, tc->position);
}

static int query_primary_actor(game_state *gs, physics_actor_view *out_actor) {
    int i;
    if (!gs || !out_actor) return 0;

    for (i = 0; i < gs->character_controller_component_count; i++) {
        character_controller_component *ccmp = &gs->character_controller_components[i];
        int entity_index = ccmp->entity_index;
        transform_component *tc;
        velocity_component *vc;
        rigid_body_component *rb;
        box_collider_component *bc;
        capsule_collider_component *cc;

        if (entity_index < 0 || entity_index >= gs->scene_entity_count) continue;

        tc = find_transform_component(gs, entity_index);
        vc = find_velocity_component(gs, entity_index);
        rb = find_rigid_body_component(gs, entity_index);
        bc = find_box_collider_component(gs, entity_index);
        cc = find_capsule_collider_component(gs, entity_index);
        if (!tc || !vc || (!bc && !cc)) continue;

        out_actor->entity_index = entity_index;
        out_actor->ent = &gs->scene_entities[entity_index];
        out_actor->transform = tc;
        out_actor->velocity = vc;
        out_actor->rigid_body = rb;
        out_actor->health = find_health_component(gs, entity_index);
        out_actor->box_collider = bc;
        out_actor->capsule_collider = cc;
        return 1;
    }

    for (i = 0; i < gs->velocity_component_count; i++) {
        velocity_component *vc = &gs->velocity_components[i];
        int entity_index = vc->entity_index;
        transform_component *tc;
        rigid_body_component *rb;
        box_collider_component *bc;
        capsule_collider_component *cc;

        if (entity_index < 0 || entity_index >= gs->scene_entity_count) continue;

        tc = find_transform_component(gs, entity_index);
        rb = find_rigid_body_component(gs, entity_index);
        bc = find_box_collider_component(gs, entity_index);
        cc = find_capsule_collider_component(gs, entity_index);
        if (!tc || !rb || (!bc && !cc)) continue;

        out_actor->entity_index = entity_index;
        out_actor->ent = &gs->scene_entities[entity_index];
        out_actor->transform = tc;
        out_actor->velocity = vc;
        out_actor->rigid_body = rb;
        out_actor->health = find_health_component(gs, entity_index);
        out_actor->box_collider = bc;
        out_actor->capsule_collider = cc;
        return 1;
    }

    for (i = 0; i < gs->velocity_component_count; i++) {
        velocity_component *vc = &gs->velocity_components[i];
        int entity_index = vc->entity_index;
        transform_component *tc;
        box_collider_component *bc;
        capsule_collider_component *cc;

        if (entity_index < 0 || entity_index >= gs->scene_entity_count) continue;

        tc = find_transform_component(gs, entity_index);
        bc = find_box_collider_component(gs, entity_index);
        cc = find_capsule_collider_component(gs, entity_index);
        if (!tc || (!bc && !cc)) continue;

        out_actor->entity_index = entity_index;
        out_actor->ent = &gs->scene_entities[entity_index];
        out_actor->transform = tc;
        out_actor->velocity = vc;
        out_actor->rigid_body = NULL;
        out_actor->health = find_health_component(gs, entity_index);
        out_actor->box_collider = bc;
        out_actor->capsule_collider = cc;
        return 1;
    }

    return 0;
}

static int query_combat_targets(game_state *gs, int exclude_entity_index,
                                collider_target_view *out_targets, int max_targets) {
    int i;
    int count = 0;
    if (!gs || !out_targets || max_targets <= 0) return 0;

    /* Iterate health_components — combat targets must have health */
    for (i = 0; i < gs->health_component_count && count < max_targets; i++) {
        health_component *hc = &gs->health_components[i];
        int entity_index = hc->entity_index;
        transform_component *tc;
        box_collider_component *bc;
        capsule_collider_component *cc;

        if (entity_index == exclude_entity_index) continue;
        if (entity_index < 0 || entity_index >= gs->scene_entity_count) continue;

        tc = find_transform_component(gs, entity_index);
        if (!tc) continue;

        bc = find_box_collider_component(gs, entity_index);
        cc = find_capsule_collider_component(gs, entity_index);
        if (!bc && !cc) continue;

        out_targets[count].entity_index = entity_index;
        out_targets[count].ent = &gs->scene_entities[entity_index];
        out_targets[count].transform = tc;
        out_targets[count].health = hc;
        out_targets[count].box_collider = bc;
        out_targets[count].capsule_collider = cc;
        count++;
    }

    return count;
}

static int query_movement_obstacles(game_state *gs, int exclude_entity_index,
                                    collider_target_view *out_targets, int max_targets) {
    int i;
    int count = 0;
    if (!gs || !out_targets || max_targets <= 0) return 0;

    /* Pass 1: box colliders */
    for (i = 0; i < gs->box_collider_component_count && count < max_targets; i++) {
        box_collider_component *bc = &gs->box_collider_components[i];
        int entity_index = bc->entity_index;
        transform_component *tc;

        if (entity_index == exclude_entity_index) continue;
        if (entity_index < 0 || entity_index >= gs->scene_entity_count) continue;
        if (find_camera_component(gs, entity_index)) continue;

        tc = find_transform_component(gs, entity_index);
        if (!tc) continue;

        out_targets[count].entity_index = entity_index;
        out_targets[count].ent = &gs->scene_entities[entity_index];
        out_targets[count].transform = tc;
        out_targets[count].health = NULL;
        out_targets[count].box_collider = bc;
        out_targets[count].capsule_collider = NULL;
        count++;
    }

    /* Pass 2: capsule colliders (skip entities already added via box pass) */
    for (i = 0; i < gs->capsule_collider_component_count && count < max_targets; i++) {
        capsule_collider_component *cc = &gs->capsule_collider_components[i];
        int entity_index = cc->entity_index;
        transform_component *tc;

        if (entity_index == exclude_entity_index) continue;
        if (entity_index < 0 || entity_index >= gs->scene_entity_count) continue;
        if (gs->box_collider_index[entity_index] >= 0) continue; /* already in box pass */
        if (find_camera_component(gs, entity_index)) continue;

        tc = find_transform_component(gs, entity_index);
        if (!tc) continue;

        out_targets[count].entity_index = entity_index;
        out_targets[count].ent = &gs->scene_entities[entity_index];
        out_targets[count].transform = tc;
        out_targets[count].health = NULL;
        out_targets[count].box_collider = NULL;
        out_targets[count].capsule_collider = cc;
        count++;
    }

    return count;
}

static rect *collider_rect_ptr(box_collider_component *bc, capsule_collider_component *cc) {
    if (cc) return &cc->aabb;
    if (bc) return &bc->rect;
    return NULL;
}

static void sync_collider_to_pos(box_collider_component *bc, capsule_collider_component *cc, vec2 pos) {
    if (cc) {
        cc->aabb.x = pos.x;
        cc->aabb.y = pos.y;
        cc->aabb.w = cc->radius * 2.0f;
        cc->aabb.h = cc->radius * 2.0f;
        return;
    }
    if (bc) {
        bc->rect.x = pos.x;
        bc->rect.y = pos.y;
    }
}

static int animator_hitbox_active(const animator *a) {
    return a->animation.keyframes[0].frame != -1 &&
           a->frame_index >= a->animation.keyframes[0].frame &&
           a->frame_index <= a->animation.keyframes[1].frame;
}

static rect make_animator_hitbox(Vec3 world_position, const animator *a) {
    rect hit_box = {
        world_position.x + a->animation.collider.x,
        world_position.y + a->animation.collider.y,
        a->animation.collider.w,
        a->animation.collider.h,
    };
    return hit_box;
}

static void draw_center_cross(debug_renderer *dbg, vec2 center, float cross_size, debug_color color) {
    vec2 center_h1 = {center.x - cross_size, center.y};
    vec2 center_h2 = {center.x + cross_size, center.y};
    vec2 center_v1 = {center.x, center.y - cross_size};
    vec2 center_v2 = {center.x, center.y + cross_size};

    debug_draw_line(dbg, center_h1, center_h2, color);
    debug_draw_line(dbg, center_v1, center_v2, color);
}

bool bbox_collide(const rect* a, const rect* b) {
    float a_left = a->x - a->w * 0.5f;
    float a_right = a->x + a->w * 0.5f;
    float a_top = a->y + a->h * 0.5f;
    float a_bottom = a->y - a->h * 0.5f;

    float b_left = b->x - b->w * 0.5f;
    float b_right = b->x + b->w * 0.5f;
    float b_top = b->y + b->h * 0.5f;
    float b_bottom = b->y - b->h * 0.5f;

    return (a_left < b_right && a_right > b_left &&
            a_bottom < b_top && a_top > b_bottom);
}

void collision(game_state* gs) {
    static int collision_dbg = 0;
    physics_actor_view actor;
    const animator* pa;
    int player_hitbox_active;
    rect player_hit_box = {0};
    vec2 player_pos;
    vec2 predicted_player_pos;
    collider_target_view targets[PHYSICS_QUERY_MAX_RESULTS];
    int target_count;
    int ti;
    float cross_size = 8.0f;
    Vec3 actor_world;
    rect *actor_rect;
    if (!gs || gs->scene_entity_count <= 0) return;
    if (collision_dbg < 3) { fprintf(stderr, "[collision] enter, entities=%d\n", gs->scene_entity_count); fflush(stderr); }

    if (!query_primary_actor(gs, &actor)) { if (collision_dbg < 3) { fprintf(stderr, "[collision] no actor\n"); fflush(stderr); } collision_dbg++; return; }
    if (!actor.health) { if (collision_dbg < 3) { fprintf(stderr, "[collision] no health\n"); fflush(stderr); } collision_dbg++; return; }
    pa = &actor.ent->current_animation;
    actor_world = resolve_world_position(gs, actor.entity_index);
    actor_rect = collider_rect_ptr(actor.box_collider, actor.capsule_collider);
    if (!actor_rect) { if (collision_dbg < 3) { fprintf(stderr, "[collision] no actor_rect\n"); fflush(stderr); } collision_dbg++; return; }
    if (collision_dbg < 3) { fprintf(stderr, "[collision] actor ok, querying targets...\n"); fflush(stderr); }

    predicted_player_pos.x = actor_world.x + (actor.velocity->velocity.x * gs->dt);
    predicted_player_pos.y = actor_world.z + (actor.velocity->velocity.z * gs->dt);
    sync_collider_to_pos(actor.box_collider, actor.capsule_collider, predicted_player_pos);

    debug_draw_rect(&gs->dbg, predicted_player_pos,
                    actor_rect->w, actor_rect->h, DEBUG_BLUE);

    player_pos.x = actor_world.x;
    player_pos.y = actor_world.z;
    draw_center_cross(&gs->dbg, player_pos, cross_size, DEBUG_RED);

    player_hitbox_active = animator_hitbox_active(pa);
    if (player_hitbox_active) {
        vec2 player_hit_center;
        player_hit_box = make_animator_hitbox(actor_world, pa);
        player_hit_center.x = player_hit_box.x;
        player_hit_center.y = player_hit_box.y;
        debug_draw_rect(&gs->dbg, player_hit_center,
                        player_hit_box.w, player_hit_box.h, DEBUG_YELLOW);
    }

    target_count = query_combat_targets(gs, actor.entity_index, targets, PHYSICS_QUERY_MAX_RESULTS);
    if (collision_dbg < 3) { fprintf(stderr, "[collision] %d targets\n", target_count); fflush(stderr); }
    for (ti = 0; ti < target_count; ti++) {
        collider_target_view *target = &targets[ti];
        animator* a = &target->ent->current_animation;
        vec2 enemy_pos;
        debug_color color = DEBUG_GREEN;
        Vec3 target_world = resolve_world_position(gs, target->entity_index);
        rect *target_rect = collider_rect_ptr(target->box_collider, target->capsule_collider);

        if (!target_rect) continue;

        sync_collider_to_pos(target->box_collider, target->capsule_collider,
                             (vec2){target_world.x, target_world.z});

        if (player_hitbox_active && bbox_collide(&player_hit_box, target_rect)) {
            color = DEBUG_RED;
            /* Apply damage once per hitbox activation (not per-frame) */
            if (!target->health->hit_this_activation) {
                target->health->health -= 5.0f;
                if (target->health->health < 0.0f) target->health->health = 0.0f;
                target->health->hit_this_activation = 1;
            }
        } else {
            if (target->health) target->health->hit_this_activation = 0;
        }

        if (bbox_collide(actor_rect, target_rect)) {
            color = DEBUG_RED;
            if (!actor.health->hit_this_activation) {
                actor.health->health -= 5.0f;
                if (actor.health->health < 0.0f) actor.health->health = 0.0f;
                actor.health->hit_this_activation = 1;
            }
        } else {
            actor.health->hit_this_activation = 0;
        }

        if (animator_hitbox_active(a)) {
            rect enemy_hit_box = make_animator_hitbox(target_world, a);
            vec2 enemy_hit_center;
            enemy_hit_center.x = enemy_hit_box.x;
            enemy_hit_center.y = enemy_hit_box.y;
            debug_draw_rect(&gs->dbg, enemy_hit_center,
                            enemy_hit_box.w, enemy_hit_box.h, DEBUG_YELLOW);
        }

        enemy_pos.x = target_rect->x;
        enemy_pos.y = target_rect->y;
        debug_draw_rect(&gs->dbg, enemy_pos,
                        target_rect->w, target_rect->h, color);

        enemy_pos.x = target_world.x;
        enemy_pos.y = target_world.z;
        draw_center_cross(&gs->dbg, enemy_pos, cross_size, DEBUG_RED);
    }

    sync_collider_to_pos(actor.box_collider, actor.capsule_collider,
                         (vec2){actor_world.x, actor_world.z});
    if (collision_dbg < 3) { fprintf(stderr, "[collision] done\n"); fflush(stderr); }
    collision_dbg++;
}

static float collider_half_height(box_collider_component *bc, capsule_collider_component *cc) {
    if (cc) return cc->half_height + cc->radius;
    if (bc) return bc->half_height;
    return 0.5f;
}

static int y_ranges_overlap(float y1, float hh1, float y2, float hh2) {
    return (y1 - hh1 < y2 + hh2) && (y1 + hh1 > y2 - hh2);
}

/* For horizontal collision: ignore obstacles the actor can step onto.
   Returns 0 if the actor's feet are above (obstacle_top - step_up). */
static int y_ranges_overlap_horizontal(float y1, float hh1, float y2, float hh2) {
    float step_up = 0.3f;
    float actor_bottom = y1 - hh1;
    float ob_top = y2 + hh2;
    if (actor_bottom >= ob_top - step_up) return 0;
    return y_ranges_overlap(y1, hh1, y2, hh2);
}

static void apply_movement_single(game_state *gs, physics_actor_view *actor) {
    collider_target_view obstacles[PHYSICS_QUERY_MAX_RESULTS];
    int obstacle_count;
    int i;
    int vertical_collision = 0;
    Vec3 actor_world;
    Vec3 actor_parent_offset;
    float actor_hh;
    const float gravity = -18.0f;

    actor_world        = resolve_world_position(gs, actor->entity_index);
    actor_parent_offset = resolve_parent_world_offset(gs, actor->entity_index);
    actor_hh           = collider_half_height(actor->box_collider, actor->capsule_collider);

    if (actor->rigid_body && actor->rigid_body->use_gravity)
        actor->velocity->velocity.y += gravity * gs->dt;

    obstacle_count = query_movement_obstacles(gs, actor->entity_index,
                                              obstacles, PHYSICS_QUERY_MAX_RESULTS);

    /* Depenetration — push actor out of any overlapping obstacles */
    {
        vec2 actor_xz = { actor_world.x, actor_world.z };
        sync_collider_to_pos(actor->box_collider, actor->capsule_collider, actor_xz);
        for (i = 0; i < obstacle_count; i++) {
            collider_target_view *ob = &obstacles[i];
            Vec3 ob_world = resolve_world_position(gs, ob->entity_index);
            float ob_hh = collider_half_height(ob->box_collider, ob->capsule_collider);
            rect *ar, *br;
            float overlap_x, overlap_z, push_x, push_z;

            sync_collider_to_pos(ob->box_collider, ob->capsule_collider,
                                 (vec2){ob_world.x, ob_world.z});
            ar = collider_rect_ptr(actor->box_collider, actor->capsule_collider);
            br = collider_rect_ptr(ob->box_collider, ob->capsule_collider);
            if (!ar || !br) continue;
            if (!bbox_collide(ar, br)) continue;
            if (!y_ranges_overlap_horizontal(actor_world.y, actor_hh, ob_world.y, ob_hh)) continue;

            /* Compute overlap on each axis and push along the smaller one */
            overlap_x = (ar->w + br->w) * 0.5f - fabsf(ar->x - br->x);
            overlap_z = (ar->h + br->h) * 0.5f - fabsf(ar->y - br->y);
            if (overlap_x <= 0.0f || overlap_z <= 0.0f) continue;

            if (overlap_x < overlap_z) {
                push_x = (ar->x < br->x) ? -overlap_x : overlap_x;
                actor->transform->position.x += push_x;
                actor_world.x += push_x;
            } else {
                push_z = (ar->y < br->y) ? -overlap_z : overlap_z;
                actor->transform->position.z += push_z;
                actor_world.z += push_z;
            }
            /* Re-sync actor collider after push */
            actor_xz.x = actor_world.x;
            actor_xz.y = actor_world.z;
            sync_collider_to_pos(actor->box_collider, actor->capsule_collider, actor_xz);
        }
    }

    /* Horizontal collision — separate-axis (X then Z) */
    {
        float move_x = actor->velocity->velocity.x * gs->dt;
        float move_z = actor->velocity->velocity.z * gs->dt;

        {
            vec2 test_xz = { actor_world.x + move_x, actor_world.z };
            sync_collider_to_pos(actor->box_collider, actor->capsule_collider, test_xz);
            for (i = 0; i < obstacle_count; i++) {
                collider_target_view *ob = &obstacles[i];
                Vec3 ob_world = resolve_world_position(gs, ob->entity_index);
                float ob_hh = collider_half_height(ob->box_collider, ob->capsule_collider);
                sync_collider_to_pos(ob->box_collider, ob->capsule_collider,
                                     (vec2){ob_world.x, ob_world.z});
                if (bbox_collide(collider_rect_ptr(actor->box_collider, actor->capsule_collider),
                                 collider_rect_ptr(ob->box_collider, ob->capsule_collider)) &&
                    y_ranges_overlap_horizontal(actor_world.y, actor_hh, ob_world.y, ob_hh)) {
                    move_x = 0.0f; break;
                }
            }
        }
        {
            vec2 test_xz = { actor_world.x + move_x, actor_world.z + move_z };
            sync_collider_to_pos(actor->box_collider, actor->capsule_collider, test_xz);
            for (i = 0; i < obstacle_count; i++) {
                collider_target_view *ob = &obstacles[i];
                Vec3 ob_world = resolve_world_position(gs, ob->entity_index);
                float ob_hh = collider_half_height(ob->box_collider, ob->capsule_collider);
                sync_collider_to_pos(ob->box_collider, ob->capsule_collider,
                                     (vec2){ob_world.x, ob_world.z});
                if (bbox_collide(collider_rect_ptr(actor->box_collider, actor->capsule_collider),
                                 collider_rect_ptr(ob->box_collider, ob->capsule_collider)) &&
                    y_ranges_overlap_horizontal(actor_world.y, actor_hh, ob_world.y, ob_hh)) {
                    move_z = 0.0f; break;
                }
            }
        }

        actor->transform->position.x += move_x;
        actor->transform->position.z += move_z;
    }

    /* Vertical collision — directional: only collide with obstacles in the
       direction of travel (floors when falling, ceilings when jumping). */
    {
        vec2 current_xz = { actor_world.x, actor_world.z };
        float vel_y = actor->velocity->velocity.y;
        float predicted_y = actor_world.y + vel_y * gs->dt;

        sync_collider_to_pos(actor->box_collider, actor->capsule_collider, current_xz);

        for (i = 0; i < obstacle_count; i++) {
            collider_target_view *ob = &obstacles[i];
            Vec3 ob_world = resolve_world_position(gs, ob->entity_index);
            float ob_hh = collider_half_height(ob->box_collider, ob->capsule_collider);
            float ob_top = ob_world.y + ob_hh;
            float ob_bottom = ob_world.y - ob_hh;

            /* Directional filter: skip obstacles on the wrong side */
            if (vel_y < 0.0f && ob_top > actor_world.y) continue;  /* falling: skip ceilings */
            if (vel_y > 0.0f && ob_bottom < actor_world.y) continue; /* jumping: skip floors */

            sync_collider_to_pos(ob->box_collider, ob->capsule_collider,
                                 (vec2){ob_world.x, ob_world.z});
            if (bbox_collide(collider_rect_ptr(actor->box_collider, actor->capsule_collider),
                             collider_rect_ptr(ob->box_collider, ob->capsule_collider)) &&
                y_ranges_overlap(predicted_y, actor_hh, ob_world.y, ob_hh)) {
                vertical_collision = 1; break;
            }
        }
    }

    if (!vertical_collision) {
        float new_y = actor_world.y + actor->velocity->velocity.y * gs->dt;
        actor->transform->position.y = new_y - actor_parent_offset.y;
    } else if (actor->velocity->velocity.y < 0.0f) {
        actor->velocity->velocity.y = 0.0f;
    }

    actor->velocity->velocity.x = 0.0f;
    actor->velocity->velocity.z = 0.0f;
}

void apply_movement(game_state* gs) {
    int i;
    if (!gs || gs->scene_entity_count <= 0) return;

    if (gs->character_controller_component_count > 0) {
        /* Multi-actor: run physics for every character controller entity */
        for (i = 0; i < gs->character_controller_component_count; i++) {
            character_controller_component *cc = &gs->character_controller_components[i];
            physics_actor_view actor;
            int entity = cc->entity_index;
            if (entity < 0 || entity >= gs->scene_entity_count) continue;
            actor.entity_index    = entity;
            actor.ent             = &gs->scene_entities[entity];
            actor.transform       = find_transform_component(gs, entity);
            actor.velocity        = find_velocity_component(gs, entity);
            actor.rigid_body      = find_rigid_body_component(gs, entity);
            actor.health          = find_health_component(gs, entity);
            actor.box_collider    = find_box_collider_component(gs, entity);
            actor.capsule_collider = find_capsule_collider_component(gs, entity);
            if (!actor.transform || !actor.velocity) continue;
            if (!actor.box_collider && !actor.capsule_collider) continue;
            apply_movement_single(gs, &actor);
        }
    } else {
        /* Fallback: single primary actor (scenes without explicit CC components) */
        physics_actor_view actor;
        if (query_primary_actor(gs, &actor))
            apply_movement_single(gs, &actor);
    }
}
