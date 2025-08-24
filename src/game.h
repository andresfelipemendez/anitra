#ifndef GAME_H
#define GAME_H

#include <glad.h>

typedef void *(*ImGuiMemAllocFunc)(size_t sz, void *user_data);
typedef void (*ImGuiMemFreeFunc)(void *ptr, void *user_data);

typedef struct {
    float x, y;
} vec2;

typedef struct entity {
    vec2 pos;
    GLuint texture;
} entity;

struct game;

typedef void (*hotreloadable_imgui_draw_func)(struct game *g);
typedef void (*render_entities_func)(struct game *g);
typedef void (*render_sprite_func)(struct game *g, GLuint texture, float x, float y);
typedef GLuint (*load_texture_func)(const char* filepath);

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
  float ortho_projection[16];
  entity* entities;
  size_t entities_size;

  render_entities_func render_entities;
  render_sprite_func render_sprite;
  load_texture_func load_texture;
};

int level[][6] = {
	{0,0,0,0,0,0},
	{0,1,1,1,0,0},
	{0,1,0,1,0,0},
};

#endif // !GAME_H