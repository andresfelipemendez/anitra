/*
 * build.c - nobuild build system for the Anitra game engine
 *
 * Compile:  cl build.c
 * Usage:    build.exe [target]
 *
 * Targets:  all (default), tracy, glad, externals, core, engine, exe, clean
 *
 * This is a single-file C89 build system that replaces CMake.
 * It invokes cl.exe, link.exe, and lib.exe directly via system().
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>

/* ========================================================================= */
/* Constants                                                                  */
/* ========================================================================= */

#define CMD_MAX  8192
#define OBJ_MAX  4096

#define BUILD_DIR       "build"
#define DEBUG_DIR       "build\\Debug"
#define OBJ_DIR         "build\\obj"
#define OBJ_TRACY_DIR   "build\\obj\\tracy"
#define OBJ_GLAD_DIR    "build\\obj\\glad"
#define OBJ_EXT_DIR     "build\\obj\\externals"
#define OBJ_CORE_DIR    "build\\obj\\core"
#define OBJ_ENGINE_DIR  "build\\obj\\engine"
#define OBJ_EXE_DIR     "build\\obj\\exe"

/* Common include paths used by externals, core, engine, and exe targets */
#define COMMON_INCLUDES \
    "/Isrc /Isrc\\core /Isrc\\engine /Isrc\\externals /Iinclude " \
    "/Ilib\\imgui-1.90.9 /Ilib\\imgui-1.90.9\\backends " \
    "/Ilib\\glad /Ilib\\glfw\\include /Ilib\\tracy\\public"

/* MSVC tool paths (resolved at startup to avoid Git's link.exe shadowing) */
static char msvc_cl[MAX_PATH];
static char msvc_link[MAX_PATH];
static char msvc_lib[MAX_PATH];

/* ========================================================================= */
/* Utility functions                                                          */
/* ========================================================================= */

static int find_msvc_tools(void)
{
    /*
     * Find cl.exe in PATH, then derive link.exe and lib.exe from same dir.
     * If the found cl.exe is the x86 version, try to use the x64 version
     * instead (since our libraries are x64).
     */
    char cl_path[MAX_PATH];
    char x64_path[MAX_PATH];
    char *last_sep;
    char *arch_pos;
    DWORD len;

    len = SearchPathA(NULL, "cl.exe", NULL, MAX_PATH, cl_path, NULL);
    if (len == 0) {
        printf("!! cl.exe not found in PATH. Run from VS Developer Command Prompt.\n");
        return 1;
    }

    /*
     * If PATH found the x86 cl.exe (path contains \Hostx86\x86\ or
     * \Hostx64\x86\), try to use the x64 target instead by replacing
     * the last \x86\ with \x64\.
     */
    arch_pos = strstr(cl_path, "\\x86\\cl.exe");
    if (arch_pos) {
        /* Build the x64 path */
        strncpy(x64_path, cl_path, arch_pos - cl_path);
        x64_path[arch_pos - cl_path] = '\0';
        strcat(x64_path, "\\x64\\cl.exe");
        if (GetFileAttributesA(x64_path) != INVALID_FILE_ATTRIBUTES) {
            strcpy(cl_path, x64_path);
            printf("   Using x64 toolchain: %s\n", cl_path);
        }
    }

    /* Strip "cl.exe" to get the directory */
    last_sep = strrchr(cl_path, '\\');
    if (!last_sep) {
        printf("!! unexpected cl.exe path: %s\n", cl_path);
        return 1;
    }
    last_sep[1] = '\0'; /* keep trailing backslash */

    snprintf(msvc_cl, MAX_PATH, "%scl.exe", cl_path);
    snprintf(msvc_link, MAX_PATH, "%slink.exe", cl_path);
    snprintf(msvc_lib, MAX_PATH, "%slib.exe", cl_path);

    /*
     * If we switched to x64 tools, fix the LIB environment variable too.
     * The x86 environment has lib paths like ...\lib\x86 and ...\um\x86.
     * Replace \x86 path segments with \x64 so the linker finds the right libs.
     */
    {
        const char *old_lib = getenv("LIB");
        if (old_lib && strstr(cl_path, "\\x64\\")) {
            char new_lib[4096];
            const char *p = old_lib;
            char *out = new_lib;
            char *end = new_lib + sizeof(new_lib) - 1;

            while (*p && out < end) {
                if (strncmp(p, "\\x86", 4) == 0 &&
                    (p[4] == ';' || p[4] == '\\' || p[4] == '\0')) {
                    if (out + 4 < end) {
                        memcpy(out, "\\x64", 4);
                        out += 4;
                        p += 4;
                    } else break;
                } else {
                    *out++ = *p++;
                }
            }
            *out = '\0';
            {
                char env_str[4096 + 8];
                snprintf(env_str, sizeof(env_str), "LIB=%s", new_lib);
                _putenv(env_str);
            }
        }
    }

    return 0;
}

