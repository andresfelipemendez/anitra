# Clay + GPU Vector Fonts + HarfBuzz — Editor UI

## Goal

Add an immediate-mode UI system to the engine using Clay for layout, GPU vector font rendering (Bezier curves in fragment shader) for text, and HarfBuzz for text shaping. This enables building editor panels (entity inspector, console, debug overlays) and eventually game HUD elements.

## Architecture

Three new subsystems, all in **externals** (the host-side rendering module that owns the GPU):

### 1. Font System

Load a `.ttf` via `stb_truetype`. For each glyph:

- Extract quadratic Bezier curves from `stbtt_GetGlyphShape`
- Convert line segments to degenerate Beziers (uniform GPU processing)
- Build 8x8 grid acceleration structure (precompute which curves intersect each cell)
- Upload curve buffer + grid cell/index buffers to GPU storage buffers
- Store `GlyphInfo` (curve offset, count, bbox, advance, grid offset) per glyph on CPU

GPU data per font (~256 glyphs):
- Curve buffer: ~120 KB (256 glyphs * 20 curves * 24 bytes)
- Grid data: ~250 KB (256 glyphs * ~1 KB per grid)
- Trivial memory footprint

### 2. Text Shaping (HarfBuzz)

HarfBuzz converts Unicode codepoints into positioned glyph sequences, handling:
- Ligatures (f + i -> fi glyph)
- Kerning classes (GPOS table)
- Contextual forms (if ever needed for non-Latin)

Pipeline:
```
Unicode string + font + script/language
  -> hb_buffer_add_utf8()
  -> hb_shape(hb_font, hb_buffer, features, count)
  -> iterate: glyph_id, x_advance, y_advance, x_offset, y_offset
```

HarfBuzz uses stb_truetype as its font backend via `hb_font_funcs_t` — no FreeType dependency.

Text measurement (for Clay layout) also goes through HarfBuzz: shape the string, sum the advances. This guarantees layout width matches rendered width exactly.

### 3. Clay Integration

Clay is a single-header C99 layout library. It takes declarative UI descriptions, computes flexbox layout, and outputs a flat `RenderCommandArray`. No GPU calls, no heap allocation.

Setup:
- Allocate ~3.5 MB arena for Clay internals (persistent across frames)
- Wire `Clay_SetMeasureTextFunction` to HarfBuzz-based measurement
- Each frame: set input state, declare UI, end layout, iterate render commands

### 4. UI Renderer

Processes Clay's render command array after the sprite/line passes:

| Command Type | Rendering |
|---|---|
| RECTANGLE | Colored quad (ui_rect pipeline) |
| TEXT | Per-glyph quads (font pipeline, Bezier fragment shader) |
| IMAGE | Textured quad (sprite pipeline, reuse existing) |
| BORDER | Line quads around bounding box |
| SCISSOR_START | SDL_SetGPUScissor |
| SCISSOR_END | Clear scissor |

Two new PSOs:
- `ui_rect_pipeline` — simple colored quads (position + color vertex, solid color fragment)
- `font_pipeline` — text quads (position + glyph_id + UV vertex, Bezier winding number fragment)

## Font Fragment Shader

For each pixel in a glyph quad:
1. Look up which grid cell the pixel falls in
2. Iterate only the curves that intersect that cell (typically 2-5)
3. For each curve: solve quadratic `at^2 + bt + c = 0` for ray-curve intersections
4. Accumulate winding number from valid crossings
5. Output coverage as alpha (with 1px anti-aliasing window)

Grid acceleration reduces per-pixel work from 20-40 curves to 2-5 curves. For an editor with ~2000 visible characters, this keeps the shader cost manageable.

## New Files

| File | Purpose |
|---|---|
| `src/stb_truetype.h` | Single header — glyph outline extraction + metrics |
| `lib/clay/clay.h` | Single header — UI layout engine |
| `lib/harfbuzz/` | HarfBuzz source (C library, built into externals) |
| `assets/fonts/Inter.ttf` | Bundled editor font (SIL Open Font License) |
| `assets/shaders/ui_rect_vs.hlsl` | Vertex shader for UI colored quads |
| `assets/shaders/ui_rect_fs.hlsl` | Fragment shader for UI colored quads |
| `assets/shaders/font_vs.hlsl` | Vertex shader for text quads |
| `assets/shaders/font_fs.hlsl` | Fragment shader — Bezier winding number |

## Changes to Existing Files

### externals.cpp

- `init_externals`: load font (.ttf -> curves -> GPU buffers), init HarfBuzz font, init Clay, create UI + font PSOs
- `update_externals`: after sprite/line rendering, run Clay layout then UI render pass
- New static data: font buffers, Clay arena, HarfBuzz font handle, UI pipelines

### build.c

- Add HarfBuzz to the externals build (compile harfbuzz source files, link into externals DLL)
- Add stb_truetype.h include path

## Data Flow Per Frame

```
SDL_PollEvent -> mouse/keyboard state
  |
  v
Clay_SetPointerState(mouse_x, mouse_y, mouse_down)
Clay_SetLayoutDimensions(window_w, window_h)
Clay_UpdateScrollContainers(true, scroll_delta, dt)
  |
  v
Clay_BeginLayout()
  declare_editor_ui(game_state)   // immediate-mode UI declarations
Clay_EndLayout() -> RenderCommandArray
  |
  v
ui_render(commands):
  for each command:
    RECTANGLE -> emit colored quad vertices
    TEXT -> hb_shape() -> glyph positions -> emit quad per glyph
    IMAGE -> emit textured quad
    BORDER -> emit line quads
    SCISSOR -> set/clear GPU scissor rect
  |
  v
  upload vertex buffer to GPU
  bind ui_rect_pipeline -> draw rect/border quads
  bind font_pipeline + curve/grid storage buffers -> draw text quads
```

## What This Does NOT Include

- Game HUD via Clay (just editor panels first — add game HUD later)
- Rounded corner rects (add when needed)
- Coarse rasterization optimization (profile first, add if font shader is bottleneck)
- Multiple fonts (single font first, extend Font struct to array later)
- On-demand glyph loading (load ASCII range at startup, extend when needed)

## Dependencies

- **Clay**: https://github.com/nicbarker/clay — single header, copy into lib/clay/
- **stb_truetype**: https://github.com/nothings/stb — single header, copy to src/
- **HarfBuzz**: https://github.com/harfbuzz/harfbuzz — C library, subset of source files needed
- **Inter font**: https://rsms.me/inter/ — SIL Open Font License, download .ttf
