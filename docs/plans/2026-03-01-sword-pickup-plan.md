# Sword Pickup with Bone Attachment — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a sword entity that the player can pick up by walking over it. On pickup, the sword attaches to the right hand joint and follows the skeleton animation.

**Architecture:** New `bone_attach_component` + `update_bone_attachments` system. On pickup, the trigger handler creates a bone_attach linking sword → player_mesh's hand joint. Each frame, the attachment system reads the target's `skin_mats` from the anim SM pool, recovers the joint's model-space transform via `mat4_affine_inverse(inverse_bind)`, and writes the final world position into the sword's `transform_component`.

**Tech Stack:** C89 (TCC compiler), single-translation-unit build, arena allocator, ECS

---

### Task 1: Add `mat4_affine_inverse` to math3d.h

**Files:**
- Modify: `src/math3d.h` (after `mat4_from_floats`, ~line 217)

**Step 1: Add the function**

Column-major affine inverse: transpose the 3×3 rotation/scale block, negate the rotated translation.

```c
static inline Mat4 mat4_affine_inverse(Mat4 m) {
    Mat4 r = {0};
    float det, inv_det;
    /* Transpose upper-left 3x3 with cofactor for general affine (R*S) */
    float a00 = m.m[0], a01 = m.m[4], a02 = m.m[8];
    float a10 = m.m[1], a11 = m.m[5], a12 = m.m[9];
    float a20 = m.m[2], a21 = m.m[6], a22 = m.m[10];
    float tx  = m.m[12], ty = m.m[13], tz = m.m[14];

    /* Cofactors of upper-left 3x3 */
    float c00 = a11*a22 - a12*a21;
    float c01 = a12*a20 - a10*a22;
    float c02 = a10*a21 - a11*a20;
    float c10 = a02*a21 - a01*a22;
    float c11 = a00*a22 - a02*a20;
    float c12 = a01*a20 - a00*a21;
    float c20 = a01*a12 - a02*a11;
    float c21 = a02*a10 - a00*a12;
    float c22 = a00*a11 - a01*a10;

    det = a00*c00 + a01*c01 + a02*c02;
    inv_det = (det != 0.0f) ? 1.0f / det : 0.0f;

    r.m[0]  = c00 * inv_det; r.m[1]  = c01 * inv_det; r.m[2]  = c02 * inv_det;
    r.m[4]  = c10 * inv_det; r.m[5]  = c11 * inv_det; r.m[6]  = c12 * inv_det;
    r.m[8]  = c20 * inv_det; r.m[9]  = c21 * inv_det; r.m[10] = c22 * inv_det;
    r.m[12] = -(r.m[0]*tx + r.m[4]*ty + r.m[8]*tz);
    r.m[13] = -(r.m[1]*tx + r.m[5]*ty + r.m[9]*tz);
    r.m[14] = -(r.m[2]*tx + r.m[6]*ty + r.m[10]*tz);
    r.m[15] = 1.0f;
    return r;
}
```

**Step 2: Verify tests still compile and pass**

```bash
lib/tcc/macos/tcc -Blib/tcc/macos -DMAC_OS_X_VERSION_MIN_REQUIRED=1100 \
  -o build/Debug/test_anim_sm -Isrc -Isrc/engine -Isrc/editor \
  -Ilib/SDL3/include -Ilib/cgltf tests/test_anim_sm.c && build/Debug/test_anim_sm
```

---

### Task 2: Add `bone_attach_component` to game.h

**Files:**
- Modify: `src/game.h`

**Step 1: Add the struct** (after `trigger_component`, ~line 195)

```c
typedef struct bone_attach_component {
    int entity_index;       /* the attached entity (sword) */
    int target_entity;      /* entity with skeleton (player_mesh) */
    int joint_index;        /* skeleton joint to follow */
    Vec3 offset_pos;        /* local offset from joint */
    Quat offset_rot;        /* local rotation offset */
} bone_attach_component;
```

**Step 2: Add arrays to game_state** (after trigger arrays, ~line 380)

```c
  bone_attach_component *bone_attach_components;
  int bone_attach_component_count;
  int bone_attach_component_capacity;
  int bone_attach_index[PROJECT_COMP_MAX];
```

**Step 3: Add `TRIGGER_WEAPON_PICKUP` to trigger_type enum** (~line 187)