static int run_cmd(const char *cmd)
{
    int rc;
    printf(">> %s\n", cmd);
    fflush(stdout);
    rc = system(cmd);
    if (rc != 0) {
        printf("!! command failed with exit code %d\n", rc);
    }
    return rc;
}

static int needs_rebuild(const char *src, const char *obj)
{
    WIN32_FILE_ATTRIBUTE_DATA src_attr;
    WIN32_FILE_ATTRIBUTE_DATA obj_attr;

    if (!GetFileAttributesExA(obj, GetFileExInfoStandard, &obj_attr)) {
        /* obj does not exist => needs rebuild */
        return 1;
    }
    if (!GetFileAttributesExA(src, GetFileExInfoStandard, &src_attr)) {
        /* src does not exist => something is very wrong, but return 1 so
           the compiler can produce a proper error message */
        return 1;
    }
    /* rebuild if src is newer than obj */
    if (CompareFileTime(&src_attr.ftLastWriteTime, &obj_attr.ftLastWriteTime) > 0) {
        return 1;
    }
    return 0;
}

static int ensure_dir(const char *path)
{
    if (!CreateDirectoryA(path, NULL)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            printf("!! failed to create directory: %s (error %lu)\n",
                   path, (unsigned long)err);
            return 1;
        }
    }
    return 0;
}

static int ensure_dirs(void)
{
    if (ensure_dir(BUILD_DIR))      return 1;
    if (ensure_dir(DEBUG_DIR))      return 1;
    if (ensure_dir(OBJ_DIR))        return 1;
    if (ensure_dir(OBJ_TRACY_DIR))  return 1;
    if (ensure_dir(OBJ_GLAD_DIR))   return 1;
    if (ensure_dir(OBJ_EXT_DIR))    return 1;
    if (ensure_dir(OBJ_CORE_DIR))   return 1;
    if (ensure_dir(OBJ_ENGINE_DIR)) return 1;
    if (ensure_dir(OBJ_EXE_DIR))    return 1;
    return 0;
}

static void rand_hex(char *buf, int len)
{
    static const char hex[] = "0123456789ABCDEF";
    static int seeded = 0;
    int i;

    if (!seeded) {
        srand((unsigned int)time(NULL) ^ (unsigned int)GetTickCount());
        seeded = 1;
    }
    for (i = 0; i < len; i++) {
        buf[i] = hex[rand() % 16];
    }
    buf[len] = '\0';
}

/* ========================================================================= */
/* Build targets                                                              */
/* ========================================================================= */

/* ------- tracy (static library) ----------------------------------------- */
static int build_tracy(void)
{
    char cmd[CMD_MAX];
    int any_rebuilt = 0;

    printf("\n=== Building tracy ===\n");
    if (ensure_dirs() != 0) return 1;

    if (needs_rebuild("lib\\tracy\\public\\TracyClient.cpp",
                      OBJ_TRACY_DIR "\\TracyClient.obj")) {
        snprintf(cmd, sizeof(cmd),
            "\"%s\" /std:c++20 /EHsc /MD /Zi /Od /nologo /c "
            "/DTRACY_ENABLE /DTRACY_ON_DEMAND "
            "/Ilib\\tracy\\public "
            "/Fo" OBJ_TRACY_DIR "\\TracyClient.obj "
            "/Fd" OBJ_TRACY_DIR "\\TracyClient.pdb "
            "lib\\tracy\\public\\TracyClient.cpp",
            msvc_cl);
        if (run_cmd(cmd) != 0) return 1;
        any_rebuilt = 1;
    }

    if (any_rebuilt ||
        needs_rebuild(OBJ_TRACY_DIR "\\TracyClient.obj",
                      DEBUG_DIR "\\TracyClient.lib")) {
        snprintf(cmd, sizeof(cmd),
            "\"%s\" /nologo /OUT:" DEBUG_DIR "\\TracyClient.lib "
            OBJ_TRACY_DIR "\\TracyClient.obj",
            msvc_lib);
        if (run_cmd(cmd) != 0) return 1;
    } else {
        printf("   tracy is up to date.\n");
    }

    return 0;
}

