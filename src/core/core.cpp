#include <stdio.h>
#include "core.h"
#include "../game.h"
#include "../externals/externals.h"
#include "../engine/engine.h"
#include "loadlibrary.h"

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>

void* load_lib(const char* libname){
    void* lib = loadlibrary(libname);
    if (lib == NULL) {
        fprintf(stderr, "Library %s loading failed\n", libname);
    }
    printf("Library %s loaded successfully\n", libname);
    return lib;
}

static void glfw_error_callback(int error, const char* description)
{
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

EXPORT void init() {
    printf("Core initialized\n");
    void* externals_lib = load_lib("externals");
    init_externals_func init = (init_externals_func)getfunction(externals_lib, "init_externals");
    update_externals_func update = (update_externals_func)getfunction(externals_lib, "update_externals");
    update_imgui_functions_func update_imgui_functions = (update_imgui_functions_func)getfunction(externals_lib, "update_imgui_functions");

    void* engine_lib = load_lib("engine");
    HotReloadImGuiUpdate_func HotReloadImGuiUpdate = (HotReloadImGuiUpdate_func)getfunction(engine_lib, "HotReloadImGuiUpdate");
    HotReloadGuiRender_func HotReloadGuiRender = (HotReloadGuiRender_func)getfunction(engine_lib, "HotReloadGuiRender");

    update_imgui_functions(HotReloadImGuiUpdate,HotReloadImGuiUpdate);

    game g;
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()){
        return;
    }
    
    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+ only
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);            // 3.0+ only

    g.window = glfwCreateWindow(640, 480, "Hello World", NULL, NULL);
    if (!g.window)
    {
        glfwTerminate();
        return;
    }

    glfwMakeContextCurrent(g.window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(g.window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    g.play =  true;

    // get signal to reload
    bool hotReload = false;
    if(hotReload) {
        
    }

    while(g.play) {
         ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        HotReloadImGuiUpdate();

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(g.window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);
        
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(g.window);
        
        g.play = !glfwWindowShouldClose(g.window);
    }
}
