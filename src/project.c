#include "project.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Unity-build toml-c: include the .c so we don't need a separate compilation unit */
#include "toml.c"

/* ── Helpers ─────────────────────────────────────────────────────────── */

/* Copy a TOML string value into a fixed buffer, free the allocated string */
static void copy_toml_string(const toml_table_t *tbl, const char *key, char *dst, int dstsz) {
    toml_value_t v = toml_table_string(tbl, key);
    if (v.ok && v.u.s) {
        snprintf(dst, dstsz, "%s", v.u.s);
        free(v.u.s);
    }
}

/* Read a TOML array of 3 doubles into a float[3] */
static void read_vec3(const toml_array_t *arr, float out[3]) {
    int i;
    if (!arr) return;
    for (i = 0; i < 3 && i < toml_array_len(arr); i++) {
        toml_value_t v = toml_array_double(arr, i);
        if (v.ok) out[i] = (float)v.u.d;
    }
}

/* Read a TOML array of 2 doubles into a float[2] */
static void read_vec2(const toml_array_t *arr, float out[2]) {
    int i;
    if (!arr) return;
    for (i = 0; i < 2 && i < toml_array_len(arr); i++) {
        toml_value_t v = toml_array_double(arr, i);
        if (v.ok) out[i] = (float)v.u.d;
    }
}

/* Read all key-value pairs from a TOML table into the unified asset array */
static int read_asset_table_unified(const toml_table_t *tbl,
                                     project_asset *assets, int *count,
                                     int max, project_asset_type type) {
    int i, n, added = 0;
    if (!tbl) return 0;
    n = toml_table_len(tbl);
    for (i = 0; i < n && *count < max; i++) {
        int keylen;
        const char *k = toml_table_key(tbl, i, &keylen);
        toml_value_t v;
        project_asset *a;
        if (!k) continue;
        a = &assets[*count];
        snprintf(a->key, 64, "%.*s", keylen, k);
        v = toml_table_string(tbl, a->key);
        if (v.ok && v.u.s) {
            snprintf(a->path, 256, "%s", v.u.s);
            free(v.u.s);
            a->type = type;
            (*count)++;
            added++;
        }
    }
    return added;
}

static int parse_index_key(const char *key, int keylen) {
    char tmp[32];
    int i;
    if (!key || keylen <= 0 || keylen >= (int)sizeof(tmp)) return -1;
    snprintf(tmp, sizeof(tmp), "%.*s", keylen, key);
    for (i = 0; tmp[i]; i++) {
        if (!isdigit((unsigned char)tmp[i])) return -1;
    }
    return atoi(tmp);
}

static toml_table_t *indexed_subtable(const toml_table_t *tbl, int key_i, int *out_entity_index) {
    int keylen;
    const char *k;
    char keybuf[64];
    int entity_index;
    toml_table_t *sub;
    if (!tbl) return NULL;

    k = toml_table_key(tbl, key_i, &keylen);
    if (!k) return NULL;
    if (keylen <= 0 || keylen >= (int)sizeof(keybuf)) return NULL;
    snprintf(keybuf, sizeof(keybuf), "%.*s", keylen, k);
    entity_index = parse_index_key(k, keylen);
    if (entity_index < 0) return NULL;

    sub = toml_table_table(tbl, keybuf);
    if (!sub) return NULL;

    if (out_entity_index) *out_entity_index = entity_index;
    return sub;
}

/* ── Legacy parser (backward compatibility) ──────────────────────────── */

