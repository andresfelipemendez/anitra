#include <game.h>
#include <externals.h>
#include <stdio.h>


#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>



ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

static void glfw_error_callback(int error, const char* description)
{
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}
EXPORT int init_externals(game* g) {
    glfwSetErrorCallback(glfw_error_callback);

    if (!glfwInit()) {
        fprintf(stderr, "Failed to initialize GLFW\n");
        return -1;
    }

    const char* glsl_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+ only
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);            // 3.0+ only

    g->window = glfwCreateWindow(640, 480, "Hello World", NULL, NULL);
    if (!g->window) {
        fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(g->window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGuiContext* ctx = ImGui::CreateContext();
    if (!ctx) {
        fprintf(stderr, "Failed to create ImGui context\n");
        glfwDestroyWindow(g->window);
        glfwTerminate();
        return -1;
    }

    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

    ImGui::StyleColorsDark();
    ImGui::SetCurrentContext(ctx);

    //ImGuiViewport* main_viewport = ImGui::GetMainViewport();
    //if (main_viewport == nullptr || main_viewport->PlatformHandleRaw == nullptr) {
    //    fprintf(stderr, "Main viewport or platform handle is null\n");
    //    ImGui::DestroyContext(ctx);
    //    glfwDestroyWindow(g->window);
    //    glfwTerminate();
    //    return -1;
    //}

    if (!ImGui_ImplGlfw_InitForOpenGL(g->window, true)) {
        fprintf(stderr, "Failed to initialize ImGui_ImplGlfw\n");
        ImGui::DestroyContext(ctx);
        glfwDestroyWindow(g->window);
        glfwTerminate();
        return -1;
    }

    if (!ImGui_ImplOpenGL3_Init(glsl_version)) {
        fprintf(stderr, "Failed to initialize ImGui_ImplOpenGL3\n");
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext(ctx);
        glfwDestroyWindow(g->window);
        glfwTerminate();
        return -1;
    }

    g->play = true;
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