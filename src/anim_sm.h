#ifndef ANIM_SM_H
#define ANIM_SM_H

#include "gltf_types.h"

#define ANIM_SM_MAX_ENTITIES     256
#define ANIM_SM_MAX_JOINTS       128
#define ANIM_SM_MAX_STATES       16
#define ANIM_SM_MAX_RULES        64
#define ANIM_SM_MAX_BLENDS       128
#define ANIM_SM_STATE_NAME_MAX   32

/* Per-entity animation instance (replaces animation_component for SM entities) */
typedef struct {
    int   entity_index;
    int   model_asset_index;
    int   skin_mats_offset;    /* Mat4 offset into shared skin_mats pool */
    float speed;
    int   playing;
} anim_instance;

/* One state = one table of entities playing the same clip (SoA) */
typedef struct {
    char  name[ANIM_SM_STATE_NAME_MAX];
    int   clip_index;
    int   looping;
    int   entity_indices[ANIM_SM_MAX_ENTITIES];
    float anim_times[ANIM_SM_MAX_ENTITIES];
    int   count;
} anim_state_table;

/* Condition types for batch evaluation */
typedef enum {
    ANIM_COND_VELOCITY_ABOVE,
    ANIM_COND_VELOCITY_BELOW,
    ANIM_COND_CLIP_FINISHED,
    ANIM_COND_ALWAYS
} anim_condition_type;

/* Transition rule: "entities in from_state satisfying condition -> to_state" */
typedef struct {
    int   from_state;
    int   to_state;
    anim_condition_type condition;
    float threshold;
    float blend_duration;
} anim_transition_rule;

/* Entity currently blending between two states */
typedef struct {
    int   entity_index;
    int   from_clip;
    int   to_clip;
    float from_time;
    float to_time;
    float elapsed;
    float duration;
    float speed;
    int   to_state;            /* destination state table index */
} anim_blend_entry;

/* Scratch + output buffers (arena-allocated, re-init after hot-reload) */
typedef struct {
    Mat4 *skin_mats;           /* contiguous pool: entity N at offset skin_mats_offset */
    int   skin_mats_count;     /* total Mat4s allocated */
    Vec3 *pose_trans;          /* scratch: reused per entity (MAX_JOINTS) */
    Quat *pose_rot;
    Vec3 *pose_scale;
    Vec3 *blend_from_trans;    /* scratch for transition blending */
    Quat *blend_from_rot;
    Vec3 *blend_from_scale;
    Vec3 *blend_to_trans;
    Quat *blend_to_rot;
    Vec3 *blend_to_scale;
    Mat4 *world_mats;          /* scratch: MAX_JOINTS */
} anim_pose_pool;

/* Top-level SM — lives in game_state, fixed-size arrays survive hot-reload */
typedef struct {
    anim_state_table     states[ANIM_SM_MAX_STATES];
    int                  state_count;
    anim_transition_rule rules[ANIM_SM_MAX_RULES];
    int                  rule_count;
    anim_instance        instances[ANIM_SM_MAX_ENTITIES];
    int                  instance_count;
    anim_blend_entry     blends[ANIM_SM_MAX_BLENDS];
    int                  blend_count;
    anim_pose_pool       pool;
    int                  pool_initialized;
    int                  next_skin_mats_offset; /* next free Mat4 offset in pool */
} anim_sm;

#endif /* ANIM_SM_H */
