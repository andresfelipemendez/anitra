#include "engine.h"
#include "../externals/externals.h"
#include <stdio.h>


EXPORT void HotReloadImGuiUpdate(ImGuiContext* ctx)
{
    ImGui::SetCurrentContext(ctx);
    ImGui::Begin("Debug Window");
    ImGui::Text("Hello from another window!");
    ImGui::End();
}

EXPORT void HotReloadGuiRender()
{
    printf("HotReloadImGuiUpdate\n");
}
