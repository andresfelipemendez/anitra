#ifndef SCENE_H
#define SCENE_H
#include "game.h"

sprite_sheet tiles {
    .texture_id = TEXTURE_TILES,
    .width = 192,
    .height = 208,
    .sprites = {
        {112,  144,    16, 16},
        {112,    176,  16, 16},
    }
};

sprite_sheet player {
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

sprite_sheet slime {
    .texture_id = TEXTURE_SLIME,
    .width = 192,
    .height = 208,
    .sprites = {
        {0,  170,  16, 32},
        {16,  170,  16, 32},
        {32,  170,  16, 32},
        {48,  170,  16, 32},
        {64,  170,  16, 32},
        {80,  170,  16, 32},
    }
};

static const animation_clip player_walk_down = {
    .frames = {0,1,2,3,4},
    .frame_count = 5,
    .frame_time = 0.16f,
};

static struct {
    int entity_count;
    entity entities[8];
} scene = {
    .entity_count = 2,
    .entities = {
        {
            .pos = {150.0f, 150.0f},
            .sprite_sheet = player,
            .current_animation = {
                .animation = player_walk_down,
            }
        },
        {
            .pos = {150.0f, 10.0f},
            .sprite_sheet = slime,
            .current_animation = {
                .animation = player_walk_down,
            }
        },
    }
};

static const int level[][6] = {
    {1,1,1,1,1,1},
    {1,1,1,1,1,1},
    {1,1,1,1,1,1},
    {1,1,1,1,1,1},
    {1,1,1,1,1,1},
    {1,1,1,1,1,1},
};

#endif