/* Game configuration - loaded at startup */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

typedef struct config {
    /* Asset paths - can be overridden via config file or command line */
    const char *default_model_path;
    const char *default_animation_path;
    
    /* Window settings */
    int window_width;
    int window_height;
typedef struct config {
    /* Asset paths - can be overridden via config file or command line */
    const char *default_model_path;
    const char *default_animation_path;
    
    /* Texture paths */
    const char *texture_player;
    const char *texture_tiles;
    const char *texture_slime;
    const char *texture_health_bar;
    const char *texture_health_fill;
    const char *font_editor;
    
    /* Shader paths (SPIR-V compiled) */
    const char *shader_sprite_vs;
    const char *shader_sprite_fs;
    const char *shader_debug_lines_vs;
    const char *shader_debug_lines_fs;
    const char *shader_ui_rect_vs;
    const char *shader_ui_rect_fs;
    const char *shader_font_vs;
    const char *shader_font_fs;
    const char *shader_mesh_vs;
/* Default configuration values */
static inline config config_defaults(void) {
    return (config){
        .default_model_path = "assets/models/Knight.glb",
        .default_animation_path = "assets/animations/Rig_Medium_General.glb",
        .texture_player = "assets/char_spritesheet.png",
        .texture_tiles = "assets/Dungeon_Tileset.png",
        .texture_slime = "assets/pinkslime_spritesheet.png",
        .texture_health_bar = "assets/health_bar_hud.png",
        .texture_health_fill = "assets/health_hud.png",
        .font_editor = "assets/fonts/SourceCodePro-Regular.ttf",
        .shader_sprite_vs = "assets/shaders/compiled/sprite_vs.spv",
        .shader_sprite_fs = "assets/shaders/compiled/sprite_fs.spv",
        .shader_debug_lines_vs = "assets/shaders/compiled/debug_lines_vs.spv",
        .shader_debug_lines_fs = "assets/shaders/compiled/debug_lines_fs.spv",
        .shader_ui_rect_vs = "assets/shaders/compiled/ui_rect_vs.spv",
        .shader_ui_rect_fs = "assets/shaders/compiled/ui_rect_fs.spv",
        .shader_font_vs = "assets/shaders/compiled/font_vs.spv",
        .shader_font_fs = "assets/shaders/compiled/font_fs.spv",
        .shader_mesh_vs = "assets/shaders/compiled/mesh_vs.spv",
        .shader_mesh_fs = "assets/shaders/compiled/mesh_fs.spv",
        .shader_composite_vs = "assets/shaders/compiled/composite_vs.spv",
        .shader_composite_fs = "assets/shaders/compiled/composite_fs.spv",
        .window_width = 1600,
static inline config config_defaults(void) {
    return (config){
        .default_model_path = "assets/models/Knight.glb",
        .default_animation_path = "assets/animations/Rig_Medium_General.glb",
        .texture_player = "assets/char_spritesheet.png",
        .texture_tiles = "assets/Dungeon_Tileset.png",
        .texture_slime = "assets/pinkslime_spritesheet.png",
        .texture_health_bar = "assets/health_bar_hud.png",
        .texture_health_fill = "assets/health_hud.png",
        .font_editor = "assets/fonts/SourceCodePro-Regular.ttf",
        .window_width = 1600,
        .show_debug_lines = 1,
        .show_profiler = 0
    };
}

/* Load configuration from file (stub - implement with TOML parser) */
int config_load(config *cfg, const char *path);

/* Save configuration to file (stub) */
int config_save(const config *cfg, const char *path);

#endif /* CONFIG_H */
