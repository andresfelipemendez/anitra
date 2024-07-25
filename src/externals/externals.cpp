#include "externals.h"
#include "../game.h"
#include <stdio.h>



ImGuiUpdateFunc g_imguiUpdate = NULL;
ImGuiRenderFunc g_imguiRender = NULL;

static void glfw_error_callback(int error, const char* description)
{
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

EXPORT int init_externals(game* g) {
    
    return 1;
}

EXPORT void update_externals(game* g){
    
   

}

EXPORT void update_imgui_functions(ImGuiUpdateFunc updateFunc, ImGuiRenderFunc renderFunc)
{
    printf("update_imgui_functions\n");

    g_imguiUpdate = updateFunc;
    g_imguiRender = renderFunc;

    
    printf("calling hot reload engine imgui frunctions from update_imgui_functions\n");
    // g_imguiUpdate();
    // g_imguiRender();
}

EXPORT void end_externals(game* g){
}