/* ------- glad (DLL) ----------------------------------------------------- */
static int build_glad(void)
{
    char cmd[CMD_MAX];
    int any_rebuilt = 0;

    printf("\n=== Building glad_loader ===\n");
    if (ensure_dirs() != 0) return 1;

    if (needs_rebuild("lib\\glad\\glad.c",
                      OBJ_GLAD_DIR "\\glad.obj")) {
        snprintf(cmd, sizeof(cmd),
            "\"%s\" /MD /Zi /Od /nologo /c "
            "/DGLAD_GLAPI_EXPORT /DGLAD_GLAPI_EXPORT_BUILD "
            "/Ilib\\glad "
            "/Fo" OBJ_GLAD_DIR "\\glad.obj "
            "/Fd" OBJ_GLAD_DIR "\\glad.pdb "
            "lib\\glad\\glad.c",
            msvc_cl);
        if (run_cmd(cmd) != 0) return 1;
        any_rebuilt = 1;
    }

    if (any_rebuilt ||
        needs_rebuild(OBJ_GLAD_DIR "\\glad.obj",
                      DEBUG_DIR "\\glad_loader.dll")) {
        snprintf(cmd, sizeof(cmd),
            "\"%s\" /nologo /DLL /DEBUG "
            "/OUT:" DEBUG_DIR "\\glad_loader.dll "
            "/IMPLIB:" DEBUG_DIR "\\glad_loader.lib "
            OBJ_GLAD_DIR "\\glad.obj "
            "opengl32.lib lib\\glfw\\lib\\glfw3.lib",
            msvc_link);
        if (run_cmd(cmd) != 0) return 1;
    } else {
        printf("   glad_loader is up to date.\n");
    }

    return 0;
}

