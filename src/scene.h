#ifndef SCENE_H
#define SCENE_H
#include "game.h"

static const struct {
    int width;     
    int height;  
} spritesheet_info = {
    .width = 415,   
    .height = 368
};

static const pixel_rect char_sprites[] = {
    {16,  16,  16, 16},
    {32,  16,  16, 16},
    {48,  16,  16, 16},
    {64,  16,  16, 16},
    {80,  16,  16, 16},
};

struct sprite_sheet {
    int width;     
    int height;
    pixel_rect sprites[];
};

sprite_sheet tiles {
    .width = 192,
    .height = 208,
    .sprites = {
        {112,  144,    16, 16},
        {112,    176,  16, 16},
    }
};

static const pixel_rect tiles_sprites[] = {
    {16,  0,  16, 16},
    {0,    176,  16, 16},
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
    .entity_count = 1,
    .entities = {
        {
            .pos = {0.0f, 0.0f},
            .current_animation = {
                .animation = player_walk_down,

            }
        },
    }
};

static const int level[][6] = {
    {0,0,0,0,0,0},
    {0,1,1,1,0,0},
    {0,1,1,1,0,0},
    {0,1,1,1,0,0},
    {0,1,1,1,0,0},
    {0,0,0,0,0,0},
};

#endif