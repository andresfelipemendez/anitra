# Simplified Build System

## Overview

The build system has been simplified with a Python-based implementation that provides:
- ✅ Better readability and maintainability
- ✅ Easier to extend and modify
- ✅ Clean separation of concerns
- ✅ Platform-specific handling
- ✅ Proper error messages

---

## File Structure

```
scripts/
├── nobuild.h          # C helper library (extracted from build.c)
└── [legacy] build_helpers.bat  # Batch file helpers

build.py               # Main Python build system (recommended)
py_build.bat           # Windows batch wrapper for build.py
simple_build.bat       # Alternative TCC-based build script
```

---

## Quick Start

### Using Python Build System (Recommended)

```bash
# Build everything
python build.py all

# Build individual components
python build.py externals
python build.py core
python build.py engine
python build.py editor
python build.py exe

# Clean build artifacts
python build.py clean
```

### Using Batch Wrapper (Windows)

```batch
# Build everything
py_build.bat

# Build specific target
py_build.bat engine

# Clean
py_build.bat clean
```

---

## Build Targets

| Target | Description |
|--------|-------------|
| `all` | Build externals, core, engine, editor, and executable |
| `externals` | Build SDL3/GPU/UI initialization DLL |
| `core` | Build hot-reload coordinator DLL |
| `engine` | Build gameplay logic DLL |
| `editor` | Build docking UI system DLL |
| `exe` | Build main executable |
| `clean` | Remove all build artifacts |

---

## How It Works

### Dependency Graph
```
AnitraEngine.exe ← core.dll ← externals.dll (SDL3)
                 ↑           └─ engine.dll (hot reload)
                 └─────────── editor.dll (hot reload)
```

### Build Order
1. **externals** - Initialize SDL3, GPU, UI systems
2. **core** - Hot-reload coordinator, depends on externals
3. **engine** - Gameplay logic (physics, animation, scene)
4. **editor** - Docking UI system
5. **exe** - Main entry point

### Compilation Flow
```
src/*.c  →  build/obj/*/*.obj  →  build/Debug/*.dll/.exe
   ↓              ↓                    ↓
 TCC/C compiler  Object files       Linker (TCC)
```

---

## Configuration

### Build Directory Structure
```
build/
├── Debug/                    # Output directory
│   ├── externals.dll
│   ├── core.dll
│   ├── engine.dll
│   ├── editor.dll
│   └── AnitraEngine.exe
└── obj/                      # Object files
    ├── externals/*.obj
    ├── core/*.obj
    ├── engine/*.obj
    ├── editor/*.obj
    └── exe/*.obj
```

### Include Paths
```python
INCLUDES = [
    "-Isrc",           # Main source directory
    "-Isrc/core",
    "-Isrc/engine",
    "-Isrc/editor",
    "-Isrc/externals",
]
```

---

## Usage Examples

### Development Workflow
```bash
# Make changes to engine.c
python build.py engine

# Run game with hot reload support
.\build\Debug\AnitraEngine.exe

# Press Ctrl+R in game to trigger hot-reload
```

### CI/CD Integration
```yaml
# .github/workflows/build.yml
name: Build Anitra Engine

steps:
  - uses: actions/checkout@v2
  
  - name: Build
    run: python build.py all
    
  - name: Test
    run: |
      # Run tests if implemented
```

### Custom Build Script
```python
# my_custom_build.py
import build as anitra_build

# Add custom pre-build steps
def pre_build():
    print("Running custom pre-build tasks...")
    return True

# Override the all target
def build_all_custom():
    if not pre_build():
        return False
    return anitra_build.build_all()

if __name__ == "__main__":
    anitra_build.main()
```

---

## Platform Support

### Windows (Primary)
- Uses TCC compiler (`tcc.exe`)
- Generates `.dll` and `.exe` files
- Full hot-reload support

### Linux (Experimental)
```bash
# Edit build.py to use cc instead of tcc
# Change: "tcc.exe" → "cc"
# Change: ".dll" → ".so"

python build.py all
```

---

## Migration from Old Build System

### Before (C-based)
```batch
.\build.bat engine
```

### After (Python-based)
```bash
python build.py engine
# or
py_build.bat engine
```

### Command Reference

| Old Command | New Command |
|-------------|-------------|
| `build.bat all` | `python build.py all` |
| `build.bat externals` | `python build.py externals` |
| `build.bat clean` | `python build.py clean` |

---

## Benefits Over Original Build System

### 1. Readability
```python
# Clear and obvious what this does
def build_engine():
    src_files = [SRC_DIR / "engine" / f"{f}.c"
                 for f in ["engine", "renderer", ...]]
```

vs.

```c
// Complex macro with string concatenation
#define TCC_ENGINE_CMD \
    ".\\tcc.exe -Blib/tcc-windows -shared" \
    " -o build/Debug/engine.dll" \
    ...
```

### 2. Error Handling
```python
try:
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"ERROR: {result.stderr}")
        return False
except Exception as e:
    print(f"Command failed: {e}")
    return False
```

### 3. Extensibility
```python
# Easy to add new targets
def build_tests():
    # Just implement the function
    pass

targets = {
    "all": build_all,
    "tests": build_tests,  # ← Add one line
}
```

---

## Troubleshooting

### "tcc.exe not found"
```bash
# Ensure TCC is in PATH or in repository root
python -c "import os; print(os.environ['PATH'])"
```

### "Failed to create directory"
```bash
# Check permissions
python build.py clean  # Try clean first
```

### Build fails with missing headers
```bash
# Verify include paths
python build.py engine 2>&1 | grep -i error
```

---

## Future Enhancements

- [ ] Incremental builds (check timestamps)
- [ ] Parallel compilation
- [ ] Dependency tracking
- [ ] Config file support
- [ ] Hot-reload trigger integration
- [ ] Unit test runner

---

## Summary

The simplified build system provides:
- ✅ Cleaner, more maintainable code
- ✅ Better error messages
- ✅ Easier to extend
- ✅ Platform abstraction
- ✅ Familiar Python syntax

Use `python build.py` instead of the complex C-based build system!