/* ------- externals (DLL) ------------------------------------------------ */
static int build_externals(void)
{
    static const char *sources[] = {
        "src\\externals\\externals.cpp",
        "lib\\imgui-1.90.9\\imgui.cpp",
        "lib\\imgui-1.90.9\\imgui_demo.cpp",
        "lib\\imgui-1.90.9\\imgui_draw.cpp",
        "lib\\imgui-1.90.9\\imgui_tables.cpp",
        "lib\\imgui-1.90.9\\imgui_widgets.cpp",
        "lib\\imgui-1.90.9\\backends\\imgui_impl_glfw.cpp",
        "lib\\imgui-1.90.9\\backends\\imgui_impl_opengl3.cpp"
    };
    static const char *objs[] = {
        OBJ_EXT_DIR "\\externals.obj",
        OBJ_EXT_DIR "\\imgui.obj",
        OBJ_EXT_DIR "\\imgui_demo.obj",
        OBJ_EXT_DIR "\\imgui_draw.obj",
        OBJ_EXT_DIR "\\imgui_tables.obj",
        OBJ_EXT_DIR "\\imgui_widgets.obj",
        OBJ_EXT_DIR "\\imgui_impl_glfw.obj",
        OBJ_EXT_DIR "\\imgui_impl_opengl3.obj"
    };
    int count = sizeof(sources) / sizeof(sources[0]);
    int i;
    int any_rebuilt = 0;
    char cmd[CMD_MAX];
    char obj_list[OBJ_MAX];
    char pdb_suffix[9];
    int pos;

    printf("\n=== Building externals ===\n");
    if (ensure_dirs() != 0) return 1;

    for (i = 0; i < count; i++) {
        if (needs_rebuild(sources[i], objs[i])) {
            snprintf(cmd, sizeof(cmd),
                "\"%s\" /std:c++20 /EHsc /MD /Zi /Od /nologo /c "
                "/DIMGUI_IMPL_OPENGL_LOADER_GLAD /DGLAD_GLAPI_EXPORT /DTRACY_ENABLE "
                COMMON_INCLUDES " "
                "/Fo%s "
                "/Fd" OBJ_EXT_DIR "\\externals.pdb "
                "%s",
                msvc_cl, objs[i], sources[i]);
            if (run_cmd(cmd) != 0) return 1;
            any_rebuilt = 1;
        }
    }

    if (any_rebuilt ||
        needs_rebuild(objs[0], DEBUG_DIR "\\externals.dll")) {
        /* Build object list string */
        pos = 0;
        for (i = 0; i < count; i++) {
            pos += snprintf(obj_list + pos, sizeof(obj_list) - pos,
                            "%s ", objs[i]);
            if (pos >= (int)sizeof(obj_list)) {
                printf("!! object list too long\n");
                return 1;
            }
        }

        rand_hex(pdb_suffix, 8);

        snprintf(cmd, sizeof(cmd),
            "\"%s\" /nologo /DLL /DEBUG "
            "/PDB:" DEBUG_DIR "\\externals_%s.pdb "
            "/OUT:" DEBUG_DIR "\\externals.dll "
            "/IMPLIB:" DEBUG_DIR "\\externals.lib "
            "%s"
            DEBUG_DIR "\\glad_loader.lib "
            DEBUG_DIR "\\TracyClient.lib "
            "opengl32.lib lib\\glfw\\lib\\glfw3.lib "
            "gdi32.lib shell32.lib "
            "ws2_32.lib dbghelp.lib advapi32.lib user32.lib",
            msvc_link, pdb_suffix, obj_list);
        if (run_cmd(cmd) != 0) return 1;
    } else {
        printf("   externals is up to date.\n");
    }

    return 0;
}

/* ------- core (DLL) ----------------------------------------------------- */
static int build_core(void)
{
    static const char *sources[] = {
        "src\\core\\core.cpp",
        "src\\core\\loadlibrary_windows.cpp"
    };
    static const char *objs[] = {
        OBJ_CORE_DIR "\\core.obj",
        OBJ_CORE_DIR "\\loadlibrary_windows.obj"
    };
    int count = sizeof(sources) / sizeof(sources[0]);
    int i;
    int any_rebuilt = 0;
    char cmd[CMD_MAX];
    char obj_list[OBJ_MAX];
    int pos;

    printf("\n=== Building core ===\n");
    if (ensure_dirs() != 0) return 1;

    for (i = 0; i < count; i++) {
        if (needs_rebuild(sources[i], objs[i])) {
            snprintf(cmd, sizeof(cmd),
                "\"%s\" /std:c++20 /EHsc /MD /Zi /Od /nologo /c "
                "/DTRACY_ENABLE "
                COMMON_INCLUDES " "
                "/Fo%s "
                "/Fd" OBJ_CORE_DIR "\\core.pdb "
                "%s",
                msvc_cl, objs[i], sources[i]);
            if (run_cmd(cmd) != 0) return 1;
            any_rebuilt = 1;
        }
    }

    if (any_rebuilt ||
        needs_rebuild(objs[0], DEBUG_DIR "\\core.dll")) {
        pos = 0;
        for (i = 0; i < count; i++) {
            pos += snprintf(obj_list + pos, sizeof(obj_list) - pos,
                            "%s ", objs[i]);
            if (pos >= (int)sizeof(obj_list)) {
                printf("!! object list too long\n");
                return 1;
            }
        }

        snprintf(cmd, sizeof(cmd),
            "\"%s\" /nologo /DLL /DEBUG "
            "/OUT:" DEBUG_DIR "\\core.dll "
            "/IMPLIB:" DEBUG_DIR "\\core.lib "
            "%s"
            DEBUG_DIR "\\externals.lib "
            DEBUG_DIR "\\TracyClient.lib "
            "opengl32.lib lib\\glfw\\lib\\glfw3.lib "
            "ws2_32.lib dbghelp.lib advapi32.lib user32.lib gdi32.lib shell32.lib",
            msvc_link, obj_list);
        if (run_cmd(cmd) != 0) return 1;
    } else {
        printf("   core is up to date.\n");
    }

    return 0;
}