static void parse_legacy_entity(const toml_table_t *tbl, project_entity *ent) {
    toml_value_t vi, vd;
    toml_array_t *arr;
    toml_table_t *sub;

    copy_toml_string(tbl, "id", ent->id, sizeof(ent->id));
    copy_toml_string(tbl, "type", ent->type, sizeof(ent->type));
    copy_toml_string(tbl, "model", ent->model, sizeof(ent->model));
    copy_toml_string(tbl, "animations", ent->animations, sizeof(ent->animations));

    arr = toml_table_array(tbl, "position");
    read_vec3(arr, ent->position);

    vd = toml_table_double(tbl, "rotation");
    if (vd.ok) ent->rotation = (float)vd.u.d;

    vi = toml_table_int(tbl, "health");
    if (vi.ok) ent->health = (int)vi.u.i;

    vd = toml_table_double(tbl, "speed");
    if (vd.ok) ent->speed = (float)vd.u.d;

    sub = toml_table_table(tbl, "collider");
    if (sub) {
        arr = toml_table_array(sub, "half_extents");
        read_vec3(arr, ent->collider_half_extents);
    }

    sub = toml_table_table(tbl, "ai");
    if (sub) {
        copy_toml_string(sub, "behavior", ent->ai_behavior, sizeof(ent->ai_behavior));
        vd = toml_table_double(sub, "aggro_range");
        if (vd.ok) ent->aggro_range = (float)vd.u.d;

        arr = toml_table_array(sub, "patrol_points");
        if (arr) {
            int i, n = toml_array_len(arr);
            if (n > 8) n = 8;
            ent->patrol_count = n;
            for (i = 0; i < n; i++) {
                toml_array_t *pt = toml_array_array(arr, i);
                read_vec3(pt, ent->patrol_points[i]);
            }
        }
    }

    sub = toml_table_table(tbl, "interaction");
    if (sub) {
        copy_toml_string(sub, "type", ent->interaction_type, sizeof(ent->interaction_type));
        arr = toml_table_array(sub, "items");
        if (arr) {
            int i, n = toml_array_len(arr);
            if (n > 4) n = 4;
            ent->item_count = n;
            for (i = 0; i < n; i++) {
                toml_value_t vs = toml_array_string(arr, i);
                if (vs.ok && vs.u.s) {
                    snprintf(ent->items[i], 64, "%s", vs.u.s);
                    free(vs.u.s);
                }
            }
        }
    }
}

static void parse_legacy_scene_tables(const toml_table_t *root, project_data *out) {
    toml_table_t *sub;
    toml_array_t *arr;
    toml_value_t v;
    int i, n;

    /* [player] -> first legacy entity */
    sub = toml_table_table(root, "player");
    if (sub && out->entity_count < 64) {
        project_entity *ent = &out->entities[out->entity_count];
        snprintf(ent->id, sizeof(ent->id), "player");
        snprintf(ent->type, sizeof(ent->type), "player");
        parse_legacy_entity(sub, ent);
        snprintf(ent->type, sizeof(ent->type), "player");
        out->entity_count++;
    }

    /* [[entities]] */
    arr = toml_table_array(root, "entities");
    if (arr) {
        n = toml_array_len(arr);
        for (i = 0; i < n && out->entity_count < 64; i++) {
            toml_table_t *etbl = toml_array_table(arr, i);
            if (!etbl) continue;
            parse_legacy_entity(etbl, &out->entities[out->entity_count]);
            out->entity_count++;
        }
    }

    /* [[pieces]] */
    arr = toml_table_array(root, "pieces");
    if (arr) {
        n = toml_array_len(arr);
        for (i = 0; i < n && out->piece_count < 256; i++) {
            toml_table_t *ptbl = toml_array_table(arr, i);
            project_piece *p;
            if (!ptbl) continue;
            p = &out->pieces[out->piece_count];
            copy_toml_string(ptbl, "asset", p->asset, sizeof(p->asset));
            read_vec3(toml_table_array(ptbl, "position"), p->position);
            v = toml_table_double(ptbl, "rotation");
            if (v.ok) p->rotation = (float)v.u.d;
            v = toml_table_double(ptbl, "scale");
            if (v.ok) p->scale = (float)v.u.d;
            out->piece_count++;
        }
    }
}

/* ── ECS scene parser (entity list + component tables) ──────────────── */

