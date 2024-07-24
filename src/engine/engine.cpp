#include "engine.h"
#include <imgui.h>

EXPORT void HotReloadImGuiUpdate()
{
    ImGui::Begin("Debug Window");
    ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
    ImGui::End();
}

EXPORT void HotReloadGuiRender()
{
}
