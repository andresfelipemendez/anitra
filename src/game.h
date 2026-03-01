#ifndef GAME_H
#define GAME_H

#include "arena.h"
#include "debug_render.h"
#include "draw_list.h"
#include "gltf_types.h"
#include "editor/editor.h"
#include "project.h"

#ifndef __cplusplus
#include <stdbool.h>
#endif

typedef struct {
    int x, y, w, h;
} pixel_rect;

typedef struct {
    float x, y, w, h;  
} rect;

typedef struct {
    int texture_id;
    rect coords;
} sprite;

typedef enum {
    TEXTURE_PLAYER,
    TEXTURE_TILES,
    TEXTURE_SLIME,
    TEXTURE_HEALTH_BAR,
    TEXTURE_HEALTH_FILL,
    TEXTURE_COUNT 
} TextureID;

typedef struct sprite_sheet {
    TextureID texture_id;
    int width;
    int height;
    pixel_rect sprites[64];
} sprite_sheet;

typedef struct keyframe {
    int frame;
} keyframe;

/* Animation clip with duration */
typedef struct {
    int frames[10];
    rect collider;
    keyframe keyframes[2];
    float frame_time;
    int frame_count;
    float duration;  /* in seconds */
} animation_clip;

typedef struct animator {
     animation_clip animation;
     float timer;
     int   frame_index;
     int playing;
} animator;

typedef enum {
    COLLIDER_BOX,
    COLLIDER_CAPSULE
} collider_type;


typedef struct collider {
    rect rect;
    collider_type type;
} collider;

typedef struct entity {
    sprite_sheet sprite_sheet;
    animator current_animation;
    sprite spr;
} entity;

typedef struct parent_component {
    int entity_index;
    int parent_entity_index;
} parent_component;

typedef struct parent_transform_component {
    int entity_index;
    int parent_entity_index;
} parent_transform_component;

typedef struct parent_rotation_component {
    int entity_index;
} parent_rotation_component;

typedef struct mesh_component {
    int entity_index;
    int visible;
    int model_asset_index;
} mesh_component;

typedef struct transform_component {
    int entity_index;
    Vec3 position;
} transform_component;

typedef struct rotation_component {
    int entity_index;
    float rotation_y_deg;
} rotation_component;

typedef struct scale_component {
    int entity_index;
    Vec3 scale;
} scale_component;

typedef struct velocity_component {
    int entity_index;
    vec2 velocity;
} velocity_component;

typedef struct rigid_body_component {
    int entity_index;
    int use_gravity;
} rigid_body_component;

typedef struct character_controller_component {
    int entity_index;
    float move_speed;
    float jump_speed;
} character_controller_component;

typedef struct health_component {
    int entity_index;
    float health;
    float max_health;
} health_component;

typedef struct collider_component {
    int entity_index;
    rect rect;
    collider_type type;
    float half_height;
} collider_component;

typedef struct box_collider_component {
    int entity_index;
    rect rect;
} box_collider_component;

typedef struct capsule_collider_component {
    int entity_index;
    float radius;
    float half_height;
    rect aabb;
} capsule_collider_component;

typedef struct animation_component {
    int entity_index;
    int playing;
    int active_clip;
    float anim_time;
    float speed;
} animation_component;

typedef struct animation_transition_entry {
    int entity_index;
    int from_clip;
    int to_clip;
    float from_time;
    float to_time;
    float elapsed;
    float duration;
} animation_transition_entry;

typedef struct camera_component {
    int entity_index;
    float fov_deg;
    float near_plane;
    float far_plane;
    Vec3 target;
    Vec3 up;
} camera_component;

typedef enum InputButton {
    INPUT_A = 1 << 0,
    INPUT_B = 1 << 1,
    INPUT_X = 1 << 2,
    INPUT_Y = 1 << 3,
} InputButton;

typedef struct input_state {
    float horizontal;
    float vertical;
    int input_mask;
} input_state;

typedef struct camera {
    vec2 position;
    float zoom;
} camera;

/* ── Lighting state ── */

typedef struct point_light {
    Vec3 position;
    Vec3 color;
    float intensity;
    float radius;
} point_light;

typedef struct lighting_state {
    Vec3 ambient;
    point_light lights[8];
    int light_count;
} lighting_state;

/* ── 3D mesh state (populated by externals, animated by engine) ── */