static void parse_scene_entity_list(const toml_table_t *root, project_data *out) {
    toml_table_t *entities_tbl = toml_table_table(root, "entities");
    toml_array_t *arr;
    int i, n;
    if (!entities_tbl) return;

    arr = toml_table_array(entities_tbl, "list");
    if (!arr) arr = toml_table_array(entities_tbl, "names");
    if (!arr) return;

    n = toml_array_len(arr);
    if (n > PROJECT_COMP_MAX) n = PROJECT_COMP_MAX;
    out->scene_entity_count = n;

    for (i = 0; i < n; i++) {
        toml_value_t v = toml_array_string(arr, i);
        if (v.ok && v.u.s) {
            snprintf(out->scene_entity_names[i], sizeof(out->scene_entity_names[i]), "%s", v.u.s);
            free(v.u.s);
        } else {
            snprintf(out->scene_entity_names[i], sizeof(out->scene_entity_names[i]), "entity_%d", i);
        }
    }
}

static void parse_transform_table(const toml_table_t *tbl, project_data *out) {
    int i, n;
    if (!tbl || out->scene_entity_count <= 0) return;
    n = toml_table_len(tbl);
    for (i = 0; i < n; i++) {
        int entity_index = -1;
        toml_table_t *entry = indexed_subtable(tbl, i, &entity_index);
        project_transform *t;
        if (!entry) continue;
        if (entity_index < 0 || entity_index >= out->scene_entity_count) continue;
        if (out->transform_count >= PROJECT_COMP_MAX) continue;

        t = &out->transforms[out->transform_count++];
        t->entity = entity_index;
        t->position[0] = 0.0f; t->position[1] = 0.0f; t->position[2] = 0.0f;
        read_vec3(toml_table_array(entry, "position"), t->position);
    }
}

static int is_identity_scale(const float scale[3]) {
    const float eps = 0.0001f;
    float dx = scale[0] - 1.0f;
    float dy = scale[1] - 1.0f;
    float dz = scale[2] - 1.0f;
    if (dx < 0.0f) dx = -dx;
    if (dy < 0.0f) dy = -dy;
    if (dz < 0.0f) dz = -dz;
    return dx <= eps && dy <= eps && dz <= eps;
}

static void parse_rotation_table(const toml_table_t *tbl, project_data *out) {
    int i, n;
    if (!tbl || out->scene_entity_count <= 0) return;
    n = toml_table_len(tbl);
    for (i = 0; i < n; i++) {
        int entity_index = -1;
        toml_table_t *entry = indexed_subtable(tbl, i, &entity_index);
        toml_value_t vd;
        project_rotation *r;
        if (!entry) continue;
        if (entity_index < 0 || entity_index >= out->scene_entity_count) continue;

        vd = toml_table_double(entry, "rotation_y");
        if (!vd.ok) vd = toml_table_double(entry, "rotation");
        if (!vd.ok) vd = toml_table_double(entry, "y");
        if (!vd.ok) vd = toml_table_double(entry, "value");
        if (!vd.ok) continue;
        if (out->rotation_count >= PROJECT_COMP_MAX) continue;

        r = &out->rotations[out->rotation_count++];
        r->entity = entity_index;
        r->y = (float)vd.u.d;
    }
}

static void parse_scale_table(const toml_table_t *tbl, project_data *out) {
    int i, n;
    if (!tbl || out->scene_entity_count <= 0) return;
    n = toml_table_len(tbl);
    for (i = 0; i < n; i++) {
        int entity_index = -1;
        toml_table_t *entry = indexed_subtable(tbl, i, &entity_index);
        float parsed_scale[3] = {1.0f, 1.0f, 1.0f};
        project_scale *s;
        if (!entry) continue;
        if (entity_index < 0 || entity_index >= out->scene_entity_count) continue;

        read_vec3(toml_table_array(entry, "value"), parsed_scale);
        read_vec3(toml_table_array(entry, "scale"), parsed_scale);

        if (is_identity_scale(parsed_scale)) continue;
        if (out->scale_count >= PROJECT_COMP_MAX) continue;

        s = &out->scales[out->scale_count++];
        s->entity = entity_index;
        s->value[0] = parsed_scale[0];
        s->value[1] = parsed_scale[1];
        s->value[2] = parsed_scale[2];
    }
}

