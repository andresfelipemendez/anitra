# Nobuild Build System Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace CMake with a single C89 `build.c` that bootstraps itself and invokes MSVC `cl.exe`/`link.exe` to build the Anitra engine and all its DLLs.

**Architecture:** A single `build.c` file that compiles with `cl build.c` to produce `build.exe`. The program accepts a target name as argv[1] (`all`, `engine`, `externals`, `core`, `glad`, `tracy`, `clean`) and shells out to `cl.exe`/`link.exe`/`lib.exe` with the exact flags each target needs. Incremental builds via file modification time comparison. Random PDB names for hot-reload support.

**Tech Stack:** C89, MSVC cl.exe/link.exe/lib.exe, Win32 API (for FindFirstFile, GetFileTime, system())

---

### Task 1: Scaffold build.c with utilities and main dispatch

**Files:**
- Create: `build.c`

**Step 1: Write the build.c skeleton**

Write `build.c` with these sections:

1. **Utility functions** (all C89, all using Win32 API):
   - `run_cmd(const char *cmd)` — wraps `system()`, prints cmd, returns exit code
   - `needs_rebuild(const char *src, const char *obj)` — compares file modification times using `GetFileAttributesEx` / `FILETIME`. Returns 1 if src is newer or obj doesn't exist.
   - `ensure_dir(const char *path)` — creates directory if it doesn't exist using `CreateDirectory`
   - `rand_hex(char *buf, int len)` — generates random hex string from `time()` + `GetTickCount()` for PDB naming
   - `path_join(char *out, int out_sz, const char *a, const char *b)` — joins paths with backslash

2. **Constants** — all the paths and flags as `#define` or `static const char*`:
   - `BUILD_DIR` = `"build\\Debug"`
   - `OBJ_DIR` = `"build\\obj"`
   - Source root, lib paths, include paths per target

3. **`main(argc, argv)`** — parse argv[1], dispatch to the right build function:
   - No arg or `"all"` → build_all()
   - `"tracy"` → build_tracy()
   - `"glad"` → build_glad()
   - `"externals"` → build_externals()
   - `"core"` → build_core()
   - `"engine"` → build_engine()
   - `"exe"` → build_exe()
   - `"clean"` → build_clean()

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>

/* --- Configuration --- */
#define BUILD_DIR    "build\\Debug"
#define OBJ_DIR      "build\\obj"
#define MAX_CMD      8192
#define MAX_PATH_LEN 1024

/* Library paths */
#define GLFW_LIB     "lib\\glfw\\lib\\glfw3.lib"
#define GLFW_INC     "lib\\glfw\\include"
#define GLAD_DIR     "lib\\glad"
#define IMGUI_DIR    "lib\\imgui-1.90.9"
#define TRACY_DIR    "lib\\tracy"
#define TRACY_PUB    "lib\\tracy\\public"

/* Common compiler flags */
#define CXXFLAGS     "/std:c++20 /EHsc /MD /Zi /Od /nologo"
#define CFLAGS       "/MD /Zi /Od /nologo"

/* Common include paths used by most targets */
#define COMMON_INCLUDES \
    "/Isrc /Isrc\\core /Isrc\\engine /Isrc\\externals " \
    "/Iinclude " \
    "/I" IMGUI_DIR " /I" IMGUI_DIR "\\backends " \
    "/I" GLAD_DIR " /I" GLFW_INC " /I" TRACY_PUB

/* forward declarations for all build targets */
static int build_tracy(void);
static int build_glad(void);
static int build_externals(void);
static int build_core(void);
static int build_engine(void);
static int build_exe(void);
static int build_all(void);
static int build_clean(void);
```

**Step 2: Implement utility functions**

```c
static int run_cmd(const char *cmd) {
    int rc;
    printf("> %s\n", cmd);
    rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "FAILED (exit code %d): %s\n", rc, cmd);
    }
    return rc;
}

