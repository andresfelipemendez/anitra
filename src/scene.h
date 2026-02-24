#ifndef SCENE_H
#define SCENE_H
#include "game.h"

extern struct sprite_sheet tiles;
extern struct sprite_sheet player;
extern struct sprite_sheet slime;

extern const animation_clip player_walk_down;
extern const animation_clip player_attack_anim;

typedef struct {
    int entity_count;
    entity entities[8];
} Scene;

extern Scene scene;

extern const int level[6][6];

#endif
