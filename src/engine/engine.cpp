#include "engine.h"
#include <externals.h>
#include <game.h>
#include <stdio.h>

#define IMGUI_USER_CONFIG "myimguiconfig.h"
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

EXPORT void hotreloadable_imgui_draw(game *g) {
    if (!g || !g->ctx) return;

    // ImGui setup
    ImGui::SetCurrentContext(g->ctx);
    ImGui::SetAllocatorFunctions(g->alloc_func, g->free_func, g->user_data);

    // Debug window with texture display
    ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400.0f, 500.0f), ImGuiCond_FirstUseEver);
    
    if (ImGui::Begin("Debug Info & Texture Viewer")) {
        ImGui::Text("Engine DLL - ImGui Only");
        ImGui::Text("OpenGL rendering in externals.dll");
        
        // Status indicators
        ImGui::Separator();
        ImGui::Text("Recent Fixes Applied:");
        ImGui::Text("- Coordinate system: (0,0) = center");
        ImGui::Text("- Texture orientation: right-side up");
        ImGui::Text("- Sprite size: 64x64 pixels");
        ImGui::Separator();
        
        ImGui::Text("Function Pointers:");
        ImGui::Text("  render_entities: %s", g->render_entities ? "OK" : "NULL");
        ImGui::Text("  render_sprite: %s", g->render_sprite ? "OK" : "NULL");
        ImGui::Text("  load_texture: %s", g->load_texture ? "OK" : "NULL");
        ImGui::Separator();
        
        ImGui::Text("OpenGL Info:");
        ImGui::Text("  Shader Program: %u", g->sprite_shader);
        ImGui::Text("  Projection Loc: %d", g->projection_loc);
        ImGui::Text("  Translation Loc: %d", g->translation_loc);  
        ImGui::Text("  Texture Loc: %d", g->texture_loc);
        ImGui::Text("  Tint Loc: %d", g->tint_loc);
        ImGui::Text("  Quad VAO: %u", g->quad_VAO);
        ImGui::Text("  Window Size: %dx%d", g->width, g->height);
        ImGui::Separator();
        
        ImGui::Text("Entities: %zu", g->entities_size);
        
        if (g->entities_size > 0 && g->entities) {
            for (size_t i = 0; i < g->entities_size; i++) {
                ImGui::PushID((int)i);
                char label[64];
                sprintf(label, "Entity %zu", i);
                
                if (ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::Text("Texture ID: %u", g->entities[i].texture);
                    ImGui::Text("Status: %s", g->entities[i].texture != 0 ? "Loaded" : "Not loaded");
                    
                    // Display texture in ImGui if loaded
                    if (g->entities[i].texture != 0) {
                        ImGui::Text("Texture Preview:");
                        // Cast GLuint to ImTextureID for ImGui
                        ImTextureID texture_id = (ImTextureID)(uintptr_t)g->entities[i].texture;
                        
                        // Display texture at different sizes for debugging
                        ImGui::Image(texture_id, ImVec2(64, 64));
                        ImGui::SameLine();
                        ImGui::Image(texture_id, ImVec2(32, 32));
                        ImGui::SameLine();
                        ImGui::Image(texture_id, ImVec2(16, 16));
                        
                        // Larger preview
                        ImGui::Text("Large Preview:");
                        ImGui::Image(texture_id, ImVec2(128, 128));
                        
                        if (ImGui::Button("Test Direct Render") && g->render_sprite) {
                            printf("Test render button clicked!\n");
                        }
                    } else {
                        ImGui::Text("[X] Texture failed to load");
                        ImGui::Text("Check console for error messages");
                    }
                    
                    ImGui::Separator();
                    
                    // Position controls
                    ImGui::Text("World Position Controls:");
                    ImGui::Text("(0,0) = Screen Center");
                    ImGui::Text("+X = Right, +Y = Up");
                    
                    // More intuitive slider ranges centered around 0
                    float half_width = g->width ? g->width / 2.0f : 400.0f;
                    float half_height = g->height ? g->height / 2.0f : 300.0f;
                    
                    ImGui::SliderFloat("X Position", &g->entities[i].pos.x, -half_width, half_width, "%.0f");
                    ImGui::SliderFloat("Y Position", &g->entities[i].pos.y, -half_height, half_height, "%.0f");
                    
                    ImGui::Text("Current Position: (%.0f, %.0f)", g->entities[i].pos.x, g->entities[i].pos.y);
                    
                    // Movement buttons with better increments
                    float move_step = 25.0f;
                    if (ImGui::Button("Up")) g->entities[i].pos.y += move_step;
                    ImGui::SameLine();
                    if (ImGui::Button("Down")) g->entities[i].pos.y -= move_step;
                    ImGui::SameLine();
                    if (ImGui::Button("Left")) g->entities[i].pos.x -= move_step;
                    ImGui::SameLine();
                    if (ImGui::Button("Right")) g->entities[i].pos.x += move_step;
                    
                    if (ImGui::Button("Center (0,0)")) {
                        g->entities[i].pos.x = 0.0f;
                        g->entities[i].pos.y = 0.0f;
                    }
                    
                    // Better test positions based on actual screen size
                    ImGui::Text("Quick Test Positions:");
                    if (ImGui::Button("Top-Left")) {
                        g->entities[i].pos.x = -half_width * 0.7f;
                        g->entities[i].pos.y = half_height * 0.7f;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Top-Right")) {
                        g->entities[i].pos.x = half_width * 0.7f;
                        g->entities[i].pos.y = half_height * 0.7f;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Bottom-Left")) {
                        g->entities[i].pos.x = -half_width * 0.7f;
                        g->entities[i].pos.y = -half_height * 0.7f;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Bottom-Right")) {
                        g->entities[i].pos.x = half_width * 0.7f;
                        g->entities[i].pos.y = -half_height * 0.7f;
                    }
                }
                
                ImGui::PopID();
            }
        }
    }
    ImGui::End();

    // Assets window  
    ImGui::SetNextWindowPos(ImVec2(420.0f, 10.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Assets & File Browser")) {
        try {
            std::string currentPath = fs::current_path().string();

            ImGui::Text("Current Directory:");
            ImGui::Separator();
            ImGui::TextWrapped("%s", currentPath.c_str());

            ImGui::Separator();

            // Show image files with more detail
            const char* imageExtensions[] = {".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif"};
            
            ImGui::Text("All Files:");
            int fileCount = 0;
            for (const auto &entry : fs::directory_iterator(fs::current_path())) {
                if (entry.is_regular_file()) {
                    fileCount++;
                    std::string extension = entry.path().extension().string();
                    std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
                    
                    bool isImage = false;
                    for (const char* ext : imageExtensions) {
                        if (extension == ext) {
                            isImage = true;
                            break;
                        }
                    }
                    
                    if (isImage) {
                        ImGui::Text("[IMG] %s", entry.path().filename().string().c_str());
                        
                        // Show file size
                        try {
                            auto size = fs::file_size(entry);
                            ImGui::SameLine();
                            ImGui::Text("(%zu bytes)", size);
                        } catch (...) {
                            ImGui::SameLine();
                            ImGui::Text("(size unknown)");
                        }
                        
                        // Test load button
                        ImGui::SameLine();
                        std::string buttonLabel = "Load##" + entry.path().filename().string();
                        if (ImGui::SmallButton(buttonLabel.c_str()) && g->load_texture) {
                            GLuint test_texture = g->load_texture(entry.path().string().c_str());
                            if (test_texture != 0) {
                                printf("Successfully loaded test texture: %s (ID: %u)\n", 
                                       entry.path().filename().string().c_str(), test_texture);
                            }
                        }
                    } else {
                        ImGui::Text("[FILE] %s", entry.path().filename().string().c_str());
                    }
                } else if (entry.is_directory()) {
                    std::string name = entry.path().filename().string();
                    ImGui::Text("[DIR] %s/", name.c_str());
                }
            }
            
            ImGui::Separator();
            ImGui::Text("Total files found: %d", fileCount);
            
        } catch (const std::exception& e) {
            ImGui::Text("[X] Error reading directory: %s", e.what());
        }
    }
    ImGui::End();

    // Rendering Info Window
    ImGui::SetNextWindowPos(ImVec2(10.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Rendering Debug")) {
        ImGui::Text("Game Loop Info");
        ImGui::Separator();
        
        ImGui::Text("This window shows the game is running");
        ImGui::Text("Sprites render BEHIND these ImGui windows");
        ImGui::Text("Use the position controls to move sprites around");
        
        ImGui::Separator();
        
        if (g->entities_size > 0 && g->entities && g->entities[0].texture != 0) {
            ImGui::Text("[OK] Texture loaded successfully");
            ImGui::Text("[OK] Should be visible at screen CENTER");
            ImGui::Text("[OK] Should be right-side up now!");
            ImGui::Text("Move with sliders or direction buttons!");
        } else {
            ImGui::Text("[X] No texture loaded");
            ImGui::Text("Check the assets/ folder for knights.png");
        }
        
        static bool show_tips = true;
        ImGui::Checkbox("Show Tips", &show_tips);
        
        if (show_tips) {
            ImGui::Separator();
            ImGui::Text("Tips:");
            ImGui::Text("- The sprite renders in world coordinates");
            ImGui::Text("- (0,0) is NOW the center of the screen");
            ImGui::Text("- Positive X = right, Positive Y = up");
            ImGui::Text("- Texture should now be right-side up!");
            ImGui::Text("- Try the corner position buttons");
            ImGui::Text("- Check console output for debug info");
        }
        
        ImGui::Separator();
        ImGui::Text("FPS: %.1f (%.3f ms/frame)", 
                    ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);
    }
    ImGui::End();
}