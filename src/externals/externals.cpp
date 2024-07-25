#include "externals.h"
#include "../game.h"
#include <stdio.h>


#include <GLFW/glfw3.h>

ImGuiUpdateFunc g_imguiUpdate = NULL;
ImGuiRenderFunc g_imguiRender = NULL;


    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

static void glfw_error_callback(int error, const char* description)
{
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

EXPORT int init_externals(game* g) {
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
        return -1;
    
    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+ only
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);            // 3.0+ only

    g->window = glfwCreateWindow(640, 480, "Hello World", NULL, NULL);
    if (!g->window)
    {
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(g->window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

    ImGui::StyleColorsDark();
    
    ImGui_ImplGlfw_InitForOpenGL(g->window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    g->play = true;
    g->ctx = ImGui::GetCurrentContext();
    return 1;
}

EXPORT void update_externals(game* g){
    
    glfwPollEvents();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    //g_imguiUpdate(g->ctx);
    ImGui::Begin("Debug Window");
    ImGui::Text("Hello from another window!");
    ImGui::End();
   //ImGui::SetCurrentContext(g->ctx);
    ImGui::Render();
    int display_w, display_h;
    glfwGetFramebufferSize(g->window, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w);
    glClear(GL_COLOR_BUFFER_BIT);
    
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(g->window);
    
    g->play = !glfwWindowShouldClose(g->window);
}

EXPORT void update_imgui_functions(ImGuiUpdateFunc updateFunc)
{
    printf("update_imgui_functions\n");

    g_imguiUpdate = updateFunc;

    printf("calling hot reload engine imgui frunctions from update_imgui_functions\n");
    // g_imguiUpdate();
    // g_imguiRender();
}

EXPORT void end_externals(game* g){
    
}

EXPORT void CallFromOutsideBegin(const char* text)
{
    ImGui::Begin(text);
    ImGui::Text("Hello from another window!");
    ImGui::End();
}

EXPORT ImGuiContext* GetImguiContext()
{
    return ImGui::GetCurrentContext();
}