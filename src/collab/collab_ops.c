#include "collab_ops.h"
#include "game.h"
#include <string.h>

/* ── Apply Operations to game_state ── */

void collab_op_apply(const collab_op *op, struct game_state *gs) {
    int eidx = op->entity_index;
    int cidx;

    if (!gs || eidx < 0 || eidx >= COLLAB_COMP_MAX) return;

    switch ((collab_op_type)op->type) {
    case OP_SET_TRANSFORM: {
        op_vec3_payload p;
        if (op->payload_len < sizeof(p)) return;
        memcpy(&p, op->payload, sizeof(p));
        cidx = gs->transform_index[eidx];
        if (cidx >= 0) {
            gs->transform_components[cidx].position.x = p.x;
            gs->transform_components[cidx].position.y = p.y;
            gs->transform_components[cidx].position.z = p.z;
        }
        break;
    }
    case OP_SET_ROTATION: {
        op_float_payload p;
        if (op->payload_len < sizeof(p)) return;
        memcpy(&p, op->payload, sizeof(p));
        cidx = gs->rotation_index[eidx];
        if (cidx >= 0)
            gs->rotation_components[cidx].rotation_y_deg = p.value;
        break;
    }
    case OP_SET_SCALE: {
        op_vec3_payload p;
        if (op->payload_len < sizeof(p)) return;
        memcpy(&p, op->payload, sizeof(p));
        cidx = gs->scale_index[eidx];
        if (cidx >= 0) {
            gs->scale_components[cidx].scale.x = p.x;
            gs->scale_components[cidx].scale.y = p.y;
            gs->scale_components[cidx].scale.z = p.z;
        }
        break;
    }
    case OP_SET_CAPSULE_RADIUS: {
        op_float_payload p;
        if (op->payload_len < sizeof(p)) return;
        memcpy(&p, op->payload, sizeof(p));
        cidx = gs->capsule_collider_index[eidx];
        if (cidx >= 0)
            gs->capsule_collider_components[cidx].radius = p.value;
        break;
    }
    case OP_SET_CAPSULE_HEIGHT: {
        op_float_payload p;
        if (op->payload_len < sizeof(p)) return;
        memcpy(&p, op->payload, sizeof(p));
        cidx = gs->capsule_collider_index[eidx];
        if (cidx >= 0)
            gs->capsule_collider_components[cidx].half_height = p.value;
        break;
    }
    case OP_SET_PARENT: {
        op_parent_payload p;
        if (op->payload_len < sizeof(p)) return;
        memcpy(&p, op->payload, sizeof(p));
        {
            int i;
            int found = 0;
            for (i = 0; i < gs->parent_transform_component_count; i++) {
                if (gs->parent_transform_components[i].entity_index == eidx) {
                    if (p.parent_entity < 0) {
                        int j;
                        for (j = i; j < gs->parent_transform_component_count - 1; j++)
                            gs->parent_transform_components[j] = gs->parent_transform_components[j + 1];
                        gs->parent_transform_component_count--;
                        gs->parent_transform_index[eidx] = -1;
                    } else {
                        gs->parent_transform_components[i].parent_entity_index = p.parent_entity;
                    }
                    found = 1;
                    break;
                }
            }
            if (!found && p.parent_entity >= 0 &&
                gs->parent_transform_component_count < gs->parent_transform_component_capacity) {
                int idx = gs->parent_transform_component_count;
                gs->parent_transform_components[idx].entity_index = eidx;
                gs->parent_transform_components[idx].parent_entity_index = p.parent_entity;
                gs->parent_transform_index[eidx] = idx;
                gs->parent_transform_component_count++;
            }
        }
        break;
    }
    default:
        break;
    }
}

/* ── Snapshot Serialization ──
   Format:
     [4: scene_entity_count]
     [scene_entity_count * 64: entity names]
     -- For each component type:
     [4: count]
     [count * sizeof(component): component data]
     -- Index tables:
     [COLLAB_COMP_MAX * 4: transform_index]
     [COLLAB_COMP_MAX * 4: rotation_index]
     [COLLAB_COMP_MAX * 4: scale_index]
     [COLLAB_COMP_MAX * 4: capsule_collider_index]
     [COLLAB_COMP_MAX * 4: parent_transform_index]
*/