static int needs_rebuild(const char *src, const char *obj) {
    WIN32_FILE_ATTRIBUTE_DATA src_data, obj_data;
    if (!GetFileAttributesExA(obj, GetFileExInfoStandard, &obj_data)) {
        return 1; /* obj doesn't exist */
    }
    if (!GetFileAttributesExA(src, GetFileExInfoStandard, &src_data)) {
        return 0; /* src doesn't exist - nothing to do */
    }
    return CompareFileTime(&src_data.ftLastWriteTime, &obj_data.ftLastWriteTime) > 0;
}

static void ensure_dir(const char *path) {
    CreateDirectoryA(path, NULL);
    /* ignore error - it may already exist */
}

static void ensure_dirs(void) {
    ensure_dir("build");
    ensure_dir(BUILD_DIR);
    ensure_dir(OBJ_DIR);
    ensure_dir(OBJ_DIR "\\tracy");
    ensure_dir(OBJ_DIR "\\glad");
    ensure_dir(OBJ_DIR "\\externals");
    ensure_dir(OBJ_DIR "\\core");
    ensure_dir(OBJ_DIR "\\engine");
    ensure_dir(OBJ_DIR "\\exe");
}

static void rand_hex(char *buf, int len) {
    static const char hex[] = "0123456789ABCDEF";
    int i;
    unsigned int seed = (unsigned int)time(NULL) ^ GetTickCount();
    srand(seed);
    for (i = 0; i < len; i++) {
        buf[i] = hex[rand() % 16];
    }
    buf[len] = '\0';
}
```

**Step 3: Implement main dispatch**

```c
int main(int argc, char **argv) {
    const char *target;
    int rc;

    if (argc < 2) {
        target = "all";
    } else {
        target = argv[1];
    }

    if (strcmp(target, "all") == 0)           rc = build_all();
    else if (strcmp(target, "tracy") == 0)    rc = build_tracy();
    else if (strcmp(target, "glad") == 0)     rc = build_glad();
    else if (strcmp(target, "externals") == 0)rc = build_externals();
    else if (strcmp(target, "core") == 0)     rc = build_core();
    else if (strcmp(target, "engine") == 0)   rc = build_engine();
    else if (strcmp(target, "exe") == 0)      rc = build_exe();
    else if (strcmp(target, "clean") == 0)    rc = build_clean();
    else {
        fprintf(stderr, "Unknown target: %s\n", target);
        fprintf(stderr, "Usage: build [all|tracy|glad|externals|core|engine|exe|clean]\n");
        return 1;
    }

    if (rc == 0) printf("\n=== Build '%s' succeeded ===\n", target);
    else         printf("\n=== Build '%s' FAILED ===\n", target);
    return rc;
}
```

**Step 4: Test bootstrap compiles**

Run (from VS Developer Command Prompt):
```
cl build.c
build.exe
```
Expected: Should print usage or attempt build_all (which is a stub at this point).

**Step 5: Commit**

```bash
git add build.c
git commit -m "scaffold nobuild build.c with utilities and dispatch"
```

---

### Task 2: Implement build_tracy (static library)

**Files:**
- Modify: `build.c`

**Step 1: Implement build_tracy**

Tracy only needs `TracyClient.cpp` compiled (it `#include`s everything else internally). Produces a static library `TracyClient.lib`.

```c
static int build_tracy(void) {
    char cmd[MAX_CMD];
    const char *src = TRACY_PUB "\\TracyClient.cpp";
    const char *obj = OBJ_DIR "\\tracy\\TracyClient.obj";
    const char *lib_out = BUILD_DIR "\\TracyClient.lib";

    ensure_dirs();

    if (needs_rebuild(src, obj)) {
        snprintf(cmd, MAX_CMD,
            "cl " CXXFLAGS " /c /DTRACY_ENABLE /DTRACY_ON_DEMAND "
            "/I" TRACY_PUB " "
            "/Fo\"%s\" \"%s\"",
            obj, src);
        if (run_cmd(cmd) != 0) return 1;
    } else {
        printf("tracy: up to date\n");
    }

    /* Always re-link the static lib (fast operation) */
    snprintf(cmd, MAX_CMD,
        "lib /nologo /OUT:\"%s\" \"%s\"",
        lib_out, obj);
    if (run_cmd(cmd) != 0) return 1;

    printf("tracy: built %s\n", lib_out);
    return 0;
}
```

