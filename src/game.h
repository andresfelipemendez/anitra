#ifndef GAME_H
#define GAME_H

#include "debug_render.h"
#include <stdbool.h>
#include <SDL3/SDL.h>

typedef struct {
    int x, y, w, h;
} pixel_rect;

typedef struct {
    float x, y, w, h;
} rect;

typedef struct {
    SDL_Texture* texture;
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

struct sprite_sheet {
    TextureID texture_id;
    int width;
    int height;
    pixel_rect sprites[64];
};

struct keyframe {
    int frame;
};

typedef struct {
    int frames[10];
    rect collider;
    struct keyframe keyframes[2];
    float frame_time;
    int frame_count;
} animation_clip;

struct animator {
     animation_clip animation;
     float timer;
     int   frame_index;
     bool playing;
};

typedef enum {
    COLLIDER
} collider_type;

typedef enum {
    ENEMY,
    PLAYER,
} entity_type;

struct collider {
    rect rect;
    collider_type type;
};

typedef struct entity {
    struct sprite_sheet sprite_sheet;
    struct animator current_animation;
    struct collider collider;
    sprite spr;
    vec2 pos;
    vec2 velocity;
    float health;
    entity_type type;
} entity;

struct game;

typedef void (*render_entities_func)(struct game *g);
typedef void (*render_sprite_func)(struct game *g, SDL_Texture* texture, float x, float y);
typedef SDL_Texture* (*load_texture_func)(struct game *g, const char* filepath);

enum InputButton {
    INPUT_A = 1 << 0,
    INPUT_B = 1 << 1,
    INPUT_X = 1 << 2,
    INPUT_Y = 1 << 3,
};

struct input_state {
    float horizontal;
    float vertical;
    int input_mask;
};

struct camera {
    vec2 position;
    float zoom;
};

struct game {
  SDL_Window *window;
  SDL_Renderer *renderer;
  void *engine_lib;
  render_entities_func render_entities;
  render_sprite_func render_sprite;
  load_texture_func load_texture;
  double _t_prev;
  size_t entities_size;

  debug_renderer debug_renderer;
  entity entities[8];
  float view_matrix[16];
  float ortho_projection[16];
  SDL_Texture* textures[TEXTURE_COUNT];

  struct camera camera;
  struct input_state input;

  int play;
  int width;
  int height;
  float dt;
};

#endif
