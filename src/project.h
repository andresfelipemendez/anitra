#ifndef PROJECT_H
#define PROJECT_H

/* Parsed project data — populated from TOML, consumed by engine */

typedef struct project_camera {
    float eye[3], target[3], up[3], fov;
} project_camera;

typedef struct project_entity {
    char id[64];
    char type[32];       /* "enemy", "interactable", "player" */
    char model[64];      /* asset key */
    char animations[64];
    float position[3];
    float rotation;
    int health;
    float speed;
    float collider_half_extents[3];
    /* AI */
    char ai_behavior[32];
    float patrol_points[8][3];
    int patrol_count;
    float aggro_range;
    /* Interaction */
    char interaction_type[32];
    char items[4][64];
    int item_count;
} project_entity;

typedef struct project_piece {
    char asset[64];      /* key into dungeon_pieces */
    float position[3];
    float rotation;
    float scale;
} project_piece;

typedef struct project_point_light {
    float position[3];
    float color[3];
    float intensity;
    float radius;
} project_point_light;

typedef struct project_lighting {
    float ambient[3];
    project_point_light point_lights[8];
    int point_light_count;
} project_lighting;

#define PROJECT_SCENE_MAX_ENTITIES 512

typedef struct project_scene_component {
    int has_transform;
    float transform_position[3];

    int has_rotation;
    float rotation_y;

    int has_scale;
    float scale[3];

    int has_parent_transform;
    int parent_transform_entity;

    int has_parent_rotation;

    int has_mesh;
    char mesh_model[64];
    char mesh_kind[32];
    int mesh_visible;

    int has_animation;
    char animation_asset[64];
    int animation_playing;
    int animation_clip;
    float animation_time;
    float animation_speed;

    int has_velocity;
    float velocity[2];

    int has_health;
    float health_current;
    float health_max;

    int has_collider;
    char collider_type[32];
    float collider_half_extents[3];

    int has_camera;
    float camera_fov;
    float camera_near;
    float camera_far;
    float camera_target[3];
    float camera_up[3];
} project_scene_component;

typedef struct project_data {
    char name[128];
    int version;
    project_camera camera;
    int has_camera;
    project_lighting lighting;
    int has_lighting;
    project_entity entities[64];
    int entity_count;
    project_piece pieces[256];
    int piece_count;
    /* Asset path registry (resolved full paths) */
    char model_paths[32][256];
    char model_keys[32][64];
    int model_count;
    char animation_paths[8][256];
    char animation_keys[8][64];
    int animation_count;
    char dungeon_piece_paths[32][256];
    char dungeon_piece_keys[32][64];
    int dungeon_piece_count;
    char sprite_paths[16][256];
    char sprite_keys[16][64];
    int sprite_count;

    char scene_entity_names[PROJECT_SCENE_MAX_ENTITIES][64];
    int scene_entity_count;
    project_scene_component scene_components[PROJECT_SCENE_MAX_ENTITIES];
} project_data;

int project_load(const char *path, project_data *out);

#endif /* PROJECT_H */
