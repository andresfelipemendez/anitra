#ifndef GAME_H
#define GAME_H

#include "debug_render.h"
#include "draw_list.h"

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

typedef struct game {
  double _t_prev;

  debug_renderer debug_renderer;
  entity entities[8];
  draw_list draw_list;

  camera camera;
  input_state input;

  int play;
  int width;
  int height;
  float dt;
} game;

#endif