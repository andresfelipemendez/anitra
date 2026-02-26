#ifndef GAME_H
#define GAME_H

#include "arena.h"
#include "debug_render.h"
#include "draw_list.h"
#include "gltf_types.h"
#include "editor.h"

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

typedef struct {
    int frames[10];
    rect collider;
    keyframe keyframes[2];
    float frame_time;
    int frame_count;
} animation_clip;

typedef struct animator {
     animation_clip animation;
     float timer;
     int   frame_index;
     int playing;
} animator;

typedef enum {
    COLLIDER
} collider_type;


typedef enum {
    ENEMY,
    PLAYER,
} entity_type;

typedef struct collider {
    rect rect;
    collider_type type;
} collider;

typedef struct entity {
    sprite_sheet sprite_sheet;
    animator current_animation;
    collider collider;
    sprite spr;
    vec2 pos;
    vec2 velocity;
    float health;
    entity_type type;
} entity;

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
    Mat4 *world_mats;
    Mat4 *skin_mats;       /* final skinning matrices — read by externals for GPU upload */

    /* Per-frame state — set by engine */
    uint32_t active_clip;
    float    anim_time;
    int      visible;

    /* Camera for 3D — set by engine */
    Vec3 camera_eye;
    Vec3 camera_target;
    Vec3 camera_up;

    /* Model transform — set by engine */
    Mat4 model_transform;
} mesh3d_state;

typedef struct memory {
  struct arena arena;
  struct arena *gameplay;       /* sub-arena for engine/gameplay allocations */
  struct arena *editor_arena;   /* sub-arena for editor/dock allocations */

  double _t_prev;

  debug_renderer debug_renderer;
  draw_list draw_list;

  camera camera;
  input_state input;

  int play;
  int width;
  int height;
  float dt;

  mesh3d_state mesh3d;

  /* GPU device handle — set by externals, used by engine for asset loading */
  void *gpu_device;

  /* Clay UI contexts — opaque Clay_Context*, set by externals init.
     Game Clay: for in-game UI (pause menu, HUD). Allocated from main arena.
     Editor Clay: for editor UI (profiler, panels). Allocated from editor_arena. */
  void *clay_game;
  void *clay_editor;

  /* Loaded 3D model — populated by engine init, read by externals for rendering */
  GltfModel loaded_model;

  /* Editor state — populated by editor.dll, read by externals for rendering */
  editor_state editor;
} memory;

#endif