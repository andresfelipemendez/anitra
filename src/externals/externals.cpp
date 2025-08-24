#include <externals.h>
#include <game.h>
#include <stdio.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <glad.h>
#include <GLFW/glfw3.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

hotreloadable_imgui_draw_func g_imguiUpdate = NULL;

ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

static void glfw_error_callback(int error, const char *description) {
  fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

GLuint compile_shader(const char* source, GLenum shader_type) {
    GLuint shader = glCreateShader(shader_type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char info_log[512];
        glGetShaderInfoLog(shader, 512, NULL, info_log);
        const char* type_str = (shader_type == GL_VERTEX_SHADER) ? "VERTEX" : "FRAGMENT";
        fprintf(stderr, "%s shader compilation failed:\n%s\n", type_str, info_log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint create_shader_program(const char* vertex_source, const char* fragment_source) {
    GLuint vertex_shader = compile_shader(vertex_source, GL_VERTEX_SHADER);
    GLuint fragment_shader = compile_shader(fragment_source, GL_FRAGMENT_SHADER);

    if (!vertex_shader || !fragment_shader) {
        if (vertex_shader) glDeleteShader(vertex_shader);
        if (fragment_shader) glDeleteShader(fragment_shader);
        return 0;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);
    glLinkProgram(program);

    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char info_log[512];
        glGetProgramInfoLog(program, 512, NULL, info_log);
        fprintf(stderr, "Shader program linking failed:\n%s\n", info_log);
        glDeleteProgram(program);
        program = 0;
    }

    // Clean up shaders
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);
    
    return program;  // Return actual program, not 0
}

GLuint externals_load_texture(const char* filepath) {
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);

    // Set texture wrapping parameters
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    
    // Set texture filtering parameters
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // Load and generate the texture
    int width, height, nrChannels;
    stbi_set_flip_vertically_on_load(false);
    
    unsigned char *data = stbi_load(filepath, &width, &height, &nrChannels, 0);
    if (data) {
        GLenum format;
        if (nrChannels == 1)
            format = GL_RED;
        else if (nrChannels == 3)
            format = GL_RGB;
        else if (nrChannels == 4)
            format = GL_RGBA;
        else {
            fprintf(stderr, "Unsupported number of channels: %d\n", nrChannels);
            stbi_image_free(data);
            glDeleteTextures(1, &texture);
            return 0;
        }

        glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, format, GL_UNSIGNED_BYTE, data);
        glGenerateMipmap(GL_TEXTURE_2D);
        
        printf("Loaded texture: %s (%dx%d, %d channels)\n", filepath, width, height, nrChannels);
    } else {
        fprintf(stderr, "Failed to load texture: %s\n", filepath);
        glDeleteTextures(1, &texture);
        texture = 0;
    }
    
    stbi_image_free(data);
    return texture;
}

void check_gl_error(const char* operation) {
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        fprintf(stderr, "OpenGL error after %s: 0x%x\n", operation, error);
    }
}

void externals_render_sprite(game* g, GLuint texture, float x, float y) {
    if (!g || g->sprite_shader == 0 || texture == 0) {
        printf("Render sprite failed: g=%p, shader=%u, texture=%u\n", g, g ? g->sprite_shader : 0, texture);
        return;
    }
    
    glUseProgram(g->sprite_shader);
    check_gl_error("glUseProgram");
    
    // Set translation uniform
    if (g->translation_loc != -1) {
        glUniform2f(g->translation_loc, x, y);
        check_gl_error("glUniform2f translation");
    }
    
    // Bind texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    check_gl_error("glBindTexture");
    
    if (g->texture_loc != -1) {
        glUniform1i(g->texture_loc, 0);
        check_gl_error("glUniform1i texture");
    }
    
    // Set tint color
    if (g->tint_loc != -1) {
        glUniform4f(g->tint_loc, 1.0f, 1.0f, 1.0f, 1.0f);
        check_gl_error("glUniform4f tint");
    }
    
    // Draw quad
    if (g->quad_VAO != 0) {
        glBindVertexArray(g->quad_VAO);
        check_gl_error("glBindVertexArray");
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
        check_gl_error("glDrawElements");
    }
}