static void parse_parent_transform_table(const toml_table_t *tbl, project_data *out) {
    int i, n;
    if (!tbl || out->scene_entity_count <= 0) return;
    n = toml_table_len(tbl);
    for (i = 0; i < n; i++) {
        int entity_index = -1;
        toml_table_t *entry = indexed_subtable(tbl, i, &entity_index);
        toml_value_t vi;
        project_parent_transform *pt;
        if (!entry) continue;
        if (entity_index < 0 || entity_index >= out->scene_entity_count) continue;

        vi = toml_table_int(entry, "parent");
        if (!vi.ok) vi = toml_table_int(entry, "entity");
        if (!vi.ok) continue;
        if (out->parent_transform_count >= PROJECT_COMP_MAX) continue;

        pt = &out->parent_transforms[out->parent_transform_count++];
        pt->entity = entity_index;
        pt->parent = (int)vi.u.i;
    }
}

static void parse_parent_rotation_table(const toml_table_t *tbl, project_data *out) {
    int i, n;
    if (!tbl || out->scene_entity_count <= 0) return;
    n = toml_table_len(tbl);
    for (i = 0; i < n; i++) {
        int entity_index = -1;
        toml_table_t *entry = indexed_subtable(tbl, i, &entity_index);
        toml_value_t vb;
        int enabled = 1;
        if (!entry) continue;
        if (entity_index < 0 || entity_index >= out->scene_entity_count) continue;
        vb = toml_table_bool(entry, "enabled");
        if (vb.ok) enabled = vb.u.b ? 1 : 0;
        if (!enabled) continue;
        if (out->parent_rotation_count >= PROJECT_COMP_MAX) continue;

        out->parent_rotations[out->parent_rotation_count++].entity = entity_index;
    }
}

static void parse_mesh_table(const toml_table_t *tbl, project_data *out) {
    int i, n;
    if (!tbl || out->scene_entity_count <= 0) return;
    n = toml_table_len(tbl);
    for (i = 0; i < n; i++) {
        int entity_index = -1;
        toml_table_t *entry = indexed_subtable(tbl, i, &entity_index);
        toml_value_t vb;
        project_mesh *m;
        if (!entry) continue;
        if (entity_index < 0 || entity_index >= out->scene_entity_count) continue;
        if (out->mesh_count >= PROJECT_COMP_MAX) continue;

        m = &out->meshes[out->mesh_count++];
        m->entity = entity_index;
        m->visible = 1;
        m->model[0] = '\0';
        copy_toml_string(entry, "model", m->model, sizeof(m->model));
        vb = toml_table_bool(entry, "visible");
        if (vb.ok) m->visible = vb.u.b ? 1 : 0;
    }
}

static void parse_animation_table(const toml_table_t *tbl, project_data *out) {
    int i, n;
    if (!tbl || out->scene_entity_count <= 0) return;
    n = toml_table_len(tbl);
    for (i = 0; i < n; i++) {
        int entity_index = -1;
        toml_table_t *entry = indexed_subtable(tbl, i, &entity_index);
        toml_value_t vb, vi, vd;
        project_anim *a;
        if (!entry) continue;
        if (entity_index < 0 || entity_index >= out->scene_entity_count) continue;
        if (out->anim_count >= PROJECT_COMP_MAX) continue;

        a = &out->anims[out->anim_count++];
        a->entity = entity_index;
        a->playing = 1;
        a->speed = 1.0f;
        a->clip = 0;
        a->time = 0.0f;
        a->asset[0] = '\0';

        copy_toml_string(entry, "asset", a->asset, sizeof(a->asset));
        if (!a->asset[0])
            copy_toml_string(entry, "animations", a->asset, sizeof(a->asset));

        vb = toml_table_bool(entry, "playing");
        if (vb.ok) a->playing = vb.u.b ? 1 : 0;

        vi = toml_table_int(entry, "clip");
        if (vi.ok) a->clip = (int)vi.u.i;

        vd = toml_table_double(entry, "time");
        if (vd.ok) a->time = (float)vd.u.d;

        vd = toml_table_double(entry, "speed");
        if (vd.ok) a->speed = (float)vd.u.d;
    }
}

