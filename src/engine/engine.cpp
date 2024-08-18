#include "engine.h"
#include <externals.h>
#include <game.h>
#include <stdio.h>

#define IMGUI_USER_CONFIG "myimguiconfig.h"
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

EXPORT void hotreloadable_imgui_draw(game *g) {
  ImGui::SetCurrentContext(g->ctx);
  ImGui::SetAllocatorFunctions(g->alloc_func, g->free_func, g->user_data);
  ImGui::Begin("Hello! world from reloadable dll!");
  ImGui::Text("This is some useful text.");
  ImGui::Text("this is hot reloading!");
  ImGui::End();
}
