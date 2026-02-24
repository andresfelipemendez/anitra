#include <externals.h>
#include <game.h>
#include <debug_render.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include <SDL3/SDL.h>

#define STBI_NO_SIMD
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

init g_init = NULL;
destroy g_destroy = NULL;
update g_update = NULL;

static float clear_r = 0.45f, clear_g = 0.55f, clear_b = 0.60f;

SDL_Texture* externals_load_texture(SDL_Renderer* renderer, const char* filepath) {
    int width, height, nrChannels;
    stbi_set_flip_vertically_on_load(0);
    unsigned char* data = stbi_load(filepath, &width, &height, &nrChannels, 4);
    if (!data) {
        fprintf(stderr, "Failed to load texture: %s\n", filepath);
        return NULL;
    }

    SDL_Surface* surface = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32, data, width * 4);
    if (!surface) {
        fprintf(stderr, "Failed to create surface: %s\n", SDL_GetError());
        stbi_image_free(data);
        return NULL;
    }

    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    stbi_image_free(data);

    if (!texture) {
        fprintf(stderr, "Failed to create texture: %s\n", SDL_GetError());
        return NULL;
    }

    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
    printf("Loaded texture: %s (%dx%d)\n", filepath, width, height);
    return texture;
}

EXPORT int init_externals(struct game* g) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    g->window = SDL_CreateWindow("Anitra Engine", 800, 600, SDL_WINDOW_RESIZABLE);
    if (!g->window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return -1;
    }

    g->renderer = SDL_CreateRenderer(g->window, NULL);
    if (!g->renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return -1;
    }

    SDL_SetRenderVSync(g->renderer, 1);

    g->debug_renderer.max_lines = 1000;
    g->debug_renderer.vertex_buffer = (float*)malloc(g->debug_renderer.max_lines * 10 * sizeof(float));
    g->debug_renderer.current_line_count = 0;

    g->textures[TEXTURE_PLAYER] = externals_load_texture(g->renderer, "assets\\char_spritesheet.png");
    g->textures[TEXTURE_TILES] = externals_load_texture(g->renderer, "assets\\Dungeon_Tileset.png");
    g->textures[TEXTURE_SLIME] = externals_load_texture(g->renderer, "assets\\pinkslime_spritesheet.png");
    g->textures[TEXTURE_HEALTH_BAR] = externals_load_texture(g->renderer, "assets\\health_bar_hud.png");
    g->textures[TEXTURE_HEALTH_FILL] = externals_load_texture(g->renderer, "assets\\health_hud.png");

    g->_t_prev = (double)SDL_GetTicks() / 1000.0;
    g->dt = 0.0f;
    g->play = 1;

    printf("SDL3 initialized successfully\n");
    return 1;
}

EXPORT void init_engine(struct game* g) {
    g_init(g);
}

EXPORT void destroy_engine(struct game* g) {
    g_destroy(g);
}

void update_input(struct game* g) {
    g->input.horizontal = 0.0f;
    g->input.vertical = 0.0f;
    g->input.input_mask = 0;

    float kb_horizontal = 0.0f;
    float kb_vertical = 0.0f;

    const bool* keys = SDL_GetKeyboardState(NULL);

    if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT])  kb_horizontal -= 1.0f;
    if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) kb_horizontal += 1.0f;
    if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP])    kb_vertical += 1.0f;
    if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN])  kb_vertical -= 1.0f;

    float kb_magnitude = sqrtf(kb_horizontal * kb_horizontal + kb_vertical * kb_vertical);
    if (kb_magnitude > 1.0f) {
        kb_horizontal /= kb_magnitude;
        kb_vertical /= kb_magnitude;
    }

    if (keys[SDL_SCANCODE_SPACE] || keys[SDL_SCANCODE_J])                                     g->input.input_mask |= INPUT_A;
    if (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT] || keys[SDL_SCANCODE_K])       g->input.input_mask |= INPUT_B;
    if (keys[SDL_SCANCODE_E] || keys[SDL_SCANCODE_L])                                         g->input.input_mask |= INPUT_X;
    if (keys[SDL_SCANCODE_Q] || keys[SDL_SCANCODE_I] || keys[SDL_SCANCODE_TAB])               g->input.input_mask |= INPUT_Y;

    float gp_horizontal = 0.0f;
    float gp_vertical = 0.0f;

    int gamepad_count = 0;
    SDL_JoystickID* gamepads = SDL_GetGamepads(&gamepad_count);
    if (gamepads && gamepad_count > 0) {
        SDL_Gamepad* gp = SDL_OpenGamepad(gamepads[0]);
        if (gp) {
            gp_horizontal = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
            gp_vertical = -(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f);

            const float deadzone = 0.2f;
            float magnitude = sqrtf(gp_horizontal * gp_horizontal + gp_vertical * gp_vertical);
            if (magnitude < deadzone) {
                gp_horizontal = 0.0f;
                gp_vertical = 0.0f;
            } else {
                float normalized = (magnitude - deadzone) / (1.0f - deadzone);
                if (normalized > 1.0f) normalized = 1.0f;
                gp_horizontal = (gp_horizontal / magnitude) * normalized;
                gp_vertical = (gp_vertical / magnitude) * normalized;
            }

            if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_SOUTH)) g->input.input_mask |= INPUT_A;
            if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_EAST))  g->input.input_mask |= INPUT_B;
            if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_WEST))  g->input.input_mask |= INPUT_X;
            if (SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_NORTH)) g->input.input_mask |= INPUT_Y;

            SDL_CloseGamepad(gp);
        }
    }
    SDL_free(gamepads);

    if (fabsf(kb_horizontal) > fabsf(gp_horizontal))
        g->input.horizontal = kb_horizontal;
    else
        g->input.horizontal = gp_horizontal;

    if (fabsf(kb_vertical) > fabsf(gp_vertical))
        g->input.vertical = kb_vertical;
    else
        g->input.vertical = gp_vertical;
}

EXPORT void update_externals(struct game* g) {
    double now = (double)SDL_GetTicks() / 1000.0;
    double dtd = now - g->_t_prev;
    g->_t_prev = now;
    if (dtd < 0.0) dtd = 0.0;
    if (dtd > 0.1) dtd = 0.1;
    g->dt = (float)dtd;

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            g->play = 0;
            return;
        }
    }

    update_input(g);

    int display_w, display_h;
    SDL_GetRenderOutputSize(g->renderer, &display_w, &display_h);
    g->width = display_w;
    g->height = display_h;

    SDL_SetRenderDrawColor(g->renderer,
        (Uint8)(clear_r * 255), (Uint8)(clear_g * 255),
        (Uint8)(clear_b * 255), 255);
    SDL_RenderClear(g->renderer);

    g_update(g);

    SDL_RenderPresent(g->renderer);
}

EXPORT void end_externals(struct game* g) {
    for (int i = 0; i < TEXTURE_COUNT; i++) {
        if (g->textures[i]) {
            SDL_DestroyTexture(g->textures[i]);
            g->textures[i] = NULL;
        }
    }
    if (g->debug_renderer.vertex_buffer) {
        free(g->debug_renderer.vertex_buffer);
        g->debug_renderer.vertex_buffer = NULL;
    }
    if (g->renderer) SDL_DestroyRenderer(g->renderer);
    if (g->window) SDL_DestroyWindow(g->window);
    SDL_Quit();
}

EXPORT void assign_init(init func) { g_init = func; }
EXPORT void assign_destroy(destroy func) { g_destroy = func; }
EXPORT void assign_update(update func) { g_update = func; }
