# Sword Pickup with Bone Attachment

## Problem

The engine has no way to parent an entity to a skeleton joint. `parent_transform_component` parents entity-to-entity positions. A sword in a hand must follow the animated joint's world-space matrix each frame.

## Design

### New component: `bone_attach_component`

```c
typedef struct bone_attach_component {
    int entity_index;          /* the attached entity (sword) */
    int target_entity;         /* the entity whose skeleton drives us (player_mesh) */
    int joint_index;           /* which joint to follow (resolved at pickup time) */
    Vec3 offset_pos;           /* local position offset from joint */
    Quat offset_rot;           /* local rotation offset from joint */
} bone_attach_component;
```

Added to `game_state` as a flat array + capacity + count + index lookup, like all other components.

### New trigger type: `TRIGGER_WEAPON_PICKUP`

When the player enters the trigger radius:
1. Look up the target entity's `scene_model_asset` skeleton
2. Find the joint index by name (e.g. `"mixamorig:RightHand"`)
3. Create a `bone_attach_component` linking sword → target joint
4. Remove the trigger component (swap-and-pop, same as key)
5. The sword stays visible — its position is now bone-driven

### New system: `update_bone_attachments`

Runs after the anim SM system. For each `bone_attach_component`:
1. Find the target entity's `anim_instance` to get its `skin_mats_offset`
2. Find the `scene_model_asset` to get `inverse_bind[joint_index]`
3. Compute: `joint_world = entity_world × skin_mat × inverse(inverse_bind)`
   - `skin_mat = pool.skin_mats[offset + joint_index]` already contains `inverse_bind × world`, so `joint_world = entity_world × skin_mat × inverse(inverse_bind)`
   - Simpler: reuse `pool.world_mats[joint_index]` from the last anim SM pass (this IS the joint's local-to-world transform, relative to the model root)
4. Apply offset: `final = entity_world × world_mats[joint] × offset`
5. Write `final` into the sword's `transform_component.position` and rotation

Since `build_mesh_draw_commands` calls `resolve_world_transform` which reads `transform_component`, the sword renders at the joint position with no rendering changes.

### Project definition

```toml
[triggers]
"52" = { type = "weapon_pickup", target = 19, radius = 1.0, joint = "mixamorig:RightHand" }
```

- `target`: the entity with the skeleton (player_mesh)
- `joint`: bone name to attach to
- `radius`: pickup proximity

### System registration order

```
triggers           (play-mode only) — handles pickup activation
anim_sm            (play-mode only) — computes world_mats for joints
bone_attachments   (always)         — copies joint world_mats → transform
draw_lists         (always)         — reads transform to build draw commands
```

## Files to modify

| File | Changes |
|------|---------|
| `src/game.h` | Add `bone_attach_component` struct, arrays, capacity, count, index lookup; add `TRIGGER_WEAPON_PICKUP` to enum; add `joint` field to `project_trigger` |
| `src/project.h` | Add `joint` field to `project_trigger` |
| `src/project.c` | Parse `joint` string in `parse_trigger_table` |
| `src/engine/engine.c` | Add `push_bone_attach_component`, `update_bone_attachments` system, handle `TRIGGER_WEAPON_PICKUP` in `update_triggers`, register system |
| `dungeon1/project.toml` | Add sword entity + trigger definition |
| `tests/test_anim_sm.c` | Test bone attachment system |