**Step 2: Test**

```
cl build.c && build.exe tracy
```
Expected: TracyClient.obj and TracyClient.lib in build/Debug/.

**Step 3: Commit**

```bash
git add build.c
git commit -m "implement build_tracy target"
```

---

### Task 3: Implement build_glad (DLL)

**Files:**
- Modify: `build.c`

**Step 1: Implement build_glad**

glad.c is C code, compiled as a DLL with `GLAD_GLAPI_EXPORT` defined.

```c
static int build_glad(void) {
    char cmd[MAX_CMD];
    const char *src = GLAD_DIR "\\glad.c";
    const char *obj = OBJ_DIR "\\glad\\glad.obj";

    ensure_dirs();

    if (needs_rebuild(src, obj)) {
        snprintf(cmd, MAX_CMD,
            "cl " CFLAGS " /c /DGLAD_GLAPI_EXPORT "
            "/I" GLAD_DIR " "
            "/Fo\"%s\" \"%s\"",
            obj, src);
        if (run_cmd(cmd) != 0) return 1;
    } else {
        printf("glad: up to date\n");
    }

    snprintf(cmd, MAX_CMD,
        "link /nologo /DLL /DEBUG "
        "/OUT:\"" BUILD_DIR "\\glad_loader.dll\" "
        "/IMPLIB:\"" BUILD_DIR "\\glad_loader.lib\" "
        "\"%s\" opengl32.lib " GLFW_LIB,
        obj);
    if (run_cmd(cmd) != 0) return 1;

    printf("glad: built glad_loader.dll\n");
    return 0;
}
```

**Step 2: Test**

```
cl build.c && build.exe glad
```
Expected: glad_loader.dll and glad_loader.lib in build/Debug/.

**Step 3: Commit**

```bash
git add build.c
git commit -m "implement build_glad target"
```

---

### Task 4: Implement build_externals (DLL)

**Files:**
- Modify: `build.c`

**Step 1: Implement build_externals**

Externals compiles `externals.cpp` + 7 ImGui sources. Depends on glad_loader and TracyClient.