static void parse_velocity_table(const toml_table_t *tbl, project_data *out) {
    int i, n;
    if (!tbl || out->scene_entity_count <= 0) return;
    n = toml_table_len(tbl);
    for (i = 0; i < n; i++) {
        int entity_index = -1;
        toml_table_t *entry = indexed_subtable(tbl, i, &entity_index);
        toml_value_t vd;
        project_velocity *vel;
        if (!entry) continue;
        if (entity_index < 0 || entity_index >= out->scene_entity_count) continue;
        if (out->velocity_count >= PROJECT_COMP_MAX) continue;

        vel = &out->velocities[out->velocity_count++];
        vel->entity = entity_index;
        vel->value[0] = 0.0f; vel->value[1] = 0.0f;
        read_vec2(toml_table_array(entry, "value"), vel->value);
        vd = toml_table_double(entry, "x");
        if (vd.ok) vel->value[0] = (float)vd.u.d;
        vd = toml_table_double(entry, "y");
        if (vd.ok) vel->value[1] = (float)vd.u.d;
    }
}

static void parse_rigid_body_table(const toml_table_t *tbl, project_data *out) {
    int i, n;
    if (!tbl || out->scene_entity_count <= 0) return;
    n = toml_table_len(tbl);
    for (i = 0; i < n; i++) {
        int entity_index = -1;
        toml_table_t *entry = indexed_subtable(tbl, i, &entity_index);
        toml_value_t vb;
        project_rigid_body *rb;
        if (!entry) continue;
        if (entity_index < 0 || entity_index >= out->scene_entity_count) continue;
        if (out->rigid_body_count >= PROJECT_COMP_MAX) continue;

        rb = &out->rigid_bodies[out->rigid_body_count++];
        rb->entity = entity_index;
        rb->use_gravity = 1;

        vb = toml_table_bool(entry, "use_gravity");
        if (!vb.ok) vb = toml_table_bool(entry, "gravity");
        if (!vb.ok) vb = toml_table_bool(entry, "enabled");
        if (vb.ok) rb->use_gravity = vb.u.b ? 1 : 0;
    }
}

static void parse_character_controller_table(const toml_table_t *tbl, project_data *out) {
    int i, n;
    if (!tbl || out->scene_entity_count <= 0) return;
    n = toml_table_len(tbl);
    for (i = 0; i < n; i++) {
        int entity_index = -1;
        toml_table_t *entry = indexed_subtable(tbl, i, &entity_index);
        toml_value_t vd;
        project_character_controller *cc;
        if (!entry) continue;
        if (entity_index < 0 || entity_index >= out->scene_entity_count) continue;
        if (out->character_controller_count >= PROJECT_COMP_MAX) continue;

        cc = &out->character_controllers[out->character_controller_count++];
        cc->entity = entity_index;
        cc->move_speed = 5.0f;
        cc->jump_speed = 8.5f;

        vd = toml_table_double(entry, "move_speed");
        if (vd.ok) cc->move_speed = (float)vd.u.d;
        vd = toml_table_double(entry, "jump_speed");
        if (!vd.ok) vd = toml_table_double(entry, "jump");
        if (vd.ok) cc->jump_speed = (float)vd.u.d;
    }
}