/* ------- engine (DLL) --------------------------------------------------- */
static int build_engine(void)
{
    static const char *sources[] = {
        "src\\engine\\engine.cpp",
        "src\\engine\\renderer.cpp",
        "src\\engine\\physics.cpp",
        "src\\engine\\scene.cpp",
        "src\\engine\\debug_render.cpp"
    };
    static const char *objs[] = {
        OBJ_ENGINE_DIR "\\engine.obj",
        OBJ_ENGINE_DIR "\\renderer.obj",
        OBJ_ENGINE_DIR "\\physics.obj",
        OBJ_ENGINE_DIR "\\scene.obj",
        OBJ_ENGINE_DIR "\\debug_render.obj"
    };
    int count = sizeof(sources) / sizeof(sources[0]);
    int i;
    int any_rebuilt = 0;
    char cmd[CMD_MAX];
    char obj_list[OBJ_MAX];
    char pdb_suffix[9];
    int pos;

    printf("\n=== Building engine ===\n");
    if (ensure_dirs() != 0) return 1;

    for (i = 0; i < count; i++) {
        if (needs_rebuild(sources[i], objs[i])) {
            snprintf(cmd, sizeof(cmd),
                "\"%s\" /std:c++20 /EHsc /MD /Zi /Od /nologo /c "
                "/DGLFW_STATIC /DGLAD_GLAPI_EXPORT /DTRACY_ENABLE "
                COMMON_INCLUDES " "
                "/Fo%s "
                "/Fd" OBJ_ENGINE_DIR "\\engine.pdb "
                "%s",
                msvc_cl, objs[i], sources[i]);
            if (run_cmd(cmd) != 0) return 1;
            any_rebuilt = 1;
        }
    }

    if (any_rebuilt ||
        needs_rebuild(objs[0], DEBUG_DIR "\\engine.dll")) {
        pos = 0;
        for (i = 0; i < count; i++) {
            pos += snprintf(obj_list + pos, sizeof(obj_list) - pos,
                            "%s ", objs[i]);
            if (pos >= (int)sizeof(obj_list)) {
                printf("!! object list too long\n");
                return 1;
            }
        }

        rand_hex(pdb_suffix, 8);

        snprintf(cmd, sizeof(cmd),
            "\"%s\" /nologo /DLL /DEBUG "
            "/PDB:" DEBUG_DIR "\\engine_%s.pdb "
            "/OUT:" DEBUG_DIR "\\engine.dll "
            "/IMPLIB:" DEBUG_DIR "\\engine.lib "
            "%s"
            DEBUG_DIR "\\externals.lib "
            DEBUG_DIR "\\glad_loader.lib "
            DEBUG_DIR "\\TracyClient.lib "
            "opengl32.lib lib\\glfw\\lib\\glfw3.lib "
            "gdi32.lib shell32.lib "
            "ws2_32.lib dbghelp.lib advapi32.lib user32.lib",
            msvc_link, pdb_suffix, obj_list);
        if (run_cmd(cmd) != 0) return 1;
    } else {
        printf("   engine is up to date.\n");
    }

    return 0;
}

