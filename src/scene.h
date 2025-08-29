#ifndef SCENE_H
#define SCENE_H
#include "game.h"

extern sprite_sheet tiles;
extern sprite_sheet player;
extern sprite_sheet slime;

extern const animation_clip player_walk_down;
extern const animation_clip player_attack_anim;

struct Scene {
    int entity_count;
    entity entities[8];
};

extern Scene scene;

extern const int level[6][6];

#endif