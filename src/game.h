#ifndef GAME_H
#define GAME_H

#include <glad.h>

typedef void *(*ImGuiMemAllocFunc)(size_t sz, void *user_data);
typedef void (*ImGuiMemFreeFunc)(void *ptr, void *user_data);

typedef struct {
    float x, y;
} vec2;

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

typedef struct {
    GLuint char_spritesheet;
    GLuint tiles_spritesheet;
} textures;

typedef struct {
    int frames[10];
    int frame_count;
    float frame_time;
} animation_clip;

struct animator {
    volatile animation_clip animation; 
    volatile float timer = 0.0f;
    volatile int   frame_index = 0;
    volatile bool playing = true;
};

typedef struct entity {
    vec2 pos;
    GLuint texture;
    sprite spr;
    animator current_animation;
} entity;

struct game;

typedef void (*hotreloadable_imgui_draw_func)(struct game *g);
typedef void (*render_entities_func)(struct game *g);
typedef void (*render_sprite_func)(struct game *g, GLuint texture, float x, float y);
typedef GLuint (*load_texture_func)(const char* filepath);

enum InputButton {
    INPUT_A = 1 << 0,  // Primary action
    INPUT_B = 1 << 1,  // Secondary action
    INPUT_X = 1 << 2,  // Use item
    INPUT_Y = 1 << 3,  // Menu/inventory
};

struct input_state {
    float horizontal;
    float vertical;
    int input_mask;
};

struct game {
  int play;
  int width;
  int height;
  struct GLFWwindow *window;
  struct ImGuiContext *ctx;
  ImGuiMemAllocFunc alloc_func;
  ImGuiMemFreeFunc free_func;
  void *user_data;
  void *engine_lib;
  GLuint quad_VAO;
  GLuint sprite_shader;
  GLuint translation_loc;
  GLuint projection_loc;
  GLuint texture_loc;
  GLuint tint_loc;
  GLuint sprite_offset_loc;
  GLuint sprite_size_loc;
  float ortho_projection[16];
  float dt;  
  double _t_prev;
  input_state input;
  entity entities[8];
  size_t entities_size;
  textures textures;
  render_entities_func render_entities;
  render_sprite_func render_sprite;
  load_texture_func load_texture;
};

#endif // !GAME_H