void externals_render_entities(game* g) {
    if (!g) return;
        
    // Set up shader and projection for rendering
    if (g->sprite_shader != 0) {
        glUseProgram(g->sprite_shader);
        check_gl_error("glUseProgram entities");
        
        if (g->projection_loc != -1) {
            glUniformMatrix4fv(g->projection_loc, 1, GL_FALSE, g->ortho_projection);
            check_gl_error("glUniformMatrix4fv projection");
            
        }
        
        // Render all entities
        if (g->entities && g->entities_size > 0) {
            for (size_t i = 0; i < g->entities_size; i++) {
                if (g->entities[i].texture != 0) {
                    
                    externals_render_sprite(g, g->entities[i].texture, 
                                          g->entities[i].pos.x, g->entities[i].pos.y);
                }
            }
        }
    } else {
        printf("No sprite shader available\n");
    }
}

int load_shader(game* g) {
    const char* vertex_source = 
        "#version 330 core\n"
        "layout (location = 0) in vec2 aPos;\n"      // 2D position
        "layout (location = 1) in vec2 aTexCoord;\n" // texture coordinates
        "out vec2 TexCoord;\n"
        "uniform vec2 translation;\n"                 // entity position
        "uniform mat4 projection;\n"                  // orthographic projection
        "void main() {\n"
        "    vec2 worldPos = aPos + translation;\n"   // translate to entity position
        "    gl_Position = projection * vec4(worldPos, 0.0, 1.0);\n"
        "    TexCoord = aTexCoord;\n"
        "}\n";
    
    // Fragment shader with alpha support
    const char* fragment_source = 
        "#version 330 core\n"
        "in vec2 TexCoord;\n"
        "out vec4 FragColor;\n"
        "uniform sampler2D ourTexture;\n"
        "uniform vec4 tintColor;\n"                   // optional color tinting
        "void main() {\n"
        "    vec4 texColor = texture(ourTexture, TexCoord);\n"
        "    if(texColor.a < 0.1)\n"                  // discard transparent pixels
        "        discard;\n"
        "    FragColor = texColor * tintColor;\n"     // apply tint and keep alpha
        "}\n";
    
    GLuint shader_program = create_shader_program(vertex_source, fragment_source);
    if (!shader_program) {
        fprintf(stderr, "Failed to create shader program\n");
        return -1;
    }

    printf("Shader program created successfully: ID = %u\n", shader_program);
    g->sprite_shader = shader_program;

    // FIXED: Store tint_loc properly in game struct
    g->translation_loc = glGetUniformLocation(shader_program, "translation");
    g->projection_loc = glGetUniformLocation(shader_program, "projection");
    g->texture_loc = glGetUniformLocation(shader_program, "ourTexture");
    g->tint_loc = glGetUniformLocation(shader_program, "tintColor");  // FIXED: Was not being stored

    printf("Uniform locations:\n");
    printf("  translation: %d\n", g->translation_loc);
    printf("  projection: %d\n", g->projection_loc);
    printf("  ourTexture: %d\n", g->texture_loc);
    printf("  tintColor: %d\n", g->tint_loc);

    // FIXED: Make sprite bigger so it's visible (was 10x10, now 32x32)
    float vertices[] = {
        -32.0f,  32.0f,  0.0f, 0.0f,  // top left
         32.0f,  32.0f,  1.0f, 0.0f,  // top right
         32.0f, -32.0f,  1.0f, 1.0f,  // bottom right
        -32.0f, -32.0f,  0.0f, 1.0f   // bottom left
    };

    unsigned int indices[] = {
        0, 1, 2,  
        2, 3, 0   
    };

    GLuint VAO, VBO, EBO;
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
    glGenBuffers(1, &EBO);

    glBindVertexArray(VAO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    // Position attribute
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    // Texture coordinate attribute
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    int display_w, display_h;
    glfwGetFramebufferSize(g->window, &display_w, &display_h);
  
    float screen_width = (float)display_w;
    float screen_height = (float)display_h;
    
    printf("Screen dimensions: %dx%d\n", display_w, display_h);
    
    // FIXED: Correct orthographic projection matrix
    float temp_ortho[16] = {
        2.0f / screen_width,  0.0f,                  0.0f,   0.0f,
        0.0f,                 2.0f / screen_height,  0.0f,   0.0f,
        0.0f,                 0.0f,                 -1.0f,   0.0f,
        0.0f,                 0.0f,                  0.0f,   1.0f
    };
    
    // Copy to the game struct's array
    for (int i = 0; i < 16; i++) {
        g->ortho_projection[i] = temp_ortho[i];
    }
    
    g->quad_VAO = VAO;
    printf("Quad VAO created: %u\n", VAO);
    
    // Enable alpha blending
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    printf("Shader setup completed successfully\n");
    return 1;  // FIXED: Return 1 not true
}

GLuint load_texture(const char* filepath) {
    return externals_load_texture(filepath);
}

EXPORT int init_externals(game *g) {
  glfwSetErrorCallback(glfw_error_callback);

  if (!glfwInit()) {
    fprintf(stderr, "Failed to initialize GLFW\n");
    return -1;
  }

  const char *glsl_version = "#version 150";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

  // FIXED: Make window bigger for better visibility
  g->window = glfwCreateWindow(800, 600, "Sprite Rendering Test", NULL, NULL);
  if (!g->window) {
    fprintf(stderr, "Failed to create GLFW window\n");
    glfwTerminate();
    return -1;
  }

  glfwMakeContextCurrent(g->window);
  glfwSwapInterval(1);

  if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
    fprintf(stderr, "Failed to initialize GLAD\n");
    return -1;
  }

  printf("OpenGL Version: %s\n", glGetString(GL_VERSION));

  IMGUI_CHECKVERSION();
  ImGuiContext *ctx = ImGui::CreateContext();
  if (!ctx) {
    fprintf(stderr, "Failed to create ImGui context\n");
    glfwDestroyWindow(g->window);
    glfwTerminate();
    return -1;
  }

  ImGuiIO &io = ImGui::GetIO();
  (void)io;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

  ImGui::StyleColorsDark();
  ImGui::SetCurrentContext(ctx);

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

  load_shader(g);

  g->entities_size = 1;
  g->entities = new entity[g->entities_size];

  // Try both path formats
  g->entities[0].texture = load_texture("assets/knights.png");
  if (g->entities[0].texture == 0) {
      g->entities[0].texture = load_texture("assets\\knights.png");
  }
  
  g->entities[0].pos.x = 0.0f;  // Start at center
  g->entities[0].pos.y = 0.0f;

  // Set up function pointers
  g->render_entities = externals_render_entities;
  g->render_sprite = externals_render_sprite;
  g->load_texture = externals_load_texture;

  g->play = true;
  g->ctx = ctx;
  ImGui::GetAllocatorFunctions(&g->alloc_func, &g->free_func, &g->user_data);

  printf("Initialization complete - texture ID: %u\n", g->entities[0].texture);
  return 1;
}

