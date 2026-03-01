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
    int i;
    if (!gs || !gs->transform_components) return NULL;
    for (i = 0; i < gs->transform_component_count; i++) {
        transform_component *tc = &gs->transform_components[i];
        if (tc->entity_index == entity_index) return tc;
    }
    return NULL;
}

static parent_transform_component *find_parent_transform_component(game_state *gs, int entity_index) {
    int i;
    if (!gs || !gs->parent_transform_components) return NULL;
    for (i = 0; i < gs->parent_transform_component_count; i++) {
        parent_transform_component *pt = &gs->parent_transform_components[i];
        if (pt->entity_index == entity_index) return pt;
    }
    return NULL;
}

static health_component *find_health_component(game_state *gs, int entity_index) {
    int i;
    if (!gs || !gs->health_components) return NULL;
    for (i = 0; i < gs->health_component_count; i++) {
        health_component *hc = &gs->health_components[i];
        if (hc->entity_index == entity_index) return hc;
    }
    return NULL;
}

static velocity_component *find_velocity_component(game_state *gs, int entity_index) {
    int i;
    if (!gs || !gs->velocity_components) return NULL;
    for (i = 0; i < gs->velocity_component_count; i++) {
        velocity_component *vc = &gs->velocity_components[i];
        if (vc->entity_index == entity_index) return vc;
    }
    return NULL;
}

static rigid_body_component *find_rigid_body_component(game_state *gs, int entity_index) {
    int i;
    if (!gs || !gs->rigid_body_components) return NULL;
    for (i = 0; i < gs->rigid_body_component_count; i++) {
        rigid_body_component *rb = &gs->rigid_body_components[i];
        if (rb->entity_index == entity_index) return rb;
    }
    return NULL;
}

static character_controller_component *find_character_controller_component(game_state *gs, int entity_index) {
    int i;
    if (!gs || !gs->character_controller_components) return NULL;
    for (i = 0; i < gs->character_controller_component_count; i++) {
        character_controller_component *cc = &gs->character_controller_components[i];
        if (cc->entity_index == entity_index) return cc;
    }
    return NULL;
}

static box_collider_component *find_box_collider_component(game_state *gs, int entity_index) {
    int i;
    if (!gs || !gs->box_collider_components) return NULL;
    for (i = 0; i < gs->box_collider_component_count; i++) {
        box_collider_component *cc = &gs->box_collider_components[i];
        if (cc->entity_index == entity_index) return cc;
    }
    return NULL;
}

static capsule_collider_component *find_capsule_collider_component(game_state *gs, int entity_index) {
    int i;
    if (!gs || !gs->capsule_collider_components) return NULL;
    for (i = 0; i < gs->capsule_collider_component_count; i++) {
        capsule_collider_component *cc = &gs->capsule_collider_components[i];
        if (cc->entity_index == entity_index) return cc;
    }
    return NULL;
}

static camera_component *find_camera_component(game_state *gs, int entity_index) {
    int i;
    if (!gs || !gs->camera_components) return NULL;
    for (i = 0; i < gs->camera_component_count; i++) {
        camera_component *cc = &gs->camera_components[i];
        if (cc->entity_index == entity_index) return cc;
    }
    return NULL;
}

static mesh_component *find_mesh_component(game_state *gs, int entity_index) {
    int i;
    if (!gs || !gs->mesh_components) return NULL;
    for (i = 0; i < gs->mesh_component_count; i++) {
        mesh_component *mc = &gs->mesh_components[i];
        if (mc->entity_index == entity_index) return mc;
    }
    return NULL;
}