```c
typedef enum { TRIGGER_PICKUP, TRIGGER_DOOR, TRIGGER_WEAPON_PICKUP } trigger_type;
```

**Step 4: Verify tests still compile**

Same build command as Task 1 step 2.

---

### Task 3: Add `joint` field to project_trigger, parse it

**Files:**
- Modify: `src/project.h:67` — add `joint` field
- Modify: `src/project.c:621` — parse `joint` string in `parse_trigger_table`

**Step 1: Add joint field to project_trigger** (`src/project.h:67`)

Change:
```c
typedef struct { int entity; char type_str[16]; int target; float radius; } project_trigger;
```
To:
```c
typedef struct { int entity; char type_str[16]; int target; float radius; char joint[64]; } project_trigger;
```

**Step 2: Parse joint in parse_trigger_table** (`src/project.c`, inside the for loop, after radius parsing)

Add after line 646:
```c
        copy_toml_string(entry, "joint", tr->joint, sizeof(tr->joint));
```

**Step 3: Verify compile**

Same build command.

---

### Task 4: Add `bone_attach` to `ensure_scene_storage` and component push/find helpers

**Files:**
- Modify: `src/engine/engine.c`

**Step 1: Add `reserve_array` call in `ensure_scene_storage`** (after trigger_components)

```c
    err = reserve_array(gs->gameplay, (void **)&gs->bone_attach_components,
        &gs->bone_attach_component_capacity, needed_entities,
        sizeof(bone_attach_component), "bone_attach_components");
    if (!ERRV_IS_OK(err)) return err;
```

**Step 2: Add `clear_scene_storage` memset** (after trigger_components clearing)

```c
    if (gs->bone_attach_components && gs->bone_attach_component_capacity > 0) {
        memset(gs->bone_attach_components, 0,
               (size_t)gs->bone_attach_component_capacity * sizeof(bone_attach_component));
    }
```

**Step 3: Add `push_bone_attach_component` helper** (near `push_trigger_component`)

```c
static void push_bone_attach_component(game_state *gs, int entity_index,
                                        int target_entity, int joint_index,
                                        Vec3 offset_pos, Quat offset_rot) {
    int i;
    if (!gs) return;
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
```

**Step 4: Init `bone_attach_index` in test helper** (`tests/test_anim_sm.c:setup_game_state`)

Add with the other memset calls:
```c
    memset(gs->bone_attach_index, 0xFF, sizeof(gs->bone_attach_index));
```

**Step 5: Verify compile + tests pass**

---

### Task 5: Implement `update_bone_attachments` system

**Files:**
- Modify: `src/engine/engine.c`

**Step 1: Add `find_joint_by_name` helper** (near other find helpers)

```c
static int find_joint_by_name(Skeleton *skel, const char *name) {
    uint32_t i;
    if (!skel || !skel->joint_names || !name) return -1;
    for (i = 0; i < skel->joint_count; i++) {
        if (skel->joint_names[i] && strcmp(skel->joint_names[i], name) == 0)
            return (int)i;
    }
    return -1;
}
```

**Step 2: Add `update_bone_attachments` system**

Pipeline per attachment:
1. Find target entity's `anim_instance` → `skin_mats_offset`
2. Find target entity's `scene_model_asset` → `skeleton`, `inverse_bind`, `armature_transform`
3. `skin_mat = pool.skin_mats[offset + joint_index]`
4. `joint_model = skin_mat × mat4_affine_inverse(inverse_bind[joint_index])`
5. `entity_world = resolve_world_transform(gs, target_entity)`
6. `joint_world = entity_world × armature_transform × joint_model`
7. Apply offset: `final = joint_world × mat4_from_trs(offset_pos, offset_rot, VEC3(1,1,1))`
8. Write position from `final.m[12..14]` into sword's `transform_component`

```c
static void update_bone_attachments(game_state *gs) {
    int i;
    if (!gs || gs->bone_attach_component_count <= 0) return;

    for (i = 0; i < gs->bone_attach_component_count; i++) {
        bone_attach_component *ba = &gs->bone_attach_components[i];
        anim_instance *inst;
        mesh_component *mc;
        scene_model_asset *asset;
        transform_component *tc;
        Mat4 skin_mat, inv_bind, joint_model, entity_world, joint_world, offset_mat, final;

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
        final = mat4_mul(joint_world, offset_mat);

        tc = find_transform_component(gs, ba->entity_index);
        if (tc) {
            tc->position.x = final.m[12];
            tc->position.y = final.m[13];
            tc->position.z = final.m[14];
        }

        /* Clear parent_transform so resolve_world_transform doesn't double-add */
        {
            parent_transform_component *pt = find_parent_transform_component(gs, ba->entity_index);
            if (pt) {
                /* Remove from parent hierarchy — bone attachment drives position now */
                pt->parent_entity_index = -1;
            }
        }
    }
}
```

