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
    {16, 0,  16, 16},
    {32, 0,  16, 16},
    {0,  16, 16, 16},
    {16, 16, 16, 16},
};

static const struct {
    int entity_count;
    struct {
        float x, y;      
        int sprite_id;   
    } entity_data[8];
} scene_config = {
    .entity_count = 3,
    .entity_data = {
        {.x = 0.0f,   .y = 0.0f,   .sprite_id = 0},
        
    }
};

static const int level[][6] = {
    {0,0,0,0,0,0},
    {0,1,1,1,0,0},
    {0,1,0,1,0,0},
};

#endif