static void parse_health_table(const toml_table_t *tbl, project_data *out) {
    int i, n;
    if (!tbl || out->scene_entity_count <= 0) return;
    n = toml_table_len(tbl);
    for (i = 0; i < n; i++) {
        int entity_index = -1;
        toml_table_t *entry = indexed_subtable(tbl, i, &entity_index);
        toml_value_t vd;
        project_health *h;
        if (!entry) continue;
        if (entity_index < 0 || entity_index >= out->scene_entity_count) continue;
        if (out->health_count >= PROJECT_COMP_MAX) continue;

        h = &out->healths[out->health_count++];
        h->entity = entity_index;
        h->current = 100.0f;
        h->max = 100.0f;

        vd = toml_table_double(entry, "current");
        if (!vd.ok) vd = toml_table_double(entry, "value");
        if (vd.ok) h->current = (float)vd.u.d;

        vd = toml_table_double(entry, "max");
        if (vd.ok) h->max = (float)vd.u.d;
        else h->max = h->current;
    }
}

static void parse_box_collider_table(const toml_table_t *tbl, project_data *out) {
    int i, n;
    if (!tbl || out->scene_entity_count <= 0) return;
    n = toml_table_len(tbl);
    for (i = 0; i < n; i++) {
        int entity_index = -1;
        toml_table_t *entry = indexed_subtable(tbl, i, &entity_index);
        project_box_collider *bc;
        if (!entry) continue;
        if (entity_index < 0 || entity_index >= out->scene_entity_count) continue;
        if (out->box_collider_count >= PROJECT_COMP_MAX) continue;

        bc = &out->box_colliders[out->box_collider_count++];
        bc->entity = entity_index;
        bc->half_extents[0] = 0.5f;
        bc->half_extents[1] = 0.5f;
        bc->half_extents[2] = 0.5f;
        read_vec3(toml_table_array(entry, "half_extents"), bc->half_extents);
    }
}

static void parse_capsule_collider_table(const toml_table_t *tbl, project_data *out) {
    int i, n;
    if (!tbl || out->scene_entity_count <= 0) return;
    n = toml_table_len(tbl);
    for (i = 0; i < n; i++) {
        int entity_index = -1;
        toml_table_t *entry = indexed_subtable(tbl, i, &entity_index);
        toml_value_t vd;
        int has_radius = 0, has_half_height = 0;
        project_capsule_collider *cc;
        if (!entry) continue;
        if (entity_index < 0 || entity_index >= out->scene_entity_count) continue;
        if (out->capsule_collider_count >= PROJECT_COMP_MAX) continue;

        cc = &out->capsule_colliders[out->capsule_collider_count++];
        cc->entity = entity_index;
        cc->radius = 0.5f;
        cc->half_height = 0.5f;

        vd = toml_table_double(entry, "radius");
        if (vd.ok) { cc->radius = (float)vd.u.d; has_radius = 1; }

        vd = toml_table_double(entry, "half_height");
        if (vd.ok) { cc->half_height = (float)vd.u.d; has_half_height = 1; }

        if (!has_half_height) {
            float he[3] = {0.5f, 0.5f, 0.5f};
            read_vec3(toml_table_array(entry, "half_extents"), he);
            if (!has_radius)
                cc->radius = he[0] > 0.0f ? he[0] : 0.5f;
            cc->half_height = he[1] > cc->radius ? he[1] - cc->radius : cc->radius;
        }
    }
}

static void parse_camera_table(const toml_table_t *tbl, project_data *out) {
    int i, n;
    if (!tbl || out->scene_entity_count <= 0) return;
    n = toml_table_len(tbl);
    for (i = 0; i < n; i++) {
        int entity_index = -1;
        toml_table_t *entry = indexed_subtable(tbl, i, &entity_index);
        toml_value_t vd;
        project_cam *c;
        if (!entry) continue;
        if (entity_index < 0 || entity_index >= out->scene_entity_count) continue;
        if (out->camera_count >= PROJECT_COMP_MAX) continue;

        c = &out->cameras[out->camera_count++];
        c->entity = entity_index;
        c->fov = 60.0f;
        c->near_plane = 0.1f;
        c->far_plane = 100.0f;
        c->target[0] = 0.0f; c->target[1] = 0.0f; c->target[2] = 0.0f;
        c->up[0] = 0.0f; c->up[1] = 1.0f; c->up[2] = 0.0f;

        vd = toml_table_double(entry, "fov");
        if (vd.ok) c->fov = (float)vd.u.d;
        vd = toml_table_double(entry, "near");
        if (vd.ok) c->near_plane = (float)vd.u.d;
        vd = toml_table_double(entry, "far");
        if (vd.ok) c->far_plane = (float)vd.u.d;

        read_vec3(toml_table_array(entry, "target"), c->target);
        read_vec3(toml_table_array(entry, "up"), c->up);
    }
}