**Step 3: Register the system** (after `animation_sm`, before `movement`)

In `init_engine`, after the `register_system("animation_sm", ...)` line:
```c
    register_system(gs, "bone_attachments", (system_fn)update_bone_attachments, 0);
```

**Step 4: Add forward declaration** (at top with other forward declarations)

```c
static void update_bone_attachments(game_state *gs);
```

**Step 5: Verify compile + tests pass**

---

### Task 6: Handle `TRIGGER_WEAPON_PICKUP` in `update_triggers`

**Files:**
- Modify: `src/engine/engine.c:2207` — `update_triggers` function

**Step 1: Add weapon pickup handler** (after the existing TRIGGER_PICKUP block, before the loop increment)

When the player enters the trigger radius:
1. Look up the target entity's skeleton
2. Find the joint index by matching `joint_names` against the joint name stored in the project trigger
3. Create a `bone_attach_component`
4. Remove the trigger (swap-and-pop)

The tricky part: we need the joint name at runtime. The project_trigger has it, but after `build_scene_from_project` the trigger_component doesn't. We need to store the joint name and target entity in the trigger_component.

**Step 1a: Extend `trigger_component`** (in `game.h`)

Add fields to `trigger_component`:
```c
typedef struct trigger_component {
    int entity_index;
    trigger_type type;
    int target_entity;
    float radius;
    int activated;
    char joint_name[64];       /* for TRIGGER_WEAPON_PICKUP: bone to attach to */
} trigger_component;
```

**Step 1b: Pass joint_name through `push_trigger_component`**

Change signature to accept `const char *joint_name`:
```c
static void push_trigger_component(game_state *gs, int entity_index,
                                   trigger_type type, int target_entity,
                                   float radius, const char *joint_name)
```

Inside, after setting radius:
```c
    gs->trigger_components[i].joint_name[0] = '\0';
    if (joint_name && joint_name[0])
        strncpy(gs->trigger_components[i].joint_name, joint_name,
                sizeof(gs->trigger_components[i].joint_name) - 1);
```

Update all existing call sites to pass `NULL` for `joint_name`.

**Step 1c: In `build_scene_from_project`**, pass the joint name for weapon_pickup triggers:

Change the trigger type resolution:
```c
        if (strcmp(tr->type_str, "pickup") == 0) ttype = TRIGGER_PICKUP;
        else if (strcmp(tr->type_str, "weapon_pickup") == 0) ttype = TRIGGER_WEAPON_PICKUP;
        push_trigger_component(gs, tr->entity, ttype, tr->target, tr->radius,
                               tr->joint[0] ? tr->joint : NULL);
```

**Step 2: Add weapon pickup handling in `update_triggers`**

After the existing `if (trig->type != TRIGGER_PICKUP)` block:
```c
        if (trig->type == TRIGGER_WEAPON_PICKUP) {
            mesh_component *target_mc;
            scene_model_asset *target_asset;
            int joint_idx;

            trig_pos = resolve_world_position(gs, trig->entity_index);
            dx = player_pos.x - trig_pos.x;
            dz = player_pos.z - trig_pos.z;
            dist_sq = dx * dx + dz * dz;
            r_sq = trig->radius * trig->radius;
            if (dist_sq >= r_sq) { i++; continue; }

            /* Find the target entity's skeleton and joint */
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
            continue; /* re-examine swapped element */
        }

        if (trig->type != TRIGGER_PICKUP) { i++; continue; }
```

**Step 3: Verify compile + tests pass**

---

### Task 7: Add sword entity to project.toml

**Files:**
- Modify: `dungeon1/project.toml`

**Step 1: Add sword to entity list** (append to list, will be entity index 63)

```toml
  "sword"
```

**Step 2: Add sword model asset** (in `[assets.models]`)

```toml
sword = "<path-to-sword.glb>"
```

The user provides the actual path to their sword mesh file.

