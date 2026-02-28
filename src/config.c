#include "config.h"
#include <stdio.h>

/* Load configuration from TOML file */
int config_load(config *cfg, const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "Warning: Config file not found: %s\n", path);
        return -1;  // Use defaults
    }
    
    /* Read entire file into buffer */
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    char *buffer = (char*)malloc(size + 1);
    if (!buffer) {
        fclose(fp);
        return -1;
    }
    
    fread(buffer, 1, size, fp);
    buffer[size] = '\0';
    fclose(fp);
    
    /* Parse TOML and update config */
    /* Using toml.hpp for parsing */
    // TODO: Implement actual TOML parsing
    
    free(buffer);
    return 0;  // Success (uses defaults if file not found)
}

/* Save configuration to TOML file */
int config_save(const config *cfg, const char *path) {
    FILE *fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr, "Error: Cannot save config to: %s\n", path);
        return -1;
    }
    
    /* Write TOML format */
    fprintf(fp, "# Anitra Engine Configuration\n");
fprintf(fp, "[assets]\n");
    fprintf(fp, "default_model_path = \"%s\"\n", cfg->default_model_path);
fprintf(fp, "texture_health_bar = \"%s\"\n", cfg->texture_health_bar);
    fprintf(fp, "texture_health_fill = \"%s\"\n", cfg->texture_health_fill);
    fprintf(fp, "font_editor = \"%s\"\n\n", cfg->font_editor);
    
    fprintf(fp, "[shaders]\n");
    fprintf(fp, "sprite_vs = \"%s\"\n", cfg->shader_sprite_vs);
    fprintf(fp, "sprite_fs = \"%s\"\n", cfg->shader_sprite_fs);
    fprintf(fp, "debug_lines_vs = \"%s\"\n", cfg->shader_debug_lines_vs);
    fprintf(fp, "debug_lines_fs = \"%s\"\n", cfg->shader_debug_lines_fs);
    fprintf(fp, "ui_rect_vs = \"%s\"\n", cfg->shader_ui_rect_vs);
    fprintf(fp, "ui_rect_fs = \"%s\"\n", cfg->shader_ui_rect_fs);
    fprintf(fp, "font_vs = \"%s\"\n", cfg->shader_font_vs);
    fprintf(fp, "font_fs = \"%s\"\n", cfg->shader_font_fs);
    fprintf(fp, "mesh_vs = \"%s\"\n", cfg->shader_mesh_vs);
    fprintf(fp, "mesh_fs = \"%s\"\n", cfg->shader_mesh_fs);
    fprintf(fp, "composite_vs = \"%s\"\n", cfg->shader_composite_vs);
    fprintf(fp, "composite_fs = \"%s\"\n\n", cfg->shader_composite_fs);
    fprintf(fp, "default_animation_path = \"%s\"\n\n", cfg->default_animation_path);
    
    fprintf(fp, "[game_loop]\n");
    fprintf(fp, "target_fps = %.1f\n", cfg->target_fps);
    fprintf(fp, "vsync_enabled = %d\n\n", cfg->vsync_enabled);
    
    fprintf(fp, "[hot_reload]\n");
    fprintf(fp, "enabled = %d\n", cfg->hot_reload_enabled);
    fprintf(fp, "event_name = \"%s\"\n\n", cfg->hot_reload_event_name);
    
    fprintf(fp, "[debug]\n");
    fprintf(fp, "show_debug_lines = %d\n", cfg->show_debug_lines);
    fprintf(fp, "show_profiler = %d\n", cfg->show_profiler);
    
    fclose(fp);
    return 0;  // Success
}
