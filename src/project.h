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

#define PROJECT_COMP_MAX 512

typedef struct { int entity; float position[3]; } project_transform;
typedef struct { int entity; float y; } project_rotation;
typedef struct { int entity; float value[3]; } project_scale;
typedef struct { int entity; int parent; } project_parent_transform;
typedef struct { int entity; } project_parent_rotation;
typedef struct { int entity; char model[64]; int visible; } project_mesh;
typedef struct { int entity; char asset[64]; int playing; int clip; float time; float speed; } project_anim;
typedef struct { int entity; float value[2]; } project_velocity;
typedef struct { int entity; int use_gravity; } project_rigid_body;
typedef struct { int entity; float move_speed; float jump_speed; } project_character_controller;
typedef struct { int entity; float current; float max; } project_health;
typedef struct { int entity; float half_extents[3]; } project_box_collider;
typedef struct { int entity; float radius; float half_height; } project_capsule_collider;
typedef struct { int entity; float fov; float near_plane; float far_plane; float target[3]; float up[3]; } project_cam;
typedef struct { int entity; char type_str[16]; int target; float radius; char joint[64]; } project_trigger;
typedef struct { int entity; char behavior[32]; float phase; } project_bot;

typedef enum { ASSET_MODEL, ASSET_ANIMATION, ASSET_DUNGEON_PIECE, ASSET_SPRITE } project_asset_type;

typedef struct {
    char key[64];
    char path[256];
    project_asset_type type;
} project_asset;

#define PROJECT_ASSET_MAX 88

typedef struct project_data {
    char name[128];
    int version;
    int autoplay;  /* 1 = enter play mode automatically on load */
    project_camera camera;
    int has_camera;
    project_lighting lighting;
    int has_lighting;
    project_entity entities[64];
    int entity_count;
    project_piece pieces[256];
    int piece_count;

    /* Unified asset registry */
    project_asset assets[PROJECT_ASSET_MAX];
    int asset_count;

    /* Scene entity names */
    char scene_entity_names[PROJECT_COMP_MAX][64];
    int scene_entity_count;

    /* Per-component tables (DKNF: no has_ flags, no NULL fields) */
    project_transform transforms[PROJECT_COMP_MAX];
    int transform_count;
    project_rotation rotations[PROJECT_COMP_MAX];
    int rotation_count;
    project_scale scales[PROJECT_COMP_MAX];
    int scale_count;
    project_parent_transform parent_transforms[PROJECT_COMP_MAX];
    int parent_transform_count;
    project_parent_rotation parent_rotations[PROJECT_COMP_MAX];
    int parent_rotation_count;
    project_mesh meshes[PROJECT_COMP_MAX];
    int mesh_count;
    project_anim anims[PROJECT_COMP_MAX];
    int anim_count;
    project_velocity velocities[PROJECT_COMP_MAX];
    int velocity_count;
    project_rigid_body rigid_bodies[PROJECT_COMP_MAX];
    int rigid_body_count;
    project_character_controller character_controllers[PROJECT_COMP_MAX];
    int character_controller_count;
    project_health healths[PROJECT_COMP_MAX];
    int health_count;
    project_box_collider box_colliders[PROJECT_COMP_MAX];
    int box_collider_count;
    project_capsule_collider capsule_colliders[PROJECT_COMP_MAX];
    int capsule_collider_count;
    project_cam cameras[PROJECT_COMP_MAX];
    int camera_count;
    project_trigger triggers[PROJECT_COMP_MAX];
    int trigger_count;
    project_bot bots[PROJECT_COMP_MAX];
    int bot_count;
} project_data;

const project_mesh *project_find_mesh(const project_data *p, int entity);
const project_anim *project_find_anim(const project_data *p, int entity);
const project_box_collider *project_find_box_collider(const project_data *p, int entity);
const char *project_find_asset(const project_data *p, const char *key, project_asset_type type);

int project_load(const char *path, project_data *out);

#endif /* PROJECT_H */
