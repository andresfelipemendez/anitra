#include "scene.h"
#include <string.h>

/* Sprite sheets */
sprite_sheet tiles = {
    .texture_id = TEXTURE_TILES,
    .width = 32,
    .height = 32,
};

sprite_sheet player = {
    .texture_id = TEXTURE_PLAYER,
    .width = 16,
    .height = 24,
};

sprite_sheet slime = {
    .texture_id = TEXTURE_SLIME,
    .width = 16,
    .height = 16,
};

/* Animation clips */
const animation_clip player_walk_down = {
    .frames = {0, 1, 2, 3},
    .frame_time = 0.15f,
    .frame_count = 4,
    .duration = 0.6f,
};

const animation_clip player_attack_anim = {
    .frames = {4, 5, 6, 7},
    .frame_time = 0.1f,
    .frame_count = 4,
    .duration = 0.3f,
};

/* Scene state */
Scene scene = {0};

void scene_init(void) {
    /* Initialize scene with default entities */
    memset(&scene, 0, sizeof(scene));
    
    /* Set capacity if not already set */
    if (scene.entity_capacity == 0) {
        scene.entity_capacity = 64;
    }
}