/* ------- exe ------------------------------------------------------------ */
static int build_exe(void)
{
    static const char *sources[] = {
        "src\\main.cpp",
        "src\\core\\loadlibrary_windows.cpp"
    };
    static const char *objs[] = {
        OBJ_EXE_DIR "\\main.obj",
        OBJ_EXE_DIR "\\loadlibrary_windows.obj"
    };
    int count = sizeof(sources) / sizeof(sources[0]);
    int i;
    int any_rebuilt = 0;
    char cmd[CMD_MAX];
    char obj_list[OBJ_MAX];
    int pos;

    printf("\n=== Building exe ===\n");
    if (ensure_dirs() != 0) return 1;

    for (i = 0; i < count; i++) {
        if (needs_rebuild(sources[i], objs[i])) {
            snprintf(cmd, sizeof(cmd),
                "\"%s\" /std:c++20 /EHsc /MD /Zi /Od /nologo /c "
                "/DGLFW_STATIC "
                COMMON_INCLUDES " "
                "/Fo%s "
                "/Fd" OBJ_EXE_DIR "\\exe.pdb "
                "%s",
                msvc_cl, objs[i], sources[i]);
            if (run_cmd(cmd) != 0) return 1;
            any_rebuilt = 1;
        }
    }

    if (any_rebuilt ||
        needs_rebuild(objs[0], DEBUG_DIR "\\AnitraEngine.exe")) {
        pos = 0;
        for (i = 0; i < count; i++) {
            pos += snprintf(obj_list + pos, sizeof(obj_list) - pos,
                            "%s ", objs[i]);
            if (pos >= (int)sizeof(obj_list)) {
                printf("!! object list too long\n");
                return 1;
            }
        }

        snprintf(cmd, sizeof(cmd),
            "\"%s\" /nologo /DEBUG "
            "/OUT:" DEBUG_DIR "\\AnitraEngine.exe "
            "%s"
            "opengl32.lib lib\\glfw\\lib\\glfw3.lib "
            "gdi32.lib user32.lib shell32.lib",
            msvc_link, obj_list);
        if (run_cmd(cmd) != 0) return 1;
    } else {
        printf("   exe is up to date.\n");
    }

    return 0;
}

/* ------- all ------------------------------------------------------------ */
static int build_all(void)
{
    printf("=== Building all targets ===\n\n");

    if (build_tracy() != 0) return 1;
    if (build_glad() != 0) return 1;
    if (build_externals() != 0) return 1;
    if (build_core() != 0) return 1;
    if (build_engine() != 0) return 1;
    if (build_exe() != 0) return 1;

    printf("\n=== All targets built successfully. ===\n");
    return 0;
}

/* ------- clean ---------------------------------------------------------- */
static int build_clean(void)
{
    printf("=== Cleaning build directory ===\n");
    return run_cmd("if exist build rmdir /s /q build");
}

/* ========================================================================= */
/* main                                                                       */
/* ========================================================================= */

static void print_usage(void)
{
    printf("Usage: build.exe [target]\n");
    printf("\n");
    printf("Targets:\n");
    printf("  all        Build everything (default)\n");
    printf("  tracy      Build TracyClient static library\n");
    printf("  glad       Build glad_loader DLL\n");
    printf("  externals  Build externals DLL (ImGui + backends)\n");
    printf("  core       Build core DLL\n");
    printf("  engine     Build engine DLL\n");
    printf("  exe        Build AnitraEngine executable\n");
    printf("  clean      Delete build directory\n");
    printf("  help       Show this message\n");
}

int main(int argc, char *argv[])
{
    const char *target;
    int rc;

    if (find_msvc_tools() != 0) return 1;

    if (argc < 2) {
        target = "all";
    } else {
        target = argv[1];
    }

    if (strcmp(target, "all") == 0) {
        rc = build_all();
    } else if (strcmp(target, "tracy") == 0) {
        rc = build_tracy();
    } else if (strcmp(target, "glad") == 0) {
        rc = build_glad();
    } else if (strcmp(target, "externals") == 0) {
        rc = build_externals();
    } else if (strcmp(target, "core") == 0) {
        rc = build_core();
    } else if (strcmp(target, "engine") == 0) {
        rc = build_engine();
    } else if (strcmp(target, "exe") == 0) {
        rc = build_exe();
    } else if (strcmp(target, "clean") == 0) {
        rc = build_clean();
    } else if (strcmp(target, "help") == 0 || strcmp(target, "-h") == 0 ||
               strcmp(target, "--help") == 0 || strcmp(target, "/?") == 0) {
        print_usage();
        rc = 0;
    } else {
        printf("Unknown target: %s\n\n", target);
        print_usage();
        rc = 1;
    }

    return rc;
}