#define SNAP_WRITE(dst, src, n) do { memcpy(dst, src, n); dst += (n); } while(0)
#define SNAP_READ(dst, src, n)  do { memcpy(dst, src, n); src += (n); } while(0)
#define SNAP_WRITE_I32(dst, v)  do { int32_t _v = (v); SNAP_WRITE(dst, &_v, 4); } while(0)
#define SNAP_READ_I32(src, v)   do { SNAP_READ(&(v), src, 4); } while(0)

int collab_snapshot_serialize(const struct game_state *gs, uint8_t *buf, int buf_size) {
    uint8_t *p = buf;
    int max = collab_snapshot_max_size();
    if (buf_size < max) return -1;

    SNAP_WRITE_I32(p, gs->scene_entity_count);
    SNAP_WRITE(p, gs->project.scene_entity_names, gs->scene_entity_count * 64);

    SNAP_WRITE_I32(p, gs->transform_component_count);
    SNAP_WRITE(p, gs->transform_components,
               gs->transform_component_count * (int)sizeof(transform_component));

    SNAP_WRITE_I32(p, gs->rotation_component_count);
    SNAP_WRITE(p, gs->rotation_components,
               gs->rotation_component_count * (int)sizeof(rotation_component));

    SNAP_WRITE_I32(p, gs->scale_component_count);
    SNAP_WRITE(p, gs->scale_components,
               gs->scale_component_count * (int)sizeof(scale_component));

    SNAP_WRITE_I32(p, gs->capsule_collider_component_count);
    SNAP_WRITE(p, gs->capsule_collider_components,
               gs->capsule_collider_component_count * (int)sizeof(capsule_collider_component));

    SNAP_WRITE_I32(p, gs->parent_transform_component_count);
    SNAP_WRITE(p, gs->parent_transform_components,
               gs->parent_transform_component_count * (int)sizeof(parent_transform_component));

    SNAP_WRITE(p, gs->transform_index, COLLAB_COMP_MAX * 4);
    SNAP_WRITE(p, gs->rotation_index, COLLAB_COMP_MAX * 4);
    SNAP_WRITE(p, gs->scale_index, COLLAB_COMP_MAX * 4);
    SNAP_WRITE(p, gs->capsule_collider_index, COLLAB_COMP_MAX * 4);
    SNAP_WRITE(p, gs->parent_transform_index, COLLAB_COMP_MAX * 4);

    return (int)(p - buf);
}

int collab_snapshot_deserialize(struct game_state *gs, const uint8_t *buf, int buf_len) {
    const uint8_t *p = buf;
    int32_t count;
    (void)buf_len;

    SNAP_READ_I32(p, count);
    if (count < 0 || count > COLLAB_COMP_MAX) return -1;
    gs->scene_entity_count = count;
    gs->project.scene_entity_count = count;
    SNAP_READ(gs->project.scene_entity_names, p, count * 64);

    SNAP_READ_I32(p, count);
    if (count < 0 || count > COLLAB_COMP_MAX) return -1;
    gs->transform_component_count = count;
    SNAP_READ(gs->transform_components, p, count * (int)sizeof(transform_component));

    SNAP_READ_I32(p, count);
    if (count < 0 || count > COLLAB_COMP_MAX) return -1;
    gs->rotation_component_count = count;
    SNAP_READ(gs->rotation_components, p, count * (int)sizeof(rotation_component));

    SNAP_READ_I32(p, count);
    if (count < 0 || count > COLLAB_COMP_MAX) return -1;
    gs->scale_component_count = count;
    SNAP_READ(gs->scale_components, p, count * (int)sizeof(scale_component));

    SNAP_READ_I32(p, count);
    if (count < 0 || count > COLLAB_COMP_MAX) return -1;
    gs->capsule_collider_component_count = count;
    SNAP_READ(gs->capsule_collider_components, p, count * (int)sizeof(capsule_collider_component));

    SNAP_READ_I32(p, count);
    if (count < 0 || count > COLLAB_COMP_MAX) return -1;
    gs->parent_transform_component_count = count;
    SNAP_READ(gs->parent_transform_components, p, count * (int)sizeof(parent_transform_component));

    SNAP_READ(gs->transform_index, p, COLLAB_COMP_MAX * 4);
    SNAP_READ(gs->rotation_index, p, COLLAB_COMP_MAX * 4);
    SNAP_READ(gs->scale_index, p, COLLAB_COMP_MAX * 4);
    SNAP_READ(gs->capsule_collider_index, p, COLLAB_COMP_MAX * 4);
    SNAP_READ(gs->parent_transform_index, p, COLLAB_COMP_MAX * 4);

    return 0;
}