```c
static int build_externals(void) {
    char cmd[MAX_CMD];
    char pdb_suffix[9];
    int i, any_rebuilt = 0;

    static const char *sources[] = {
        "src\\externals\\externals.cpp",
        IMGUI_DIR "\\imgui.cpp",
        IMGUI_DIR "\\imgui_demo.cpp",
        IMGUI_DIR "\\imgui_draw.cpp",
        IMGUI_DIR "\\imgui_tables.cpp",
        IMGUI_DIR "\\imgui_widgets.cpp",
        IMGUI_DIR "\\backends\\imgui_impl_glfw.cpp",
        IMGUI_DIR "\\backends\\imgui_impl_opengl3.cpp"
    };
    static const char *objs[] = {
        OBJ_DIR "\\externals\\externals.obj",
        OBJ_DIR "\\externals\\imgui.obj",
        OBJ_DIR "\\externals\\imgui_demo.obj",
        OBJ_DIR "\\externals\\imgui_draw.obj",
        OBJ_DIR "\\externals\\imgui_tables.obj",
        OBJ_DIR "\\externals\\imgui_widgets.obj",
        OBJ_DIR "\\externals\\imgui_impl_glfw.obj",
        OBJ_DIR "\\externals\\imgui_impl_opengl3.obj"
    };
    int n = sizeof(sources) / sizeof(sources[0]);

    ensure_dirs();

    for (i = 0; i < n; i++) {
        if (needs_rebuild(sources[i], objs[i])) {
            snprintf(cmd, MAX_CMD,
                "cl " CXXFLAGS " /c "
                "/DIMGUI_IMPL_OPENGL_LOADER_GLAD /DGLAD_GLAPI_IMPORT /DTRACY_ENABLE "
                COMMON_INCLUDES " "
                "/Fo\"%s\" \"%s\"",
                objs[i], sources[i]);
            if (run_cmd(cmd) != 0) return 1;
            any_rebuilt = 1;
        }
    }

    if (!any_rebuilt) {
        printf("externals: up to date\n");
    }

    /* Link DLL with random PDB for hot-reload */
    rand_hex(pdb_suffix, 8);
    snprintf(cmd, MAX_CMD,
        "link /nologo /DLL /DEBUG "
        "/PDB:\"" BUILD_DIR "\\externals_%s.pdb\" "
        "/OUT:\"" BUILD_DIR "\\externals.dll\" "
        "/IMPLIB:\"" BUILD_DIR "\\externals.lib\" "
        "%s %s %s %s %s %s %s %s "
        "\"" BUILD_DIR "\\glad_loader.lib\" "
        "\"" BUILD_DIR "\\TracyClient.lib\" "
        "opengl32.lib " GLFW_LIB " "
        "ws2_32.lib dbghelp.lib advapi32.lib user32.lib",
        pdb_suffix,
        objs[0], objs[1], objs[2], objs[3],
        objs[4], objs[5], objs[6], objs[7]);
    if (run_cmd(cmd) != 0) return 1;

    printf("externals: built externals.dll\n");
    return 0;
}
```

Note: We always re-link even if no .obj changed, because re-linking is fast and the random PDB name means we can't rely on output file existence for skip logic.

**Step 2: Test**

```
cl build.c && build.exe tracy && build.exe glad && build.exe externals
```
Expected: externals.dll in build/Debug/.

**Step 3: Commit**

```bash
git add build.c
git commit -m "implement build_externals target"
```

---

### Task 5: Implement build_core (DLL)

**Files:**
- Modify: `build.c`

**Step 1: Implement build_core**

```c
static int build_core(void) {
    char cmd[MAX_CMD];
    int i, any_rebuilt = 0;

    static const char *sources[] = {
        "src\\core\\core.cpp",
        "src\\core\\loadlibrary_windows.cpp"
    };
    static const char *objs[] = {
        OBJ_DIR "\\core\\core.obj",
        OBJ_DIR "\\core\\loadlibrary_windows.obj"
    };
    int n = sizeof(sources) / sizeof(sources[0]);

    ensure_dirs();

    for (i = 0; i < n; i++) {
        if (needs_rebuild(sources[i], objs[i])) {
            snprintf(cmd, MAX_CMD,
                "cl " CXXFLAGS " /c "
                "/DTRACY_ENABLE "
                COMMON_INCLUDES " "
                "/Fo\"%s\" \"%s\"",
                objs[i], sources[i]);
            if (run_cmd(cmd) != 0) return 1;
            any_rebuilt = 1;
        }
    }

    if (!any_rebuilt) {
        printf("core: up to date\n");
    }

    snprintf(cmd, MAX_CMD,
        "link /nologo /DLL /DEBUG "
        "/OUT:\"" BUILD_DIR "\\core.dll\" "
        "/IMPLIB:\"" BUILD_DIR "\\core.lib\" "
        "\"%s\" \"%s\" "
        "\"" BUILD_DIR "\\externals.lib\" "
        "\"" BUILD_DIR "\\TracyClient.lib\" "
        "opengl32.lib " GLFW_LIB " "
        "ws2_32.lib dbghelp.lib advapi32.lib user32.lib gdi32.lib shell32.lib",
        objs[0], objs[1]);
    if (run_cmd(cmd) != 0) return 1;

    printf("core: built core.dll\n");
    return 0;
}
```

**Step 2: Test**

```
cl build.c && build.exe core
```
Expected: core.dll in build/Debug/.

