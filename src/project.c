#include "project.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Read all key-value pairs from a table into parallel key/path arrays */
static int read_asset_table(const toml_table_t *tbl, char keys[][64], char paths[][256], int max) {
    int i, n;
    if (!tbl) return 0;
    n = toml_table_len(tbl);
    if (n > max) n = max;
    for (i = 0; i < n; i++) {
        int keylen;
        const char *k = toml_table_key(tbl, i, &keylen);
        toml_value_t v;
        if (!k) continue;
        snprintf(keys[i], 64, "%.*s", keylen, k);
        v = toml_table_string(tbl, keys[i]);
        if (v.ok && v.u.s) {
            snprintf(paths[i], 256, "%s", v.u.s);
            free(v.u.s);
        }
    }
    return n;
}

/* ── Parse entity from a TOML table ─────────────────────────────────── */

static void parse_entity(const toml_table_t *tbl, project_entity *ent) {
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

    /* Collider: inline table { half_extents = [x, y, z] } */
    sub = toml_table_table(tbl, "collider");
    if (sub) {
        arr = toml_table_array(sub, "half_extents");
        read_vec3(arr, ent->collider_half_extents);
    }

    /* AI sub-table */
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

    /* Interaction sub-table */
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

/* ── Public API ──────────────────────────────────────────────────────── */

int project_load(const char *path, project_data *out) {
    FILE *fp;
    char errbuf[256];
    toml_table_t *root, *sub, *assets;
    toml_array_t *arr;
    toml_value_t v;
    int i, n;

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
        out->has_lighting = 1;
        read_vec3(toml_table_array(sub, "ambient"), out->lighting.ambient);

        lights = toml_table_array(sub, "point_lights");
        if (lights) {
            int li, ln = toml_array_len(lights);
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
        out->model_count = read_asset_table(
            toml_table_table(assets, "models"),
            out->model_keys, out->model_paths, 32);
        out->animation_count = read_asset_table(
            toml_table_table(assets, "animations"),
            out->animation_keys, out->animation_paths, 8);
        out->dungeon_piece_count = read_asset_table(
            toml_table_table(assets, "dungeon_pieces"),
            out->dungeon_piece_keys, out->dungeon_piece_paths, 32);
        out->sprite_count = read_asset_table(
            toml_table_table(assets, "sprites"),
            out->sprite_keys, out->sprite_paths, 16);
    }

    /* [player] → stored as first entity with type="player" */
    sub = toml_table_table(root, "player");
    if (sub && out->entity_count < 64) {
        project_entity *ent = &out->entities[out->entity_count];
        snprintf(ent->id, sizeof(ent->id), "player");
        snprintf(ent->type, sizeof(ent->type), "player");
        parse_entity(sub, ent);
        /* Ensure type stays "player" even if TOML overrides */
        snprintf(ent->type, sizeof(ent->type), "player");
        out->entity_count++;
    }

    /* [[entities]] */
    arr = toml_table_array(root, "entities");
    if (arr) {
        n = toml_array_len(arr);
        for (i = 0; i < n && out->entity_count < 64; i++) {
            toml_table_t *etbl = toml_array_table(arr, i);
            if (etbl) {
                parse_entity(etbl, &out->entities[out->entity_count]);
                out->entity_count++;
            }
        }
    }

    /* [[pieces]] */
    arr = toml_table_array(root, "pieces");
    if (arr) {
        n = toml_array_len(arr);
        for (i = 0; i < n && out->piece_count < 256; i++) {
            toml_table_t *ptbl = toml_array_table(arr, i);
            if (ptbl) {
                project_piece *p = &out->pieces[out->piece_count];
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

    printf("Project loaded: '%s' (v%d) — %d entities, %d pieces, %d sprites, %d lights\n",
           out->name, out->version, out->entity_count, out->piece_count,
           out->sprite_count, out->lighting.point_light_count);

    toml_free(root);
    return 0;
}