**Step 3: Add sword transform** (in `[transforms]`)

Place the sword on the ground somewhere in the dungeon:
```toml
"63" = { position = [0.0000, -0.8000, 0.0000] }
```

**Step 4: Add sword mesh component** (in `[meshes]`)

```toml
"63" = { model = "sword" }
```

**Step 5: Add sword trigger** (in `[triggers]`)

Target entity 19 = `player_mesh` (has the skeleton). Joint name from KayKit Mixamo rig:
```toml
"63" = { type = "weapon_pickup", target = 19, radius = 1.0, joint = "mixamorig:RightHand" }
```

**Step 6: Add sword parent_transform** (in `[parent_transform]`)

Parent to floor_root so it sits in the dungeon:
```toml
"63" = { parent = 2 }
```

---

### Task 8: Add error path tests for bone attachment

**Files:**
- Modify: `tests/test_anim_sm.c`

**Step 1: Add test for bone attachment with valid joint**

```c
TEST(bone_attach_updates_transform_from_joint) {
    game_state *gs = setup_game_state();
    anim_sm *sm = &gs->anim;
    scene_model_asset *asset;

    init_test_clips();
    setup_test_model_asset(gs);
    asset = &gs->scene_model_assets[0];

    /* Set up joint_names so find_joint_by_name works */
    {
        static const char *names[2] = {"Root", "RightHand"};
        asset->model.skeleton.joint_names = names;
    }

    anim_sm_init_pool(sm, gs->gameplay);
    anim_sm_add_state(sm, "idle", 0, 1, NULL);
    add_animated_entity(gs, 0, 0, 0, 1.0f);
    anim_sm_register_entity(sm, 0, 0, 0, 2);

    /* Add a sword entity with transform */
    gs->scene_entity_count = 2;
    {
        int ti = gs->transform_component_count++;
        gs->transform_components[ti].entity_index = 1;
        gs->transform_components[ti].position = VEC3(5, 0, 5);
        gs->transform_index[1] = ti;
    }

    /* Attach sword (entity 1) to entity 0's joint 1 */
    push_bone_attach_component(gs, 1, 0, 1, VEC3(0,0,0), QUAT(0,0,0,1));
    ASSERT_EQ(gs->bone_attach_component_count, 1);

    /* Run anim SM to compute skin_mats */
    anim_sm_update(sm, gs, 1.0f/60.0f);

    /* Run bone attachment system */
    update_bone_attachments(gs);

    /* Sword transform should have changed from (5,0,5) */
    {
        transform_component *tc = find_transform_component(gs, 1);
        ASSERT(tc != NULL);
        /* Position should reflect joint 1's animated position, not original (5,0,5) */
        ASSERT(tc->position.x != 5.0f || tc->position.y != 0.0f || tc->position.z != 5.0f);
    }
}
```

**Step 2: Register test in main()**

```c
    /* Bone attachment */
    run_bone_attach_updates_transform_from_joint();
```

**Step 3: Verify compile + tests pass**

```bash
lib/tcc/macos/tcc -Blib/tcc/macos -DMAC_OS_X_VERSION_MIN_REQUIRED=1100 \
  -o build/Debug/test_anim_sm -Isrc -Isrc/engine -Isrc/editor \
  -Ilib/SDL3/include -Ilib/cgltf tests/test_anim_sm.c && build/Debug/test_anim_sm
```

---

### Task 9: Build engine DLL and verify

**Step 1: Compile engine DLL**

```bash
lib/tcc/macos/tcc -Blib/tcc/macos -shared -DMAC_OS_X_VERSION_MIN_REQUIRED=1100 -DSTBI_NO_THREAD_LOCALS \
  -o build/Debug/libengine.dylib -Isrc -Isrc/engine -Isrc/editor -Ilib/SDL3/include -Ilib/cgltf src/engine/*.c
```

**Step 2: Run test binary**

```bash
build/Debug/test_anim_sm
```

All tests should pass.

---

## Notes

- The joint name `"mixamorig:RightHand"` is the standard Mixamo rig naming. If the KayKit models use different joint names, the user will see the "could not find joint" warning and can adjust.
- `offset_pos` and `offset_rot` default to zero/identity. The user can tune these in-editor later to position the sword grip correctly in the hand.
- The `update_bone_attachments` system runs every frame (not play-mode-only) so the sword stays attached in edit mode too after pickup.
