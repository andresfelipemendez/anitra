#ifndef GAME_H
#define GAME_H

#include "debug_render.h"
#include <glad.h>

typedef void *(*ImGuiMemAllocFunc)(size_t sz, void *user_data);
typedef void (*ImGuiMemFreeFunc)(void *ptr, void *user_data);

typedef struct {
    int x, y, w, h;
} pixel_rect;

typedef struct {
    float x, y, w, h;  
} rect;

typedef struct {
    GLuint texture;    
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

typedef struct {
    int frames[10];
    float frame_time;
    int frame_count;
} animation_clip;

struct animator {
     animation_clip animation;
     float timer = 0.0f;
     int   frame_index = 0;
     bool playing = true;
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
    sprite_sheet sprite_sheet;
    animator current_animation;
    collider collider;
    sprite spr;
    vec2 pos;
    float health;
    entity_type type;
} entity;

struct game;

typedef void (*hotreloadable_imgui_draw_func)(struct game *g);
typedef void (*render_entities_func)(struct game *g);
typedef void (*render_sprite_func)(struct game *g, GLuint texture, float x, float y);
typedef GLuint (*load_texture_func)(const char* filepath);

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
    vec2 position = {0,160};
    float zoom = 1;
};

struct game {
  struct GLFWwindow *window;
  struct ImGuiContext *ctx;
  ImGuiMemAllocFunc alloc_func;
  ImGuiMemFreeFunc free_func;
  void *user_data;
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
  GLuint textures[TEXTURE_COUNT];
  
  camera camera;
  input_state input;
  
  int play;
  int width;
  int height;
  GLuint quad_VAO;
  GLuint sprite_shader;
  GLuint translation_loc;
  GLuint view_loc;
  GLuint projection_loc;
  GLuint texture_loc;
  GLuint tint_loc;
  GLuint sprite_offset_loc;
  GLuint sprite_size_loc;
  float dt;
};

#endif