**Step 3: Commit**

```bash
git add build.c
git commit -m "implement build_core target"
```

---

### Task 6: Implement build_engine (DLL)

**Files:**
- Modify: `build.c`

**Step 1: Implement build_engine**

The engine DLL gets the random PDB treatment for hot-reload.

```c
static int build_engine(void) {
    char cmd[MAX_CMD];
    char pdb_suffix[9];
    int i, any_rebuilt = 0;

    static const char *sources[] = {
        "src\\engine\\engine.cpp",
        "src\\engine\\renderer.cpp",
        "src\\engine\\physics.cpp",
        "src\\engine\\scene.cpp",
        "src\\engine\\debug_render.cpp"
    };
    static const char *objs[] = {
        OBJ_DIR "\\engine\\engine.obj",
        OBJ_DIR "\\engine\\renderer.obj",
        OBJ_DIR "\\engine\\physics.obj",
        OBJ_DIR "\\engine\\scene.obj",
        OBJ_DIR "\\engine\\debug_render.obj"
    };
    int n = sizeof(sources) / sizeof(sources[0]);

    ensure_dirs();

    for (i = 0; i < n; i++) {
        if (needs_rebuild(sources[i], objs[i])) {
            snprintf(cmd, MAX_CMD,
                "cl " CXXFLAGS " /c "
                "/DGLFW_STATIC /DGLAD_GLAPI_IMPORT /DTRACY_ENABLE "
                COMMON_INCLUDES " "
                "/Fo\"%s\" \"%s\"",
                objs[i], sources[i]);
            if (run_cmd(cmd) != 0) return 1;
            any_rebuilt = 1;
        }
    }

    if (!any_rebuilt) {
        printf("engine: up to date\n");
    }

    /* Random PDB for hot-reload */
    rand_hex(pdb_suffix, 8);
    snprintf(cmd, MAX_CMD,
        "link /nologo /DLL /DEBUG "
        "/PDB:\"" BUILD_DIR "\\engine_%s.pdb\" "
        "/OUT:\"" BUILD_DIR "\\engine.dll\" "
        "/IMPLIB:\"" BUILD_DIR "\\engine.lib\" "
        "\"%s\" \"%s\" \"%s\" \"%s\" \"%s\" "
        "\"" BUILD_DIR "\\externals.lib\" "
        "\"" BUILD_DIR "\\glad_loader.lib\" "
        "\"" BUILD_DIR "\\TracyClient.lib\" "
        "opengl32.lib " GLFW_LIB " "
        "ws2_32.lib dbghelp.lib advapi32.lib user32.lib",
        pdb_suffix,
        objs[0], objs[1], objs[2], objs[3], objs[4]);
    if (run_cmd(cmd) != 0) return 1;

    printf("engine: built engine.dll\n");
    return 0;
}
```

**Step 2: Test**

```
cl build.c && build.exe engine
```
Expected: engine.dll in build/Debug/.

**Step 3: Commit**

```bash
git add build.c
git commit -m "implement build_engine target"
```

---

### Task 7: Implement build_exe and build_all and build_clean

**Files:**
- Modify: `build.c`

**Step 1: Implement build_exe**

```c
static int build_exe(void) {
    char cmd[MAX_CMD];
    int i, any_rebuilt = 0;

    static const char *sources[] = {
        "src\\main.cpp",
        "src\\core\\loadlibrary_windows.cpp"
    };
    static const char *objs[] = {
        OBJ_DIR "\\exe\\main.obj",
        OBJ_DIR "\\exe\\loadlibrary_windows.obj"
    };
    int n = sizeof(sources) / sizeof(sources[0]);

    ensure_dirs();

    for (i = 0; i < n; i++) {
        if (needs_rebuild(sources[i], objs[i])) {
            snprintf(cmd, MAX_CMD,
                "cl " CXXFLAGS " /c "
                "/DGLFW_STATIC "
                COMMON_INCLUDES " "
                "/Fo\"%s\" \"%s\"",
                objs[i], sources[i]);
            if (run_cmd(cmd) != 0) return 1;
            any_rebuilt = 1;
        }
    }

    if (!any_rebuilt) {
        printf("exe: up to date\n");
    }

    snprintf(cmd, MAX_CMD,
        "link /nologo /DEBUG "
        "/OUT:\"" BUILD_DIR "\\AnitraEngine.exe\" "
        "\"%s\" \"%s\" "
        "opengl32.lib " GLFW_LIB " "
        "gdi32.lib user32.lib shell32.lib",
        objs[0], objs[1]);
    if (run_cmd(cmd) != 0) return 1;

    printf("exe: built AnitraEngine.exe\n");
    return 0;
}
```

