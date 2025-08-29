#include <externals.h>
#include <game.h>
#include <debug_render.h>
#include <stdio.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <glad.h>
#include <GLFW/glfw3.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

init g_init = NULL;
destroy g_destroy = NULL;
update g_update = NULL;

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

int load_shader(game* g) {
    static const char* debug_vertex_shader = 
      "#version 330 core\n"
      "layout(location = 0) in vec2 aPos;\n"
      "layout(location = 1) in vec3 aColor;\n"
      "uniform mat4 projection;\n"
      "uniform mat4 view;\n"
      "uniform vec2 translation;\n"
      "out vec3 vertexColor;\n"
      "void main() {\n"
      "    gl_Position = projection * view * vec4(aPos + translation, 0.0, 1.0);\n"
      "    vertexColor = aColor;\n"
      "}\n";

    static const char* debug_fragment_shader = 
      "#version 330 core\n"
      "in vec3 vertexColor;\n"
      "out vec4 FragColor;\n"
      "void main() {\n"
      "    FragColor = vec4(vertexColor, 1.0);\n"
      "}\n";

    debug_renderer* dr = &g->debug_renderer;
    dr->debug_shader = create_shader_program(debug_vertex_shader, debug_fragment_shader);
    dr->debug_projection_loc = glGetUniformLocation(dr->debug_shader, "projection");
    dr->debug_view_loc = glGetUniformLocation(dr->debug_shader, "view");
    dr->debug_translation_loc = glGetUniformLocation(dr->debug_shader, "translation");
    
    dr->max_lines = 1000;
    dr->vertex_buffer = (float*)malloc(dr->max_lines * 10 *sizeof(float));
    
    glGenVertexArrays(1, &dr->line_VAO);
    glGenBuffers(1, &dr->line_VBO);
    
    glBindVertexArray(dr->line_VAO);
    glBindBuffer(GL_ARRAY_BUFFER, dr->line_VBO);
    glBufferData(GL_ARRAY_BUFFER, dr->max_lines * 4 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
    
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    const char* vertex_source = 
        "#version 330 core\n"
        "layout (location = 0) in vec2 aPos;\n"      // 2D position
        "layout (location = 1) in vec2 aTexCoord;\n" // texture coordinates
        "out vec2 TexCoord;\n"
        "uniform vec2 translation;\n"                 // entity position
        "uniform mat4 view; \n"
        "uniform mat4 projection;\n"                  // orthographic projection
        "uniform vec2 sprite_offset;\n"
        "uniform vec2 sprite_size;\n"
        "void main() {\n"
        "    vec2 worldPos = aPos + translation;\n" 
        "    gl_Position = projection * view* vec4(worldPos, 0.0, 1.0);\n"
        "    TexCoord = sprite_offset + (aTexCoord * sprite_size);\n"
        "}\n";
    
    const char* fragment_source = 
        "#version 330 core\n"
        "in vec2 TexCoord;\n"
        "out vec4 FragColor;\n"
        "uniform sampler2D ourTexture;\n"
        "uniform vec4 tintColor;\n"                   
        "void main() {\n"
        "    vec4 texColor = texture(ourTexture, TexCoord);\n"
        "    FragColor = texColor * tintColor;\n"    
        "}\n";
    
    GLuint shader_program = create_shader_program(vertex_source, fragment_source);
    if (!shader_program) {
        fprintf(stderr, "Failed to create shader program\n");
        return -1;
    }

    printf("Shader program created successfully: ID = %u\n", shader_program);
    g->sprite_shader = shader_program;
    
    g->view_loc = glGetUniformLocation(shader_program, "view");
    g->translation_loc = glGetUniformLocation(shader_program, "translation");
    g->projection_loc = glGetUniformLocation(shader_program, "projection");
    g->texture_loc = glGetUniformLocation(shader_program, "ourTexture");
    g->tint_loc = glGetUniformLocation(shader_program, "tintColor");  

    g->sprite_offset_loc = glGetUniformLocation(g->sprite_shader, "sprite_offset");
    g->sprite_size_loc = glGetUniformLocation(g->sprite_shader, "sprite_size");

    printf("Uniform locations:\n");
    printf("  translation: %d\n", g->translation_loc);
    printf("  view: %d\n", g->view_loc);
    printf("  projection: %d\n", g->projection_loc);
    printf("  ourTexture: %d\n", g->texture_loc);
    printf("  tintColor: %d\n", g->tint_loc);

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

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    int display_w, display_h;
    glfwGetFramebufferSize(g->window, &display_w, &display_h);
  
    float screen_width = (float)display_w;
    float screen_height = (float)display_h;
        
    float temp_ortho[16] = {
        2.0f / screen_width,  0.0f,                  0.0f,   0.0f,
        0.0f,                 2.0f / screen_height,  0.0f,   0.0f,
        0.0f,                 0.0f,                 -1.0f,   0.0f,
        0.0f,                 0.0f,                  0.0f,   1.0f
    };
    
    for (int i = 0; i < 16; i++) {
        g->ortho_projection[i] = temp_ortho[i];
    }
    
    g->quad_VAO = VAO;
    printf("Quad VAO created: %u\n", VAO);
    
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    printf("Shader setup completed successfully\n");
    return 1; 
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

  g->textures[TEXTURE_PLAYER] = load_texture("assets\\char_spritesheet.png");
  g->textures[TEXTURE_TILES] = load_texture("assets\\Dungeon_Tileset.png");
  g->textures[TEXTURE_SLIME] = load_texture("assets\\pinkslime_spritesheet.png");
  g->textures[TEXTURE_HEALTH_BAR] = load_texture("assets\\health_bar_hud.png");
  g->textures[TEXTURE_HEALTH_FILL] = load_texture("assets\\health_hud.png");
  
  g->_t_prev = glfwGetTime();
  g->dt = 0.0f;
  g->play = true;
  g->ctx = ctx;
  ImGui::GetAllocatorFunctions(&g->alloc_func, &g->free_func, &g->user_data);
  return 1;
}

EXPORT void init_engine(game *g){
  g_init(g);
}

EXPORT void destroy_engine(game *g){
  g_destroy(g);
}

void update_input(game *g) {
  g->input.horizontal = 0.0f;
  g->input.vertical = 0.0f;
  g->input.input_mask = 0;
  
  // Keyboard input
  float kb_horizontal = 0.0f;
  float kb_vertical = 0.0f;
  
  if (glfwGetKey(g->window, GLFW_KEY_A) == GLFW_PRESS || 
      glfwGetKey(g->window, GLFW_KEY_LEFT) == GLFW_PRESS) {
    kb_horizontal -= 1.0f;
  }
  if (glfwGetKey(g->window, GLFW_KEY_D) == GLFW_PRESS || 
      glfwGetKey(g->window, GLFW_KEY_RIGHT) == GLFW_PRESS) {
    kb_horizontal += 1.0f;
  }
  if (glfwGetKey(g->window, GLFW_KEY_W) == GLFW_PRESS || 
      glfwGetKey(g->window, GLFW_KEY_UP) == GLFW_PRESS) {
    kb_vertical += 1.0f;
  }
  if (glfwGetKey(g->window, GLFW_KEY_S) == GLFW_PRESS || 
      glfwGetKey(g->window, GLFW_KEY_DOWN) == GLFW_PRESS) {
    kb_vertical -= 1.0f;
  }
  
  float kb_magnitude = sqrt(kb_horizontal * kb_horizontal + kb_vertical * kb_vertical);
  if (kb_magnitude > 1.0f) {
    kb_horizontal /= kb_magnitude;
    kb_vertical /= kb_magnitude;
  }
  
  float gp_horizontal = 0.0f;
  float gp_vertical = 0.0f;
  
  if (glfwJoystickPresent(GLFW_JOYSTICK_1) && glfwJoystickIsGamepad(GLFW_JOYSTICK_1)) {
    GLFWgamepadstate state;
    if (glfwGetGamepadState(GLFW_JOYSTICK_1, &state)) {
      gp_horizontal = state.axes[GLFW_GAMEPAD_AXIS_LEFT_X];
      gp_vertical = -state.axes[GLFW_GAMEPAD_AXIS_LEFT_Y];
      
      const float deadzone = 0.2f;
      float magnitude = sqrt(gp_horizontal * gp_horizontal + gp_vertical * gp_vertical);
      
      if (magnitude < deadzone) {
        gp_horizontal = 0.0f;
        gp_vertical = 0.0f;
      } else {
        float normalized_magnitude = (magnitude - deadzone) / (1.0f - deadzone);
        if (normalized_magnitude > 1.0f) normalized_magnitude = 1.0f;
        
        gp_horizontal = (gp_horizontal / magnitude) * normalized_magnitude;
        gp_vertical = (gp_vertical / magnitude) * normalized_magnitude;
      }
      
      if (state.buttons[GLFW_GAMEPAD_BUTTON_A] == GLFW_PRESS) {
        g->input.input_mask |= INPUT_A;
      }
      if (state.buttons[GLFW_GAMEPAD_BUTTON_B] == GLFW_PRESS) {
        g->input.input_mask |= INPUT_B;
      }
      if (state.buttons[GLFW_GAMEPAD_BUTTON_X] == GLFW_PRESS) {
        g->input.input_mask |= INPUT_X;
      }
      if (state.buttons[GLFW_GAMEPAD_BUTTON_Y] == GLFW_PRESS) {
        g->input.input_mask |= INPUT_Y;
      }
    }
  }
  
  // Keyboard buttons (action-adventure mapping)
  if (glfwGetKey(g->window, GLFW_KEY_SPACE) == GLFW_PRESS || 
      glfwGetKey(g->window, GLFW_KEY_J) == GLFW_PRESS) {
    g->input.input_mask |= INPUT_A;  // Primary action
  }
  if (glfwGetKey(g->window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS || 
      glfwGetKey(g->window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS ||
      glfwGetKey(g->window, GLFW_KEY_K) == GLFW_PRESS) {
    g->input.input_mask |= INPUT_B;  // Secondary action
  }
  if (glfwGetKey(g->window, GLFW_KEY_E) == GLFW_PRESS || 
      glfwGetKey(g->window, GLFW_KEY_L) == GLFW_PRESS) {
    g->input.input_mask |= INPUT_X;  // Use item
  }
  if (glfwGetKey(g->window, GLFW_KEY_Q) == GLFW_PRESS || 
      glfwGetKey(g->window, GLFW_KEY_I) == GLFW_PRESS ||
      glfwGetKey(g->window, GLFW_KEY_TAB) == GLFW_PRESS) {
    g->input.input_mask |= INPUT_Y;  // Menu/inventory
  }
  
  // Combine keyboard and gamepad input (take the stronger input)
  if (fabs(kb_horizontal) > fabs(gp_horizontal)) {
    g->input.horizontal = kb_horizontal;
  } else {
    g->input.horizontal = gp_horizontal;
  }
  
  if (fabs(kb_vertical) > fabs(gp_vertical)) {
    g->input.vertical = kb_vertical;
  } else {
    g->input.vertical = gp_vertical;
  }
}

EXPORT void update_externals(game *g) {
  double now = glfwGetTime();
  double dtd = now - g->_t_prev;
  g->_t_prev = now;
  // clamp to avoid huge steps after pauses
  if (dtd < 0.0) dtd = 0.0;
  if (dtd > 0.1) dtd = 0.1; // max 100 ms
  g->dt = (float)dtd;

  update_input(g);
  int display_w, display_h;
  glfwGetFramebufferSize(g->window, &display_w, &display_h);
  glViewport(0, 0, display_w, display_h);
  g->width = display_w;
  g->height = display_h;
  
  glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w,
               clear_color.z * clear_color.w, clear_color.w);
  glClear(GL_COLOR_BUFFER_BIT);

  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();

  g_update(g);

  ImGui::SetCurrentContext(g->ctx);
  ImGui::SetAllocatorFunctions(g->alloc_func, g->free_func, g->user_data);

  ImGui::Render();
  
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

  glfwSwapBuffers(g->window);
  glfwPollEvents();
  g->play = !glfwWindowShouldClose(g->window);
}

EXPORT void end_externals(game *g) {

}

EXPORT void assign_init(init func) {
  g_init = func;
}

EXPORT void assign_destroy(destroy func) {
  g_destroy = func;
}

EXPORT void assign_update(update func) {
  g_update = func;
}

EXPORT ImGuiContext *GetImguiContext() { return ImGui::GetCurrentContext(); }