typedef struct mesh3d_state {
    /* Model data — set once by externals during init */
    Skeleton  skeleton;
    AnimClip *clips;
    uint32_t  clip_count;
    uint32_t  primitive_count;  /* for externals to know if there's anything to draw */

    /* Animation scratch buffers (arena-allocated by externals) */
    Vec3 *pose_trans;
    Quat *pose_rot;
    Vec3 *pose_scale;
    Vec3 *blend_from_trans;
    Quat *blend_from_rot;
    Vec3 *blend_from_scale;
    Vec3 *blend_to_trans;
    Quat *blend_to_rot;
    Vec3 *blend_to_scale;
    Mat4 *world_mats;
    Mat4 *skin_mats;       /* final skinning matrices — read by externals for GPU upload */

    /* Per-frame state — set by engine */
    uint32_t active_clip;
    float    anim_time;
    int      visible;

    /* Camera for 3D — set by engine, or by project file */
    Vec3 camera_eye;
    Vec3 camera_target;
    Vec3 camera_up;
    float camera_fov_deg;
    float camera_near;
    float camera_far;
    int  camera_set_by_project;

    /* Model transform — set by engine */
    Mat4 model_transform;
} mesh3d_state;

#define SCENE_MODEL_ASSET_MAX 64

typedef struct scene_model_asset {
    char key[64];
    char path[256];
    char animation_path[256];
    int has_animation_path;
    int loaded;
    int has_skeleton;
    GltfModel model;
} scene_model_asset;

typedef struct game_state {
  struct arena *root_arena;     /* pointer to memory.arena */
  struct arena *gameplay;       /* sub-arena for engine allocations */
  struct mig_header *mig_hdr;   /* game_state migration header — persists in gameplay arena */

  double _t_prev;

  debug_renderer dbg;
  draw_list dl;

  camera camera;
  input_state input;

  int play;
  int editor_play_mode; /* 0 = edit mode, 1 = play mode */
  int width;
  int height;
  float dt;

  /* Scene entity/component views (owned by engine's scene storage) */
  entity *scene_entities;
  int scene_entity_count;
  int scene_entity_capacity;
  parent_component *parent_components;
  int parent_component_count;
  int parent_component_capacity;
  parent_transform_component *parent_transform_components;
  int parent_transform_component_count;
  int parent_transform_component_capacity;
  parent_rotation_component *parent_rotation_components;
  int parent_rotation_component_count;
  int parent_rotation_component_capacity;
  transform_component *transform_components;
  int transform_component_count;
  int transform_component_capacity;
  rotation_component *rotation_components;
  int rotation_component_count;
  int rotation_component_capacity;
  scale_component *scale_components;
  int scale_component_count;
  int scale_component_capacity;
  velocity_component *velocity_components;
  int velocity_component_count;
  int velocity_component_capacity;
  rigid_body_component *rigid_body_components;
  int rigid_body_component_count;
  int rigid_body_component_capacity;
  character_controller_component *character_controller_components;
  int character_controller_component_count;
  int character_controller_component_capacity;
  health_component *health_components;
  int health_component_count;
  int health_component_capacity;
  box_collider_component *box_collider_components;
  int box_collider_component_count;
  int box_collider_component_capacity;
  capsule_collider_component *capsule_collider_components;
  int capsule_collider_component_count;
  int capsule_collider_component_capacity;
  mesh_component *mesh_components;
  int mesh_component_count;
  int mesh_component_capacity;
  animation_component *animation_components;
  int animation_component_count;
  int animation_component_capacity;
  animation_transition_entry *animation_transition_entries;
  int animation_transition_count;
  int animation_transition_capacity;
  camera_component *camera_components;
  int camera_component_count;
  int camera_component_capacity;

  mesh3d_state mesh3d;
  lighting_state lighting;
  GltfModel loaded_model;       /* loaded 3D model data */
  GltfModel loaded_floor_model; /* loaded floor tile model data */
  scene_model_asset scene_model_assets[SCENE_MODEL_ASSET_MAX];
  int scene_model_asset_count;
  int scene_primary_skinned_entity;
  int scene_camera_entity;
  project_data project;
  int project_loaded;
  char project_path[512];

  /* GPU device handle — set by externals, used by engine for asset loading */
  void *gpu_device;

  /* Clay UI context — opaque Clay_Context*, set by externals init.
     Game Clay: for in-game UI (pause menu, HUD). Allocated from main arena. */
  void *clay_game;

  /* Asset paths — loaded from config file at startup */
  const char *default_model_path;
  const char *default_animation_path;
  const char *default_floor_model_path;
  const char *texture_player;
  const char *texture_tiles;
  const char *texture_slime;
  const char *texture_health_bar;
  const char *texture_health_fill;
  const char *font_editor;
  const char *shader_sprite_vs;
  const char *shader_sprite_fs;
  const char *shader_debug_lines_vs;
  const char *shader_debug_lines_fs;
  const char *shader_ui_rect_vs;
  const char *shader_ui_rect_fs;
  const char *shader_font_vs;
  const char *shader_font_fs;
  const char *shader_mesh_vs;
  const char *shader_mesh_fs;
  const char *shader_composite_vs;
  const char *shader_composite_fs;
} game_state;

typedef struct memory {
  struct arena arena;
  game_state game;
  editor_state editor;
} memory;

#endif /* GAME_H */