**Step 2: Implement build_all**

```c
static int build_all(void) {
    printf("=== Building all targets ===\n\n");
    if (build_tracy() != 0)    return 1;
    if (build_glad() != 0)     return 1;
    if (build_externals() != 0)return 1;
    if (build_core() != 0)     return 1;
    if (build_engine() != 0)   return 1;
    if (build_exe() != 0)      return 1;
    return 0;
}
```

**Step 3: Implement build_clean**

```c
static int build_clean(void) {
    printf("Cleaning build artifacts...\n");
    run_cmd("if exist build rmdir /s /q build");
    printf("Clean complete.\n");
    return 0;
}
```

**Step 4: Test full build**

```
cl build.c && build.exe clean && build.exe
```
Expected: All DLLs, libs, and AnitraEngine.exe in build/Debug/.

**Step 5: Test running the engine**

```
cd build\Debug
AnitraEngine.exe
```
Expected: Engine starts, loads core.dll, which hot-compiles and loads engine.dll.

**Step 6: Commit**

```bash
git add build.c
git commit -m "implement build_exe, build_all, build_clean targets"
```

---

### Task 8: Update core.cpp compile_dll to use build.exe

**Files:**
- Modify: `src/core/core.cpp:77-84`

**Step 1: Update compile_dll function**

The hot-reload file watcher calls `compile_dll()` which currently runs `build_engine.bat`. Update it to call `build.exe engine` instead.

Change:
```cpp
void compile_dll() {
  std::string cwd = getCurrentWorkingDirectory();
  std::string command =
      "cd /d " + cwd +
      " && build_engine.bat";
```
To:
```cpp
void compile_dll() {
  std::string cwd = getCurrentWorkingDirectory();
  std::string command =
      "cd /d " + cwd +
      " && build.exe engine";
```

**Step 2: Test hot-reload still works**

1. Build everything: `build.exe`
2. Run: `build\Debug\AnitraEngine.exe`
3. Modify a file in `src/engine/`
4. Verify the engine hot-reloads

**Step 3: Commit**

```bash
git add src/core/core.cpp
git commit -m "update compile_dll to use build.exe instead of build_engine.bat"
```

---

### Task 9: Delete CMake and old batch files

**Files:**
- Delete: `CMakeLists.txt`
- Delete: `build.bat`
- Delete: `build_engine.bat`
- Delete: `build_externals.bat`
- Delete: `generate.bat`

**Step 1: Verify build.exe works end-to-end first**

```
build.exe clean && build.exe
```
Must succeed before deleting anything.

**Step 2: Delete old build files**

```bash
git rm CMakeLists.txt build.bat build_engine.bat build_externals.bat generate.bat
```

**Step 3: Commit**

```bash
git commit -m "remove CMake and old batch build scripts"
```

---

### Task 10: Final verification

**Step 1: Clean build from scratch**

```
build.exe clean && build.exe
```

**Step 2: Run the engine**

```
cd build\Debug && AnitraEngine.exe
```

**Step 3: Test hot-reload**

Edit a file in `src/engine/`, verify auto-recompile and reload works.

**Step 4: Test incremental build**

```
build.exe engine
```
Should print "up to date" for unchanged files.
