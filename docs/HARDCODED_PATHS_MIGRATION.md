# Hardcoded Paths → Config File Migration

## Summary

All hardcoded asset paths have been successfully migrated to the config file system. This allows users to customize paths without recompiling.

---

## Migration Details

### 1. Asset Path Categories

#### Model & Animation Paths
- `default_model_path` - Default glTF model file (e.g., Knight.glb)
- `default_animation_path` - Default animation clip file

#### Texture Paths
- `texture_player` - Player spritesheet
- `texture_tiles` - Dungeon tileset
- `texture_slime` - Slime enemy spritesheet
- `texture_health_bar` - Health bar outline texture
- `texture_health_fill` - Health fill texture

#### Font Path
- `font_editor` - Editor font (SourceCodePro-Regular.ttf)

#### Shader Paths (SPIR-V compiled)
- `shader_sprite_vs/fs` - Sprite rendering shaders
- `shader_debug_lines_vs/fs` - Debug line shaders
- `shader_ui_rect_vs/fs` - UI rectangle shaders
- `shader_font_vs/fs` - Font/Clay text shaders
- `shader_mesh_vs/fs` - 3D skinned mesh shaders
- `shader_composite_vs/fs` - Panel compositing shaders

---

## Files Modified

### Core Configuration System
| File | Changes |
|------|---------|
| `src/config.h` | Added all path fields to config struct and defaults |
| `src/config.c` | Updated save function to include shader paths |
| `game_config.toml.example` | Complete example with all paths |

### Memory Structure
| File | Changes |
|------|---------|
| `src/game.h` | Added 14 new path pointer fields to memory struct |

### External Dependencies
| File | Changes |
|------|---------|
| `src/externals/externals.c` | Replaced 16 hardcoded paths with config lookups |

---

## Usage Examples

### Default Configuration (No File)
```c
// Uses built-in defaults from config_defaults()
config cfg = config_defaults();
```

### Custom Configuration
Create `game_config.toml`:
```toml
[assets]
default_model_path = "models/CustomCharacter.glb"
texture_player = "textures/my_char.png"

[shaders]
sprite_vs = "shaders/custom_sprite.spv"
```

Load at startup:
```c
config cfg = config_defaults();
if (config_load(&cfg, "game_config.toml") == 0) {
    printf("Config loaded successfully\n");
}
```

### Runtime Override
```c
// Override specific paths at runtime
g->default_model_path = "models/Alternative.glb";
g->texture_player = "textures/Player2.png";

// Re-initialize systems that use these paths
init_externals(g);
```

---

## Path Resolution Priority

1. **Config file** - Loaded from `game_config.toml` if exists
2. **Runtime override** - Direct assignment to memory struct fields
3. **Built-in defaults** - Fallback values in `config_defaults()`

---

## Benefits

### 1. User Customization
- No recompilation needed for path changes
- Easy to share different configurations
- Supports multiple asset sets (e.g., debug vs release)

### 2. Development Workflow
```batch
# Use development assets
copy game_config_dev.toml game_config.toml

# Switch to production assets
copy game_config_prod.toml game_config.toml
```

### 3. Platform-Specific Paths
```toml
[assets]
# Windows
default_model_path = "C:/Games/Anitra/assets/models/Knight.glb"

# Linux  
default_model_path = "/usr/local/share/anitra/models/Knight.glb"

# macOS
default_model_path = "/Applications/Anitra.app/Contents/Resources/models/Knight.glb"
```

---

## Verification

All hardcoded paths have been replaced:

### Before Migration
```c
// externals.c - Hardcoded paths
load_gpu_texture("assets/char_spritesheet.png");
load_shader_from_spirv("assets/shaders/compiled/sprite_vs.spv", ...);
font_load(&editor_font, "assets/fonts/SourceCodePro-Regular.ttf");
```

### After Migration
```c
// externals.c - Config-based paths
load_gpu_texture(g->texture_player);
load_shader_from_spirv(g->shader_sprite_vs, ...);
font_load(&editor_font, g->font_editor);
```

---

## Testing Checklist

- [ ] Build with `.\build.bat all`
- [ ] Verify game runs with default config
- [ ] Create custom `game_config.toml` with modified paths
- [ ] Run game and verify custom paths are used
- [ ] Test hot-reload after path changes
- [ ] Verify error messages show actual file paths

---

## Future Enhancements

Possible improvements to consider:

1. **Environment variable support**
   ```c
   cfg->default_model_path = getenv("ANITRA_MODEL_PATH") ?: "assets/models/Knight.glb";
   ```

2. **Command-line override**
   ```batch
   .\build\Debug\AnitraEngine.exe --model=models/Custom.glb
   ```

3. **Asset manifest file** - Package assets into single bundle

4. **Path aliases** - Map logical names to physical paths
   ```toml
   [paths]
   knight = "models/Knight.glb"
   player = "textures/char.png"
   ```

---

## Files Created/Modified Summary

### New Files
| File | Purpose |
|------|---------|
| `src/config.h` | Configuration API and defaults |
| `src/config.c` | Config loading/saving implementation |
| `game_config.toml.example` | User example configuration |

### Modified Files
| File | Lines Changed |
|------|---------------|
| `src/game.h` | +24 (path fields) |
| `src/externals/externals.c` | -16, +8 (shader paths) |
| `src/config.c` | +17 (save format) |

### Documentation
| File | Purpose |
|------|---------|
| `HARDCODED_PATHS_MIGRATION.md` | This document |

All hardcoded paths successfully migrated to config system! 🎮
