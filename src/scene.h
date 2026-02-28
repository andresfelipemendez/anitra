#ifndef SCENE_H
#define SCENE_H
#include "game.h"

extern sprite_sheet tiles;
extern sprite_sheet player;
extern sprite_sheet slime;

extern const animation_clip player_walk_down;
extern const animation_clip player_attack_anim;

typedef struct Scene {
    int entity_count;
    int entity_capacity;
    entity *entities;

    int parent_component_count;
    int parent_component_capacity;
    parent_component *parent_components;

    int parent_transform_component_count;
    int parent_transform_component_capacity;
    parent_transform_component *parent_transform_components;

    int parent_rotation_component_count;
    int parent_rotation_component_capacity;
    parent_rotation_component *parent_rotation_components;
} Scene;

extern Scene scene;
void scene_init(void);

extern const int level[6][6];

#endif
