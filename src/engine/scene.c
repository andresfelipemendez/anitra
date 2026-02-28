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
    .keyframes = {{-1},{-1}},
    .frame_time = 0.16f,
    .frame_count = 5,
};

const animation_clip slime_idle = {
    .frames = {0,1,2,3,4,5},
    .keyframes = {{-1},{-1}},
    .frame_time = 0.16f,
    .frame_count = 5,
};

const animation_clip player_attack_anim = {
    .frames = {5,6,7,8,9,10},
    .collider = {0,-45,60,30},
    .keyframes = {
        {2},{3}
    },
    .frame_time = 0.08f,
    .frame_count = 6,
};

Scene scene = {0};

void scene_init(void) {
    scene.entity_count = 2;
    scene.parent_component_count = 0;
    scene.parent_transform_component_count = 0;
    scene.parent_rotation_component_count = 0;

    /* Player entity */
    scene.entities[0].sprite_sheet = player;
    scene.entities[0].current_animation.animation = player_attack_anim;
    scene.entities[0].current_animation.timer = 0.0f;
    scene.entities[0].current_animation.frame_index = 0;
    scene.entities[0].current_animation.playing = 1;
    scene.entities[0].collider.rect.x = 160.0f;
    scene.entities[0].collider.rect.y = 160.0f;
    scene.entities[0].collider.rect.w = 58.0f;
    scene.entities[0].collider.rect.h = 64.0f;
    scene.entities[0].collider.type = COLLIDER;
    scene.entities[0].pos.x = 160.0f;
    scene.entities[0].pos.y = 160.0f;
    scene.entities[0].velocity.x = 0.0f;
    scene.entities[0].velocity.y = 0.0f;
    scene.entities[0].health = 100.0f;
    scene.entities[0].type = PLAYER;

    /* Slime entity */
    scene.entities[1].sprite_sheet = slime;
    scene.entities[1].current_animation.animation = slime_idle;
    scene.entities[1].current_animation.timer = 0.0f;
    scene.entities[1].current_animation.frame_index = 0;
    scene.entities[1].current_animation.playing = 1;
    scene.entities[1].collider.rect.x = 160.0f;
    scene.entities[1].collider.rect.y = 80.0f;
    scene.entities[1].collider.rect.w = 59.0f;
    scene.entities[1].collider.rect.h = 50.0f;
    scene.entities[1].collider.type = COLLIDER;
    scene.entities[1].pos.x = 160.0f;
    scene.entities[1].pos.y = 0.0f;
    scene.entities[1].velocity.x = 0.0f;
    scene.entities[1].velocity.y = 0.0f;
    scene.entities[1].health = 50.0f;
    scene.entities[1].type = ENEMY;

    if (scene.parent_component_capacity > 0 && scene.parent_components) {
        scene.parent_components[scene.parent_component_count].entity_index = 1;
        scene.parent_components[scene.parent_component_count].parent_entity_index = 0;
        scene.parent_component_count++;
    }
    if (scene.parent_transform_component_capacity > 0 && scene.parent_transform_components) {
        scene.parent_transform_components[scene.parent_transform_component_count].entity_index = 1;
        scene.parent_transform_component_count++;
    }
    if (scene.parent_rotation_component_capacity > 0 && scene.parent_rotation_components) {
        scene.parent_rotation_components[scene.parent_rotation_component_count].entity_index = 1;
        scene.parent_rotation_component_count++;
    }
}

const int level[6][6] = {
    {1,1,1,1,1,1},
    {1,1,1,1,1,1},
    {1,1,1,1,1,1},
    {1,1,1,1,1,1},
    {1,1,1,1,1,1},
    {1,1,1,1,1,1},
};