static void parse_trigger_table(const toml_table_t *tbl, project_data *out) {
    int i, n;
    if (!tbl || out->scene_entity_count <= 0) return;
    n = toml_table_len(tbl);
    for (i = 0; i < n; i++) {
        int entity_index = -1;
        toml_table_t *entry = indexed_subtable(tbl, i, &entity_index);
        toml_value_t vi, vd;
        project_trigger *tr;
        if (!entry) continue;
        if (entity_index < 0 || entity_index >= out->scene_entity_count) continue;
        if (out->trigger_count >= PROJECT_COMP_MAX) continue;

        tr = &out->triggers[out->trigger_count++];
        tr->entity = entity_index;
        tr->type_str[0] = '\0';
        tr->target = 0;
        tr->radius = 1.0f;

        copy_toml_string(entry, "type", tr->type_str, sizeof(tr->type_str));

        vi = toml_table_int(entry, "target");
        if (vi.ok) tr->target = (int)vi.u.i;

        vd = toml_table_double(entry, "radius");
        if (vd.ok) tr->radius = (float)vd.u.d;

        copy_toml_string(entry, "joint", tr->joint, sizeof(tr->joint));
    }
}

static void parse_ecs_component_tables(const toml_table_t *root, project_data *out) {
    parse_transform_table(toml_table_table(root, "transforms"), out);
    parse_rotation_table(toml_table_table(root, "rotations"), out);
    parse_scale_table(toml_table_table(root, "scales"), out);
    parse_parent_transform_table(toml_table_table(root, "parent_transform"), out);
    parse_parent_rotation_table(toml_table_table(root, "parent_rotation"), out);
    parse_mesh_table(toml_table_table(root, "meshes"), out);
    parse_animation_table(toml_table_table(root, "animations"), out);
    parse_velocity_table(toml_table_table(root, "velocities"), out);
    parse_rigid_body_table(toml_table_table(root, "rigid_bodies"), out);
    parse_character_controller_table(toml_table_table(root, "character_controllers"), out);
    parse_health_table(toml_table_table(root, "health"), out);
    parse_box_collider_table(toml_table_table(root, "box_colliders"), out);
    parse_capsule_collider_table(toml_table_table(root, "capsule_colliders"), out);
    parse_box_collider_table(toml_table_table(root, "colliders"), out); /* legacy */
    parse_camera_table(toml_table_table(root, "cameras"), out);
    parse_trigger_table(toml_table_table(root, "triggers"), out);
}

/* ── Public API ──────────────────────────────────────────────────────── */