static Vec3 resolve_world_position(game_state *gs, int entity_index) {
    Vec3 world = VEC3(0.0f, 0.0f, 0.0f);
    int current = entity_index;
    int guard = 0;
    if (!gs || !gs->scene_entities || !gs->transform_components) return world;

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

static Vec3 resolve_parent_world_offset(game_state *gs, int entity_index) {
    transform_component *tc = find_transform_component(gs, entity_index);
    Vec3 world = resolve_world_position(gs, entity_index);
    if (!tc) return world;
    return vec3_sub(world, tc->position);
}

static int query_primary_actor(game_state *gs, physics_actor_view *out_actor) {
    int i;
    if (!gs || !out_actor || !gs->scene_entities || !gs->velocity_components) return 0;

    for (i = 0; i < gs->character_controller_component_count; i++) {
        character_controller_component *ccmp = &gs->character_controller_components[i];
        int entity_index = ccmp->entity_index;
        transform_component *tc;
        velocity_component *vc;
        rigid_body_component *rb;
        box_collider_component *bc;
        capsule_collider_component *cc;

        if (entity_index < 0 || entity_index >= gs->scene_entity_count) continue;
        if (!find_character_controller_component(gs, entity_index)) continue;

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
    if (!gs || !out_targets || max_targets <= 0 || !gs->scene_entities) {
        return 0;
    }

    for (i = 0; i < gs->scene_entity_count && count < max_targets; i++) {
        int entity_index = i;
        box_collider_component *bc;
        capsule_collider_component *cc;
        transform_component *tc;
        health_component *hc;

        if (entity_index == exclude_entity_index) continue;
        if (entity_index < 0 || entity_index >= gs->scene_entity_count) continue;
        bc = find_box_collider_component(gs, entity_index);
        cc = find_capsule_collider_component(gs, entity_index);
        if (!bc && !cc) continue;

        tc = find_transform_component(gs, entity_index);
        hc = find_health_component(gs, entity_index);
        if (!tc || !hc) continue;

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
    if (!gs || !out_targets || max_targets <= 0 || !gs->scene_entities) {
        return 0;
    }

    for (i = 0; i < gs->scene_entity_count && count < max_targets; i++) {
        int entity_index = i;
        box_collider_component *bc;
        capsule_collider_component *cc;
        entity *ent;
        transform_component *tc;

        if (entity_index == exclude_entity_index) continue;
        if (entity_index < 0 || entity_index >= gs->scene_entity_count) continue;
        bc = find_box_collider_component(gs, entity_index);
        cc = find_capsule_collider_component(gs, entity_index);
        if (!bc && !cc) continue;
        ent = &gs->scene_entities[entity_index];
        if (find_camera_component(gs, entity_index)) continue;

        tc = find_transform_component(gs, entity_index);
        if (!tc) continue;

        out_targets[count].entity_index = entity_index;
        out_targets[count].ent = ent;
        out_targets[count].transform = tc;
        out_targets[count].health = NULL;
        out_targets[count].box_collider = bc;
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
    rect *r = collider_rect_ptr(bc, cc);
    if (!r) return;
    r->x = pos.x;
    r->y = pos.y;
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
    if (!gs || !gs->scene_entities || gs->scene_entity_count <= 0) return;

    if (!query_primary_actor(gs, &actor)) return;
    if (!actor.health) return;
    pa = &actor.ent->current_animation;
    actor_world = resolve_world_position(gs, actor.entity_index);

    predicted_player_pos.x = actor_world.x + (actor.velocity->velocity.x * gs->dt);
    predicted_player_pos.y = actor_world.z + (actor.velocity->velocity.z * gs->dt);
    sync_collider_to_pos(actor.box_collider, actor.capsule_collider, predicted_player_pos);

    debug_draw_rect(&gs->dbg, predicted_player_pos,
                    collider_rect_ptr(actor.box_collider, actor.capsule_collider)->w,
                    collider_rect_ptr(actor.box_collider, actor.capsule_collider)->h,
                    DEBUG_BLUE);

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
    for (ti = 0; ti < target_count; ti++) {
        collider_target_view *target = &targets[ti];
        animator* a = &target->ent->current_animation;
        vec2 enemy_pos;
        debug_color color = DEBUG_GREEN;
        Vec3 target_world = resolve_world_position(gs, target->entity_index);

        sync_collider_to_pos(target->box_collider, target->capsule_collider,
                             (vec2){target_world.x, target_world.z});

        if (player_hitbox_active && bbox_collide(&player_hit_box,
            collider_rect_ptr(target->box_collider, target->capsule_collider))) {
            color = DEBUG_RED;
            target->health->health -= 5 * gs->dt;
            if (target->health->health < 0.0f) target->health->health = 0.0f;
        }

        if (bbox_collide(collider_rect_ptr(actor.box_collider, actor.capsule_collider),
                         collider_rect_ptr(target->box_collider, target->capsule_collider))) {
            color = DEBUG_RED;
            actor.health->health -= 5.0f * gs->dt;
            if (actor.health->health < 0.0f) {
                actor.health->health = 0.0f;
            }
        }

        if (animator_hitbox_active(a)) {
            rect enemy_hit_box = make_animator_hitbox(target_world, a);
            vec2 enemy_hit_center;
            enemy_hit_center.x = enemy_hit_box.x;
            enemy_hit_center.y = enemy_hit_box.y;
            debug_draw_rect(&gs->dbg, enemy_hit_center,
                            enemy_hit_box.w, enemy_hit_box.h, DEBUG_YELLOW);
        }

        enemy_pos.x = collider_rect_ptr(target->box_collider, target->capsule_collider)->x;
        enemy_pos.y = collider_rect_ptr(target->box_collider, target->capsule_collider)->y;
        debug_draw_rect(&gs->dbg, enemy_pos,
                        collider_rect_ptr(target->box_collider, target->capsule_collider)->w,
                        collider_rect_ptr(target->box_collider, target->capsule_collider)->h,
                        color);

        enemy_pos.x = target_world.x;
        enemy_pos.y = target_world.z;
        draw_center_cross(&gs->dbg, enemy_pos, cross_size, DEBUG_RED);
    }

    sync_collider_to_pos(actor.box_collider, actor.capsule_collider,
                         (vec2){actor_world.x, actor_world.z});
}

static float collider_half_height(box_collider_component *bc, capsule_collider_component *cc) {
    if (cc) return cc->half_height;
    if (bc) return bc->half_height;
    return 0.5f;
}

static int y_ranges_overlap(float y1, float hh1, float y2, float hh2) {
    return (y1 - hh1 < y2 + hh2) && (y1 + hh1 > y2 - hh2);
}

void apply_movement(game_state* gs) {
    physics_actor_view actor;
    collider_target_view obstacles[PHYSICS_QUERY_MAX_RESULTS];
    int obstacle_count;
    int i;
    int vertical_collision = 0;
    Vec3 actor_world;
    Vec3 actor_parent_offset;
    float actor_hh;
    const float gravity = -18.0f;
    if (!gs || !gs->scene_entities || gs->scene_entity_count <= 0) return;

    if (!query_primary_actor(gs, &actor)) return;
    actor_world = resolve_world_position(gs, actor.entity_index);
    actor_parent_offset = resolve_parent_world_offset(gs, actor.entity_index);
    actor_hh = collider_half_height(actor.box_collider, actor.capsule_collider);

    if (actor.rigid_body && actor.rigid_body->use_gravity) {
        actor.velocity->velocity.y += gravity * gs->dt;
    }

    obstacle_count = query_movement_obstacles(gs, actor.entity_index,
                                              obstacles, PHYSICS_QUERY_MAX_RESULTS);

    /* Horizontal collision with wall sliding (separate-axis).
       Test X and Z independently — blocked axis zeroes out,
       free axis slides through.  v_slide = v - dot(v,n)*n
       simplifies to this for axis-aligned boxes. */
    {
        float move_x = actor.velocity->velocity.x * gs->dt;
        float move_z = actor.velocity->velocity.z * gs->dt;

        /* Test X axis alone */
        {
            vec2 test_xz = { actor_world.x + move_x, actor_world.z };
            sync_collider_to_pos(actor.box_collider, actor.capsule_collider, test_xz);
            for (i = 0; i < obstacle_count; i++) {
                collider_target_view *ob = &obstacles[i];
                Vec3 ob_world = resolve_world_position(gs, ob->entity_index);
                float ob_hh = collider_half_height(ob->box_collider, ob->capsule_collider);
                sync_collider_to_pos(ob->box_collider, ob->capsule_collider,
                                     (vec2){ob_world.x, ob_world.z});
                if (bbox_collide(collider_rect_ptr(actor.box_collider, actor.capsule_collider),
                                 collider_rect_ptr(ob->box_collider, ob->capsule_collider)) &&
                    y_ranges_overlap(actor_world.y, actor_hh, ob_world.y, ob_hh)) {
                    move_x = 0.0f;
                    break;
                }
            }
        }

        /* Test Z axis (from resolved X) */
        {
            vec2 test_xz = { actor_world.x + move_x, actor_world.z + move_z };
            sync_collider_to_pos(actor.box_collider, actor.capsule_collider, test_xz);
            for (i = 0; i < obstacle_count; i++) {
                collider_target_view *ob = &obstacles[i];
                Vec3 ob_world = resolve_world_position(gs, ob->entity_index);
                float ob_hh = collider_half_height(ob->box_collider, ob->capsule_collider);
                sync_collider_to_pos(ob->box_collider, ob->capsule_collider,
                                     (vec2){ob_world.x, ob_world.z});
                if (bbox_collide(collider_rect_ptr(actor.box_collider, actor.capsule_collider),
                                 collider_rect_ptr(ob->box_collider, ob->capsule_collider)) &&
                    y_ranges_overlap(actor_world.y, actor_hh, ob_world.y, ob_hh)) {
                    move_z = 0.0f;
                    break;
                }
            }
        }

        actor.transform->position.x += move_x;
        actor.transform->position.z += move_z;
    }

    /* Vertical collision — gravity / jump */
    {
        vec2 current_xz = { actor_world.x, actor_world.z };
        float predicted_y = actor_world.y + actor.velocity->velocity.y * gs->dt;

        sync_collider_to_pos(actor.box_collider, actor.capsule_collider, current_xz);

        for (i = 0; i < obstacle_count; i++) {
            collider_target_view *ob = &obstacles[i];
            Vec3 ob_world = resolve_world_position(gs, ob->entity_index);
            float ob_hh = collider_half_height(ob->box_collider, ob->capsule_collider);

            sync_collider_to_pos(ob->box_collider, ob->capsule_collider,
                                 (vec2){ob_world.x, ob_world.z});

            if (bbox_collide(collider_rect_ptr(actor.box_collider, actor.capsule_collider),
                             collider_rect_ptr(ob->box_collider, ob->capsule_collider)) &&
                y_ranges_overlap(predicted_y, actor_hh, ob_world.y, ob_hh)) {
                vertical_collision = 1;
                break;
            }
        }
    }

    if (!vertical_collision) {
        float new_y = actor_world.y + actor.velocity->velocity.y * gs->dt;
        actor.transform->position.y = new_y - actor_parent_offset.y;
    } else if (actor.velocity->velocity.y < 0.0f) {
        actor.velocity->velocity.y = 0.0f;
    }

    actor.velocity->velocity.x = 0.0f;
    actor.velocity->velocity.z = 0.0f;
}
