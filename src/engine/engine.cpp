#include "engine.h"
#include <imgui.h>
#include <stdio.h>

EXPORT void HotReloadImGuiUpdate()
{
    printf("HotReloadImGuiUpdate\n");
    ImGui::Begin("Debug Window");
    ImGui::Text("Hello from another window!");
    ImGui::End();
}

EXPORT void HotReloadGuiRender()
{
    printf("HotReloadImGuiUpdate\n");
}