int project_load(const char *path, project_data *out) {
    FILE *fp;
    char errbuf[256];
    toml_table_t *root, *sub, *assets;
    toml_value_t v;

    memset(out, 0, sizeof(*out));

    fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "project_load: cannot open '%s'\n", path);
        return -1;
    }

    root = toml_parse_file(fp, errbuf, sizeof(errbuf));
    fclose(fp);
    if (!root) {
        fprintf(stderr, "project_load: parse error: %s\n", errbuf);
        return -1;
    }

    /* [project] */
    sub = toml_table_table(root, "project");
    if (sub) {
        copy_toml_string(sub, "name", out->name, sizeof(out->name));
        v = toml_table_int(sub, "version");
        if (v.ok) out->version = (int)v.u.i;
    }

    /* [camera] */
    sub = toml_table_table(root, "camera");
    if (sub) {
        out->has_camera = 1;
        read_vec3(toml_table_array(sub, "eye"), out->camera.eye);
        read_vec3(toml_table_array(sub, "target"), out->camera.target);
        read_vec3(toml_table_array(sub, "up"), out->camera.up);
        v = toml_table_double(sub, "fov");
        if (v.ok) out->camera.fov = (float)v.u.d;
    }

    /* [lighting] */
    sub = toml_table_table(root, "lighting");
    if (sub) {
        toml_array_t *lights;
        int li, ln;
        out->has_lighting = 1;
        read_vec3(toml_table_array(sub, "ambient"), out->lighting.ambient);

        lights = toml_table_array(sub, "point_lights");
        if (lights) {
            ln = toml_array_len(lights);
            if (ln > 8) ln = 8;
            out->lighting.point_light_count = ln;
            for (li = 0; li < ln; li++) {
                toml_table_t *lt = toml_array_table(lights, li);
                if (lt) {
                    project_point_light *pl = &out->lighting.point_lights[li];
                    read_vec3(toml_table_array(lt, "position"), pl->position);
                    read_vec3(toml_table_array(lt, "color"), pl->color);
                    v = toml_table_double(lt, "intensity");
                    if (v.ok) pl->intensity = (float)v.u.d;
                    v = toml_table_double(lt, "radius");
                    if (v.ok) pl->radius = (float)v.u.d;
                }
            }
        }
    }

    /* [assets] */
    assets = toml_table_table(root, "assets");
    if (assets) {
        out->asset_count = 0;
        read_asset_table_unified(toml_table_table(assets, "models"),
                                  out->assets, &out->asset_count, PROJECT_ASSET_MAX, ASSET_MODEL);
        read_asset_table_unified(toml_table_table(assets, "animations"),
                                  out->assets, &out->asset_count, PROJECT_ASSET_MAX, ASSET_ANIMATION);
        read_asset_table_unified(toml_table_table(assets, "dungeon_pieces"),
                                  out->assets, &out->asset_count, PROJECT_ASSET_MAX, ASSET_DUNGEON_PIECE);
        read_asset_table_unified(toml_table_table(assets, "sprites"),
                                  out->assets, &out->asset_count, PROJECT_ASSET_MAX, ASSET_SPRITE);
    }

    /* New ECS scene schema:
       [entities] list = [...]
       [transforms]/[meshes]/... with entity-index keys. */
    parse_scene_entity_list(root, out);
    if (out->scene_entity_count > 0) {
        parse_ecs_component_tables(root, out);
    } else {
        parse_legacy_scene_tables(root, out);
    }

    printf("Project loaded: '%s' (v%d) — scene_entities=%d, legacy_entities=%d, pieces=%d, assets=%d, lights=%d\n",
           out->name, out->version, out->scene_entity_count, out->entity_count, out->piece_count,
           out->asset_count, out->lighting.point_light_count);

    toml_free(root);
    return 0;
}

const project_mesh *project_find_mesh(const project_data *p, int entity) {
    int i;
    if (!p) return NULL;
    for (i = 0; i < p->mesh_count; i++)
        if (p->meshes[i].entity == entity) return &p->meshes[i];
    return NULL;
}

const project_anim *project_find_anim(const project_data *p, int entity) {
    int i;
    if (!p) return NULL;
    for (i = 0; i < p->anim_count; i++)
        if (p->anims[i].entity == entity) return &p->anims[i];
    return NULL;
}

const project_box_collider *project_find_box_collider(const project_data *p, int entity) {
    int i;
    if (!p) return NULL;
    for (i = 0; i < p->box_collider_count; i++)
        if (p->box_colliders[i].entity == entity) return &p->box_colliders[i];
    return NULL;
}

const char *project_find_asset(const project_data *p, const char *key, project_asset_type type) {
    int i;
    if (!p || !key || !key[0]) return NULL;
    for (i = 0; i < p->asset_count; i++) {
        if (p->assets[i].type == type && strcmp(p->assets[i].key, key) == 0)
            return p->assets[i].path;
    }
    return NULL;
}
