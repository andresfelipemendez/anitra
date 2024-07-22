#include "engine/engine.h"
#include <imgui.h>

typedef void (*ImguiUpdateFunc)();
typedef void (*ImGuirenderFunc)();

void HotReloadImGuiUpdate()
{

}

void HotReloadGuiRender()
{
}

void GetImguiFunctions(ImguiUpdateFunc* outUpdate, ImGuirenderFunc* outRender)
{
    *outUpdate = HotReloadImGuiUpdate;
    *outRender = HotReloadGuiRender;
}

