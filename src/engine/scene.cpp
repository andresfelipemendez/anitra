#include "scene.h"

sprite_sheet tiles = {
    .texture_id = TEXTURE_TILES,
    .width = 192,
    .height = 208,
    .sprites = {
        {112,  144,    16, 16},
        {112,    176,  16, 16},
    }
};

sprite_sheet player = {
    .texture_id = TEXTURE_PLAYER,
    .width = 415,
    .height = 368,
    .sprites = {
        {16,  16,  16, 16},
        {32,  16,  16, 16},
        {48,  16,  16, 16},
        {64,  16,  16, 16},
        {80,  16,  16, 16},
    }
};

sprite_sheet slime = {
    .texture_id = TEXTURE_SLIME,
    .width = 192,
    .height = 80,
    .sprites = {
        {0,  64,  16, 16},
        {16,  64,  16, 16},
        {32,  64,  16, 16},
        {48,  64,  16, 16},
        {64,  64,  16, 16},
        {80,  64,  16, 16},
    }
};

const animation_clip player_walk_down = {
    .frames = {0,1,2,3,4},
    .frame_count = 5,
    .frame_time = 0.16f,
};

Scene scene = {
    .entity_count = 2,
    .entities = {
        {
            .pos = {160.0f, 160.0f},  // Position in 4x pixel coordinates
            .health = 100.0f,
            .sprite_sheet = player,
            .current_animation = {
                .animation = player_walk_down,
            }
        },
        {
            .pos = {160.0f, 80.0f},  // Position in 4x pixel coordinates
            .health = 5.0f,
            .sprite_sheet = slime,
            .current_animation = {
                .animation = player_walk_down,
            }
        },
    }
};

const int level[6][6] = {
    {1,1,1,1,1,1},
    {1,1,1,1,1,1},
    {1,1,1,1,1,1},
    {1,1,1,1,1,1},
    {1,1,1,1,1,1},
    {1,1,1,1,1,1},
};