EXPORT void update_externals(game *g) {
  int display_w, display_h;
  glfwGetFramebufferSize(g->window, &display_w, &display_h);
  glViewport(0, 0, display_w, display_h);
  g->width = display_w;
  g->height = display_h;
  
  glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w,
               clear_color.z * clear_color.w, clear_color.w);
  glClear(GL_COLOR_BUFFER_BIT);

  // Render sprites before ImGui
  if (g->render_entities) {
      g->render_entities(g);
  }
        
  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();

  g_imguiUpdate(g);

  ImGui::SetCurrentContext(g->ctx);
  ImGui::SetAllocatorFunctions(g->alloc_func, g->free_func, g->user_data);

  ImGui::Render();
  
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

  glfwSwapBuffers(g->window);
  glfwPollEvents();
  g->play = !glfwWindowShouldClose(g->window);
}

EXPORT void end_externals(game *g) {
    if (g->entities) {
        // Clean up textures
        for (size_t i = 0; i < g->entities_size; i++) {
            if (g->entities[i].texture != 0) {
                glDeleteTextures(1, &g->entities[i].texture);
            }
        }
        delete[] g->entities;
    }
}

EXPORT void assign_hotreloadable(hotreloadable_imgui_draw_func func) {
  g_imguiUpdate = func;
}

EXPORT ImGuiContext *GetImguiContext() { return ImGui::GetCurrentContext(); }