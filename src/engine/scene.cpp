#include "scene.h"
#include "game.h"

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
        {0,  192,   48, 48},
        {48, 192,   48, 48},
        {96, 192,   48, 48},
        {144, 192,   48, 48},
        {192, 192,   48, 48},
        {240, 192,   48, 48},
        
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
    .frames = {5,6,7},
    .frame_time = 0.16f,
    .frame_count = 5,
};

const animation_clip slime_idle = {
    .frames = {0,1,2,3,4,5},
    .frame_time = 0.16f,
    .frame_count = 5,
};

const animation_clip player_attack_anim = {
    .frames = {5,6,7,8,9,10},
    .frame_time = 0.08f,
    .frame_count = 6,
};

Scene scene = {
    .entity_count = 2,
    .entities = {
        {
            .sprite_sheet = player,
            .current_animation = {
                .animation = player_attack_anim,
            },
            .collider = {
                .rect = {160.0f, 160.0f, 58.0f, 64.0f},
                .type = COLLIDER,
            },
            .pos = {160.0f, 160.0f},
            .velocity = {0.0f, 0.0f},
            .health = 100.0f,
            .type = PLAYER,
        },
        {
            .sprite_sheet = slime,
            .current_animation = {
                .animation = slime_idle,
            },
            .collider = {
                .rect = {160.0f, 80.0f, 59.0f, 50.0f},
                .type = COLLIDER,
            },
            .pos = {160.0f, 80.0f},
            .velocity = {0.0f, 0.0f},
            .health = 50.0f,
            .type = ENEMY,
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
