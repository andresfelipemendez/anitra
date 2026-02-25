/*
 * build.c - nobuild build system for the Anitra game engine
 *
 * Compile:  cl build.c                                  (Windows)
 *           ./tcc -Blib/tcc-linux -o build build.c      (Linux)
 * Usage:    build [target]
 *
 * Targets:  all (default), tracy, sdl3, spirvcross, shadercross, shaders, externals, core, engine, exe, watch, clean
 *
 * This is a single-file C89 build system that replaces CMake.
 * It invokes the platform's native toolchain directly via system().
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <sys/inotify.h>
#include <poll.h>
#endif

/* ========================================================================= */
/* Constants                                                                  */
/* ========================================================================= */

#define CMD_MAX  8192
#define OBJ_MAX  4096

#ifdef _WIN32
#define PATH_SIZE MAX_PATH
#else
#define PATH_SIZE 4096
#endif

#ifdef _WIN32
#define OBJ_EXT  ".obj"
#define LIB_EXT  ".lib"
#define DLL_EXT  ".dll"
#define DLL_PREFIX ""
#define EXE_EXT  ".exe"
#else
#define OBJ_EXT  ".o"
#define LIB_EXT  ".a"
#define DLL_EXT  ".so"
#define DLL_PREFIX "lib"
#define EXE_EXT  ""
#endif

#define BUILD_DIR       "build"
#define DEBUG_DIR       "build/Debug"
#define OBJ_DIR         "build/obj"
#define OBJ_TRACY_DIR   "build/obj/tracy"
#define OBJ_EXT_DIR     "build/obj/externals"
#define OBJ_CORE_DIR    "build/obj/core"
#define OBJ_ENGINE_DIR  "build/obj/engine"
#define OBJ_EXE_DIR     "build/obj/exe"
#define OBJ_SDL3_DIR        "build/obj/sdl3"
#define OBJ_SPIRVCROSS_DIR  "build/obj/spirvcross"
#define OBJ_SHADERCROSS_DIR "build/obj/shadercross"

/* Common include paths used by externals, core, engine, and exe targets */
#ifdef _WIN32
#define COMMON_INCLUDES \
    "/Isrc /Isrc/core /Isrc/engine /Isrc/externals /Iinclude " \
    "/Ilib/SDL3/include /Ilib/SDL_shadercross/include " \
    "/Ilib/SDL_shadercross/external/SPIRV-Cross " \
    "/Ilib/SDL_shadercross/external/prebuilt/inc " \
    "/Ilib/tracy/public " \
    "/Ilib/harfbuzz-src/src " \
    "/Ilib/clay"
#else
#define COMMON_INCLUDES \
    "-Isrc -Isrc/core -Isrc/engine -Isrc/externals -Iinclude " \
    "-Ilib/SDL3/include -Ilib/SDL_shadercross/include " \
    "-Ilib/SDL_shadercross/external/SPIRV-Cross " \
    "-Ilib/SDL_shadercross/external/prebuilt/inc " \
    "-Ilib/tracy/public " \
    "-Ilib/harfbuzz-src/src " \
    "-Ilib/clay"
#endif

/* Tool paths */
#ifdef _WIN32
static char tool_cc[PATH_SIZE];
static char tool_cxx[PATH_SIZE];
static char tool_link[PATH_SIZE];
static char tool_ar[PATH_SIZE];
#else
static const char *tool_cc  = "cc";
static const char *tool_cxx = "c++";
static const char *tool_link = "cc";
static const char *tool_ar  = "ar";
#endif

/* ========================================================================= */
/* Utility functions                                                          */
/* ========================================================================= */

#ifdef _WIN32
static int find_tools(void)
{
    /*
     * Strategy:
     *  1. Try PATH first (fast path when running from Dev Command Prompt).
     *  2. If cl.exe not in PATH, use vswhere to find VS, then run vcvarsall
     *     to set up the full environment (INCLUDE, LIB, PATH, etc.).
     */
    char cl_path[PATH_SIZE];
    char x64_path[PATH_SIZE];
    char *last_sep;
    char *arch_pos;
    DWORD len;

    len = SearchPathA(NULL, "cl.exe", NULL, PATH_SIZE, cl_path, NULL);

    if (len == 0) {
        /* cl.exe not in PATH -- try to set up MSVC environment automatically */
        char cmd[CMD_MAX];
        FILE *fp;
        char vs_path[PATH_SIZE];
        char vcvarsall[PATH_SIZE];

        /* Use vswhere to find VS installation */
        fp = _popen(
            "\"C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe\" "
            "-latest -property installationPath 2>nul", "r");
        if (!fp) {
            printf("!! cl.exe not found and vswhere.exe not available.\n");
            printf("!! Install Visual Studio or run from VS Developer Command Prompt.\n");
            return 1;
        }
        if (!fgets(vs_path, PATH_SIZE, fp)) {
            _pclose(fp);
            printf("!! vswhere.exe found no Visual Studio installation.\n");
            return 1;
        }
        _pclose(fp);

        /* Strip trailing newline */
        {
            size_t slen = strlen(vs_path);
            while (slen > 0 && (vs_path[slen-1] == '\n' || vs_path[slen-1] == '\r'))
                vs_path[--slen] = '\0';
        }

        snprintf(vcvarsall, PATH_SIZE, "%s\\VC\\Auxiliary\\Build\\vcvarsall.bat", vs_path);
        if (GetFileAttributesA(vcvarsall) == INVALID_FILE_ATTRIBUTES) {
            printf("!! vcvarsall.bat not found at: %s\n", vcvarsall);
            return 1;
        }

        /*
         * Run vcvarsall, dump the resulting environment into a temp file,
         * then parse it to set our process environment.
         */
        {
            char tmp_file[PATH_SIZE];
            char line[8192];
            FILE *env_fp;

            GetTempPathA(PATH_SIZE, tmp_file);
            strcat(tmp_file, "anitra_env.txt");

            snprintf(cmd, sizeof(cmd),
                "cmd /C \"call \"%s\" x64 >nul 2>&1 && set > \"%s\"\"",
                vcvarsall, tmp_file);
            if (system(cmd) != 0) {
                printf("!! vcvarsall.bat failed.\n");
                return 1;
            }

            env_fp = fopen(tmp_file, "r");
            if (!env_fp) {
                printf("!! failed to read environment from vcvarsall.\n");
                return 1;
            }

            while (fgets(line, sizeof(line), env_fp)) {
                /* Strip trailing newline */
                size_t slen = strlen(line);
                while (slen > 0 && (line[slen-1] == '\n' || line[slen-1] == '\r'))
                    line[--slen] = '\0';
                /* Only import vars that matter for compilation */
                if (strncmp(line, "PATH=", 5) == 0 ||
                    strncmp(line, "Path=", 5) == 0 ||
                    strncmp(line, "LIB=", 4) == 0 ||
                    strncmp(line, "LIBPATH=", 8) == 0 ||
                    strncmp(line, "INCLUDE=", 8) == 0 ||
                    strncmp(line, "WindowsSdkDir=", 14) == 0 ||
                    strncmp(line, "WindowsSDKVersion=", 18) == 0 ||
                    strncmp(line, "UCRTVersion=", 12) == 0 ||
                    strncmp(line, "VCToolsInstallDir=", 18) == 0) {
                    _putenv(line);
                }
            }
            fclose(env_fp);
            DeleteFileA(tmp_file);
        }

        printf("   MSVC environment set up via vcvarsall.bat\n");

        /* Now cl.exe should be in PATH */
        len = SearchPathA(NULL, "cl.exe", NULL, PATH_SIZE, cl_path, NULL);
        if (len == 0) {
            printf("!! cl.exe still not found after running vcvarsall.\n");
            return 1;
        }
    }

    /*
     * If PATH found the x86 cl.exe (path contains \Hostx86\x86\ or
     * \Hostx64\x86\), try to use the x64 target instead by replacing
     * the last \x86\ with \x64\.
     */
    arch_pos = strstr(cl_path, "\\x86\\cl.exe");
    if (arch_pos) {
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
    last_sep[1] = '\0';

    snprintf(tool_cc,   PATH_SIZE, "%scl.exe", cl_path);
    snprintf(tool_cxx,  PATH_SIZE, "%scl.exe", cl_path);
    snprintf(tool_link, PATH_SIZE, "%slink.exe", cl_path);
    snprintf(tool_ar,   PATH_SIZE, "%slib.exe", cl_path);

    return 0;
}
#else
static int find_tools(void)
{
    /* On Linux, just verify that cc exists */
    if (system("command -v cc >/dev/null 2>&1") != 0) {
        printf("!! cc not found. Install a C compiler (gcc or clang).\n");
        return 1;
    }
    return 0;
}
#endif

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

static int force_rebuild = 0;

static int needs_rebuild(const char *src, const char *obj)
{
    if (force_rebuild) return 1;
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA src_attr;
    WIN32_FILE_ATTRIBUTE_DATA obj_attr;

    if (!GetFileAttributesExA(obj, GetFileExInfoStandard, &obj_attr)) {
        return 1;
    }
    if (!GetFileAttributesExA(src, GetFileExInfoStandard, &src_attr)) {
        return 1;
    }
    if (CompareFileTime(&src_attr.ftLastWriteTime, &obj_attr.ftLastWriteTime) > 0) {
        return 1;
    }
    return 0;
#else
    struct stat src_st, obj_st;

    if (stat(obj, &obj_st) != 0) {
        return 1;
    }
    if (stat(src, &src_st) != 0) {
        return 1;
    }
    if (src_st.st_mtime > obj_st.st_mtime) {
        return 1;
    }
    return 0;
#endif
}

static int ensure_dir(const char *path)
{
#ifdef _WIN32
    if (!CreateDirectoryA(path, NULL)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            printf("!! failed to create directory: %s (error %lu)\n",
                   path, (unsigned long)err);
            return 1;
        }
    }
#else
    if (mkdir(path, 0755) != 0) {
        if (errno != EEXIST) {
            printf("!! failed to create directory: %s (%s)\n",
                   path, strerror(errno));
            return 1;
        }
    }
#endif
    return 0;
}

static int ensure_dirs(void)
{
    if (ensure_dir(BUILD_DIR))      return 1;
    if (ensure_dir(DEBUG_DIR))      return 1;
    if (ensure_dir(OBJ_DIR))        return 1;
    if (ensure_dir(OBJ_TRACY_DIR))  return 1;
    if (ensure_dir(OBJ_EXT_DIR))    return 1;
    if (ensure_dir(OBJ_CORE_DIR))   return 1;
    if (ensure_dir(OBJ_ENGINE_DIR)) return 1;
    if (ensure_dir(OBJ_EXE_DIR))    return 1;
    if (ensure_dir(OBJ_SDL3_DIR))       return 1;
    if (ensure_dir(OBJ_SPIRVCROSS_DIR)) return 1;
    if (ensure_dir(OBJ_SHADERCROSS_DIR)) return 1;
    return 0;
}

static void rand_hex(char *buf, int len)
{
    static const char hex[] = "0123456789ABCDEF";
    static int seeded = 0;
    int i;

    if (!seeded) {
#ifdef _WIN32
        srand((unsigned int)time(NULL) ^ (unsigned int)GetTickCount());
#else
        srand((unsigned int)time(NULL) ^ (unsigned int)getpid());
#endif
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

    if (needs_rebuild("lib/tracy/public/TracyClient.cpp",
                      OBJ_TRACY_DIR "/TracyClient" OBJ_EXT)) {
#ifdef _WIN32
        snprintf(cmd, sizeof(cmd),
            "\"%s\" /std:c++20 /EHsc /MD /Zi /Od /nologo /c "
            "/DTRACY_ENABLE /DTRACY_ON_DEMAND "
            "/Ilib/tracy/public "
            "/Fo" OBJ_TRACY_DIR "/TracyClient" OBJ_EXT " "
            "/Fd" OBJ_TRACY_DIR "/TracyClient.pdb "
            "lib/tracy/public/TracyClient.cpp",
            tool_cxx);
#else
        snprintf(cmd, sizeof(cmd),
            "%s -std=c++20 -fPIC -g -O0 -c "
            "-DTRACY_ENABLE -DTRACY_ON_DEMAND "
            "-Ilib/tracy/public "
            "-o " OBJ_TRACY_DIR "/TracyClient" OBJ_EXT " "
            "lib/tracy/public/TracyClient.cpp",
            tool_cxx);
#endif
        if (run_cmd(cmd) != 0) return 1;
        any_rebuilt = 1;
    }

    if (any_rebuilt ||
        needs_rebuild(OBJ_TRACY_DIR "/TracyClient" OBJ_EXT,
                      DEBUG_DIR "/TracyClient" LIB_EXT)) {
#ifdef _WIN32
        snprintf(cmd, sizeof(cmd),
            "\"%s\" /nologo /OUT:" DEBUG_DIR "/TracyClient" LIB_EXT " "
            OBJ_TRACY_DIR "/TracyClient" OBJ_EXT,
            tool_ar);
#else
        snprintf(cmd, sizeof(cmd),
            "%s rcs " DEBUG_DIR "/TracyClient" LIB_EXT " "
            OBJ_TRACY_DIR "/TracyClient" OBJ_EXT,
            tool_ar);
#endif
        if (run_cmd(cmd) != 0) return 1;
    } else {
        printf("   tracy is up to date.\n");
    }

    return 0;
}

/* ------- externals (DLL) ------------------------------------------------ */
static int build_externals(void)
{
    static const char *sources[] = {
        "src/externals/externals.cpp"
    };
    static const char *objs[] = {
        OBJ_EXT_DIR "/externals" OBJ_EXT
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

    /* Compile HarfBuzz amalgamated (C++) */
    if (needs_rebuild("lib/harfbuzz-src/src/harfbuzz.cc",
                       OBJ_EXT_DIR "/harfbuzz" OBJ_EXT)) {
#ifdef _WIN32
        snprintf(cmd, sizeof(cmd),
            "\"%s\" /std:c++17 /EHsc /MD /Zi /Od /nologo /c "
            "/DHAVE_DIRECTWRITE "
            "/Ilib/harfbuzz-src/src "
            "/Fo" OBJ_EXT_DIR "/harfbuzz" OBJ_EXT " "
            "/Fd" OBJ_EXT_DIR "/harfbuzz.pdb "
            "lib/harfbuzz-src/src/harfbuzz.cc",
            tool_cxx);
#else
        snprintf(cmd, sizeof(cmd),
            "%s -std=c++17 -fPIC -g -O0 -c "
            "-DHB_NO_DIRECTWRITE "
            "-Ilib/harfbuzz-src/src "
            "-o " OBJ_EXT_DIR "/harfbuzz" OBJ_EXT " "
            "lib/harfbuzz-src/src/harfbuzz.cc",
            tool_cxx);
#endif
        if (run_cmd(cmd) != 0) return 1;
        any_rebuilt = 1;
    }

    for (i = 0; i < count; i++) {
        if (needs_rebuild(sources[i], objs[i])) {
#ifdef _WIN32
            snprintf(cmd, sizeof(cmd),
                "\"%s\" /std:c++20 /EHsc /MD /Zi /Od /nologo /c "
                "/DTRACY_ENABLE "
                COMMON_INCLUDES " "
                "/Fo%s "
                "/Fd" OBJ_EXT_DIR "/externals.pdb "
                "%s",
                tool_cxx, objs[i], sources[i]);
#else
            snprintf(cmd, sizeof(cmd),
                "%s -std=c++20 -fPIC -g -O0 -Wno-changes-meaning -c "
                "-DTRACY_ENABLE "
                COMMON_INCLUDES " "
                "-o %s "
                "%s",
                tool_cxx, objs[i], sources[i]);
#endif
            if (run_cmd(cmd) != 0) return 1;
            any_rebuilt = 1;
        }
    }

    if (any_rebuilt ||
        needs_rebuild(objs[0], DEBUG_DIR "/" DLL_PREFIX "externals" DLL_EXT)) {
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
        pos += snprintf(obj_list + pos, sizeof(obj_list) - pos,
                        OBJ_EXT_DIR "/harfbuzz" OBJ_EXT " ");

#ifdef _WIN32
        rand_hex(pdb_suffix, 8);

        snprintf(cmd, sizeof(cmd),
            "\"%s\" /nologo /DLL /DEBUG "
            "/PDB:" DEBUG_DIR "/externals_%s.pdb "
            "/OUT:" DEBUG_DIR "/" DLL_PREFIX "externals" DLL_EXT " "
            "/IMPLIB:" DEBUG_DIR "/externals" LIB_EXT " "
            "%s"
            DEBUG_DIR "/SDL3" LIB_EXT " "
            DEBUG_DIR "/SDL3_shadercross" LIB_EXT " "
            DEBUG_DIR "/TracyClient" LIB_EXT " "
            "ws2_32.lib dbghelp.lib advapi32.lib user32.lib dwrite.lib",
            tool_link, pdb_suffix, obj_list);
#else
        (void)pdb_suffix;
        snprintf(cmd, sizeof(cmd),
            "%s -shared -o " DEBUG_DIR "/" DLL_PREFIX "externals" DLL_EXT " "
            "%s"
            DEBUG_DIR "/TracyClient" LIB_EXT " "
            "-L" DEBUG_DIR " -lSDL3 -lSDL3_shadercross "
            "-lpthread -ldl -lm",
            tool_link, obj_list);
#endif
        if (run_cmd(cmd) != 0) return 1;
    } else {
        printf("   externals is up to date.\n");
    }

    return 0;
}

/* ------- core (DLL) ----------------------------------------------------- */
static int build_core(void)
{
#ifdef _WIN32
    static const char *sources[] = {
        "src/core/core.cpp",
        "src/core/loadlibrary_windows.cpp"
    };
    static const char *objs[] = {
        OBJ_CORE_DIR "/core" OBJ_EXT,
        OBJ_CORE_DIR "/loadlibrary_windows" OBJ_EXT
    };
#else
    static const char *sources[] = {
        "src/core/core.cpp",
        "src/core/loadlibrary_linux.cpp"
    };
    static const char *objs[] = {
        OBJ_CORE_DIR "/core" OBJ_EXT,
        OBJ_CORE_DIR "/loadlibrary_linux" OBJ_EXT
    };
#endif
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
#ifdef _WIN32
            snprintf(cmd, sizeof(cmd),
                "\"%s\" /std:c++20 /EHsc /MD /Zi /Od /nologo /c "
                "/DTRACY_ENABLE "
                COMMON_INCLUDES " "
                "/Fo%s "
                "/Fd" OBJ_CORE_DIR "/core.pdb "
                "%s",
                tool_cxx, objs[i], sources[i]);
#else
            snprintf(cmd, sizeof(cmd),
                "%s -std=c++20 -fPIC -g -O0 -Wno-changes-meaning -c "
                "-DTRACY_ENABLE "
                COMMON_INCLUDES " "
                "-o %s "
                "%s",
                tool_cxx, objs[i], sources[i]);
#endif
            if (run_cmd(cmd) != 0) return 1;
            any_rebuilt = 1;
        }
    }

    if (any_rebuilt ||
        needs_rebuild(objs[0], DEBUG_DIR "/" DLL_PREFIX "core" DLL_EXT)) {
        pos = 0;
        for (i = 0; i < count; i++) {
            pos += snprintf(obj_list + pos, sizeof(obj_list) - pos,
                            "%s ", objs[i]);
            if (pos >= (int)sizeof(obj_list)) {
                printf("!! object list too long\n");
                return 1;
            }
        }

#ifdef _WIN32
        snprintf(cmd, sizeof(cmd),
            "\"%s\" /nologo /DLL /DEBUG "
            "/OUT:" DEBUG_DIR "/" DLL_PREFIX "core" DLL_EXT " "
            "/IMPLIB:" DEBUG_DIR "/core" LIB_EXT " "
            "%s"
            DEBUG_DIR "/externals" LIB_EXT " "
            DEBUG_DIR "/TracyClient" LIB_EXT " "
            DEBUG_DIR "/SDL3" LIB_EXT " "
            "ws2_32.lib dbghelp.lib advapi32.lib user32.lib",
            tool_link, obj_list);
#else
        snprintf(cmd, sizeof(cmd),
            "%s -shared -o " DEBUG_DIR "/" DLL_PREFIX "core" DLL_EXT " "
            "%s"
            DEBUG_DIR "/" DLL_PREFIX "externals" DLL_EXT " "
            DEBUG_DIR "/TracyClient" LIB_EXT " "
            "-L" DEBUG_DIR " -lSDL3 "
            "-lpthread -ldl",
            tool_link, obj_list);
#endif
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
        "src/engine/engine.c",
        "src/engine/renderer.c",
        "src/engine/physics.c",
        "src/engine/scene.c",
        "src/engine/debug_render.c"
    };
    static const char *objs[] = {
        OBJ_ENGINE_DIR "/engine" OBJ_EXT,
        OBJ_ENGINE_DIR "/renderer" OBJ_EXT,
        OBJ_ENGINE_DIR "/physics" OBJ_EXT,
        OBJ_ENGINE_DIR "/scene" OBJ_EXT,
        OBJ_ENGINE_DIR "/debug_render" OBJ_EXT
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
#ifdef _WIN32
            snprintf(cmd, sizeof(cmd),
                "\"%s\" /TC /MD /Zi /Od /nologo /c "
                COMMON_INCLUDES " "
                "/Fo%s "
                "/Fd" OBJ_ENGINE_DIR "/engine.pdb "
                "%s",
                tool_cc, objs[i], sources[i]);
#else
            snprintf(cmd, sizeof(cmd),
                "%s -fPIC -g -O0 -c "
                COMMON_INCLUDES " "
                "-o %s "
                "%s",
                tool_cc, objs[i], sources[i]);
#endif
            if (run_cmd(cmd) != 0) return 1;
            any_rebuilt = 1;
        }
    }

    if (any_rebuilt ||
        needs_rebuild(objs[0], DEBUG_DIR "/" DLL_PREFIX "engine" DLL_EXT)) {
        pos = 0;
        for (i = 0; i < count; i++) {
            pos += snprintf(obj_list + pos, sizeof(obj_list) - pos,
                            "%s ", objs[i]);
            if (pos >= (int)sizeof(obj_list)) {
                printf("!! object list too long\n");
                return 1;
            }
        }

#ifdef _WIN32
        rand_hex(pdb_suffix, 8);

        snprintf(cmd, sizeof(cmd),
            "\"%s\" /nologo /DLL /DEBUG "
            "/PDB:" DEBUG_DIR "/engine_%s.pdb "
            "/OUT:" DEBUG_DIR "/" DLL_PREFIX "engine" DLL_EXT " "
            "/IMPLIB:" DEBUG_DIR "/engine" LIB_EXT " "
            "%s"
            DEBUG_DIR "/externals" LIB_EXT " "
            DEBUG_DIR "/TracyClient" LIB_EXT " "
            "ws2_32.lib dbghelp.lib advapi32.lib user32.lib",
            tool_link, pdb_suffix, obj_list);
#else
        (void)pdb_suffix;
        snprintf(cmd, sizeof(cmd),
            "%s -shared -o " DEBUG_DIR "/" DLL_PREFIX "engine" DLL_EXT " "
            "%s"
            DEBUG_DIR "/" DLL_PREFIX "externals" DLL_EXT " "
            DEBUG_DIR "/TracyClient" LIB_EXT " "
            "-lpthread -ldl",
            tool_link, obj_list);
#endif
        if (run_cmd(cmd) != 0) return 1;
    } else {
        printf("   engine is up to date.\n");
    }

    return 0;
}

/* ------- watch (forge) -------------------------------------------------- */

#ifdef _WIN32
#define HOTRELOAD_EVENT_NAME "Global\\ReloadEvent"

#define TCC_COMPILE_CMD \
    ".\\tcc.exe -Blib/tcc -shared" \
    " -o build/Debug/engine.dll" \
    " -Isrc -Isrc/engine" \
    " src/engine/engine.c" \
    " src/engine/renderer.c" \
    " src/engine/physics.c" \
    " src/engine/scene.c" \
    " src/engine/debug_render.c"
#else
#define TCC_COMPILE_CMD \
    "./tcc -Blib/tcc-linux -shared" \
    " -o build/Debug/engine.so" \
    " -Isrc -Isrc/engine" \
    " src/engine/*.c"
#endif

static int watch_and_rebuild(void)
{
#ifdef _WIN32
    HANDLE hDir;
    HANDLE hEvent;
    char buffer[4096];
    DWORD bytesReturned;
    OVERLAPPED overlapped = {0};
    char src_path[PATH_SIZE];
    char dst_path[PATH_SIZE];

    printf("=== Forge: watching src/engine for changes ===\n");
    fflush(stdout);

    /* Open directory handle for watching */
    hDir = CreateFileA(
        "src/engine",
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        NULL);
    if (hDir == INVALID_HANDLE_VALUE) {
        printf("!! Failed to open src/engine for watching (error %lu)\n",
               GetLastError());
        return 1;
    }

    overlapped.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (overlapped.hEvent == NULL) {
        printf("!! CreateEvent failed (error %lu)\n", GetLastError());
        CloseHandle(hDir);
        return 1;
    }

    /* Create/open the named event for signaling the engine */
    hEvent = CreateEventA(NULL, TRUE, FALSE, HOTRELOAD_EVENT_NAME);
    if (hEvent == NULL) {
        printf("!! Failed to create reload event (error %lu)\n",
               GetLastError());
        CloseHandle(overlapped.hEvent);
        CloseHandle(hDir);
        return 1;
    }

    /* Build absolute paths for DLL copy */
    {
        char cwd[PATH_SIZE];
        GetCurrentDirectoryA(PATH_SIZE, cwd);
        snprintf(src_path, PATH_SIZE, "%s/build/Debug/engine.dll", cwd);
        snprintf(dst_path, PATH_SIZE, "%s/build/Debug/engine_copy.dll", cwd);
    }

    /* Ensure build output directory exists */
    if (ensure_dirs() != 0) return 1;

    /* Do an initial compile so engine_copy.dll exists on disk */
    printf(">> " TCC_COMPILE_CMD "\n");
    fflush(stdout);
    if (system(TCC_COMPILE_CMD) == 0) {
        CopyFileA(src_path, dst_path, FALSE);
        printf("   Initial compile OK, engine_copy.dll ready.\n");
        SetEvent(hEvent);
    } else {
        printf("!! Initial compile failed. Waiting for changes...\n");
    }

    /* Watch loop */
    while (1) {
        if (!ReadDirectoryChangesW(
                hDir, buffer, sizeof(buffer), TRUE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE,
                &bytesReturned, &overlapped, NULL)) {
            printf("!! ReadDirectoryChangesW failed (error %lu)\n",
                   GetLastError());
            break;
        }

        WaitForSingleObject(overlapped.hEvent, INFINITE);
        ResetEvent(overlapped.hEvent);

        /* Debounce: wait 50ms, drain any extra notifications */
        Sleep(50);
        {
            OVERLAPPED drain = {0};
            DWORD drained;
            drain.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
            ReadDirectoryChangesW(hDir, buffer, sizeof(buffer), TRUE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE,
                &drained, &drain, NULL);
            CancelIo(hDir);
            CloseHandle(drain.hEvent);
        }

        printf("\n--- Change detected, recompiling... ---\n");
        fflush(stdout);

        printf(">> " TCC_COMPILE_CMD "\n");
        fflush(stdout);

        if (system(TCC_COMPILE_CMD) == 0) {
            if (CopyFileA(src_path, dst_path, FALSE)) {
                printf("   Compile OK. Signaling engine...\n");
                SetEvent(hEvent);
                ResetEvent(hEvent);
            } else {
                printf("!! DLL copy failed (error %lu)\n", GetLastError());
            }
        } else {
            printf("!! Compile failed. Engine keeps running old code.\n");
        }
        fflush(stdout);
    }

    CloseHandle(hEvent);
    CloseHandle(overlapped.hEvent);
    CloseHandle(hDir);
    return 0;

#else /* Linux inotify */
    int ifd, wfd;
    struct pollfd pfd;
    char buf[4096];

    printf("=== Forge: watching src/engine for changes ===\n");
    fflush(stdout);

    ifd = inotify_init();
    if (ifd < 0) {
        printf("!! inotify_init failed\n");
        return 1;
    }

    wfd = inotify_add_watch(ifd, "src/engine",
        IN_MODIFY | IN_CREATE | IN_MOVED_TO);
    if (wfd < 0) {
        printf("!! inotify_add_watch failed on src/engine\n");
        close(ifd);
        return 1;
    }

    /* Ensure build output directory exists */
    if (ensure_dirs() != 0) return 1;

    /* Initial compile */
    printf(">> " TCC_COMPILE_CMD "\n");
    fflush(stdout);
    if (system(TCC_COMPILE_CMD) == 0) {
        /* Touch reload signal file */
        fclose(fopen(DEBUG_DIR "/.reload-signal", "w"));
        printf("   Initial compile OK, engine.so ready.\n");
    } else {
        printf("!! Initial compile failed. Waiting for changes...\n");
    }

    /* Watch loop */
    pfd.fd = ifd;
    pfd.events = POLLIN;

    while (1) {
        int ret = poll(&pfd, 1, -1);
        if (ret < 0) {
            printf("!! poll failed\n");
            break;
        }

        /* Drain inotify events */
        {
            ssize_t n = read(ifd, buf, sizeof(buf));
            (void)n;
        }

        /* Debounce: wait 50ms and drain again */
        usleep(50000);
        {
            struct pollfd drain_pfd;
            drain_pfd.fd = ifd;
            drain_pfd.events = POLLIN;
            while (poll(&drain_pfd, 1, 0) > 0) {
                ssize_t n = read(ifd, buf, sizeof(buf));
                (void)n;
            }
        }

        printf("\n--- Change detected, recompiling... ---\n");
        fflush(stdout);

        printf(">> " TCC_COMPILE_CMD "\n");
        fflush(stdout);

        if (system(TCC_COMPILE_CMD) == 0) {
            /* Touch reload signal file */
            fclose(fopen(DEBUG_DIR "/.reload-signal", "w"));
            printf("   Compile OK. Reload signal written.\n");
        } else {
            printf("!! Compile failed. Engine keeps running old code.\n");
        }
        fflush(stdout);
    }

    close(ifd);
    return 0;
#endif
}

/* ------- exe ------------------------------------------------------------ */
static int build_exe(void)
{
#ifdef _WIN32
    static const char *sources[] = {
        "src/main.cpp",
        "src/core/loadlibrary_windows.cpp"
    };
    static const char *objs[] = {
        OBJ_EXE_DIR "/main" OBJ_EXT,
        OBJ_EXE_DIR "/loadlibrary_windows" OBJ_EXT
    };
#else
    static const char *sources[] = {
        "src/main.cpp",
        "src/core/loadlibrary_linux.cpp"
    };
    static const char *objs[] = {
        OBJ_EXE_DIR "/main" OBJ_EXT,
        OBJ_EXE_DIR "/loadlibrary_linux" OBJ_EXT
    };
#endif
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
#ifdef _WIN32
            snprintf(cmd, sizeof(cmd),
                "\"%s\" /std:c++20 /EHsc /MD /Zi /Od /nologo /c "
                COMMON_INCLUDES " "
                "/Fo%s "
                "/Fd" OBJ_EXE_DIR "/exe.pdb "
                "%s",
                tool_cxx, objs[i], sources[i]);
#else
            snprintf(cmd, sizeof(cmd),
                "%s -std=c++20 -fPIC -g -O0 -c "
                COMMON_INCLUDES " "
                "-o %s "
                "%s",
                tool_cxx, objs[i], sources[i]);
#endif
            if (run_cmd(cmd) != 0) return 1;
            any_rebuilt = 1;
        }
    }

    if (any_rebuilt ||
        needs_rebuild(objs[0], DEBUG_DIR "/AnitraEngine" EXE_EXT)) {
        pos = 0;
        for (i = 0; i < count; i++) {
            pos += snprintf(obj_list + pos, sizeof(obj_list) - pos,
                            "%s ", objs[i]);
            if (pos >= (int)sizeof(obj_list)) {
                printf("!! object list too long\n");
                return 1;
            }
        }

#ifdef _WIN32
        snprintf(cmd, sizeof(cmd),
            "\"%s\" /nologo /DEBUG "
            "/OUT:" DEBUG_DIR "/AnitraEngine" EXE_EXT " "
            "%s"
            DEBUG_DIR "/SDL3" LIB_EXT " "
            "user32.lib",
            tool_link, obj_list);
#else
        snprintf(cmd, sizeof(cmd),
            "%s -o " DEBUG_DIR "/AnitraEngine" EXE_EXT " "
            "%s"
            "-L" DEBUG_DIR " -lSDL3 "
            "-Wl,-rpath,'$ORIGIN'",
            tool_link, obj_list);
#endif
        if (run_cmd(cmd) != 0) return 1;
    } else {
        printf("   exe is up to date.\n");
    }

    return 0;
}

/* ------- sdl3 (DLL) ----------------------------------------------------- */

/*
 * Helper: flatten an SDL3 source path into an obj filename.
 * E.g. "lib/SDL3/src/audio/wasapi/SDL_wasapi.c" -> "audio_wasapi_SDL_wasapi.obj"
 * We strip the leading "lib/SDL3/src/" prefix (13 chars) and replace
 * remaining slashes with underscores, then swap the extension.
 */
static void sdl3_obj_path(char *out, size_t out_size, const char *src)
{
    const char *p;
    char *o;
    char *end;
    char *dot;

    /* Skip "lib/SDL3/src/" prefix (13 chars) */
    p = src + 13;

    o = out;
    end = out + out_size - 5; /* room for ".obj\0" */

    /* Write OBJ_SDL3_DIR prefix */
    {
        const char *dir = OBJ_SDL3_DIR "/";
        while (*dir && o < end) *o++ = *dir++;
    }

    /* Copy rest, replacing slashes with underscores */
    while (*p && o < end) {
        if (*p == '/' || *p == '\\') {
            *o++ = '_';
        } else {
            *o++ = *p;
        }
        p++;
    }
    *o = '\0';

    /* Replace .c or .cpp extension with platform obj extension */
    dot = strrchr(out, '.');
    if (dot) {
        strcpy(dot, OBJ_EXT);
    }
}

/* SDL3 common sources (shared by all platforms) */
static const char *sdl3_sources_common[] = {
    /* Root src/ */
    "lib/SDL3/src/SDL.c",
    "lib/SDL3/src/SDL_assert.c",
    "lib/SDL3/src/SDL_error.c",
    "lib/SDL3/src/SDL_guid.c",
    "lib/SDL3/src/SDL_hashtable.c",
    "lib/SDL3/src/SDL_hints.c",
    "lib/SDL3/src/SDL_list.c",
    "lib/SDL3/src/SDL_log.c",
    "lib/SDL3/src/SDL_properties.c",
    "lib/SDL3/src/SDL_utils.c",

    /* Subsystem generic files */
    "lib/SDL3/src/atomic/SDL_atomic.c",
    "lib/SDL3/src/atomic/SDL_spinlock.c",
    "lib/SDL3/src/audio/SDL_audio.c",
    "lib/SDL3/src/audio/SDL_audiocvt.c",
    "lib/SDL3/src/audio/SDL_audiodev.c",
    "lib/SDL3/src/audio/SDL_audioqueue.c",
    "lib/SDL3/src/audio/SDL_audioresample.c",
    "lib/SDL3/src/audio/SDL_audiotypecvt.c",
    "lib/SDL3/src/audio/SDL_mixer.c",
    "lib/SDL3/src/audio/SDL_wave.c",
    "lib/SDL3/src/camera/SDL_camera.c",
    "lib/SDL3/src/core/SDL_core_unsupported.c",
    "lib/SDL3/src/cpuinfo/SDL_cpuinfo.c",
    "lib/SDL3/src/dialog/SDL_dialog.c",
    "lib/SDL3/src/dialog/SDL_dialog_utils.c",
    "lib/SDL3/src/dynapi/SDL_dynapi.c",
    "lib/SDL3/src/events/SDL_categories.c",
    "lib/SDL3/src/events/SDL_clipboardevents.c",
    "lib/SDL3/src/events/SDL_displayevents.c",
    "lib/SDL3/src/events/SDL_dropevents.c",
    "lib/SDL3/src/events/SDL_events.c",
    "lib/SDL3/src/events/SDL_eventwatch.c",
    "lib/SDL3/src/events/SDL_keyboard.c",
    "lib/SDL3/src/events/SDL_keymap.c",
    "lib/SDL3/src/events/SDL_keysym_to_keycode.c",
    "lib/SDL3/src/events/SDL_keysym_to_scancode.c",
    "lib/SDL3/src/events/SDL_mouse.c",
    "lib/SDL3/src/events/SDL_pen.c",
    "lib/SDL3/src/events/SDL_quit.c",
    "lib/SDL3/src/events/SDL_scancode_tables.c",
    "lib/SDL3/src/events/SDL_touch.c",
    "lib/SDL3/src/events/SDL_windowevents.c",
    "lib/SDL3/src/events/imKStoUCS.c",
    "lib/SDL3/src/filesystem/SDL_filesystem.c",
    "lib/SDL3/src/gpu/SDL_gpu.c",
    "lib/SDL3/src/haptic/SDL_haptic.c",
    "lib/SDL3/src/hidapi/SDL_hidapi.c",
    "lib/SDL3/src/io/SDL_asyncio.c",
    "lib/SDL3/src/io/SDL_iostream.c",
    "lib/SDL3/src/joystick/SDL_gamepad.c",
    "lib/SDL3/src/joystick/SDL_joystick.c",
    "lib/SDL3/src/joystick/SDL_steam_virtual_gamepad.c",
    "lib/SDL3/src/joystick/controller_type.c",
    "lib/SDL3/src/locale/SDL_locale.c",
    "lib/SDL3/src/main/SDL_main_callbacks.c",
    "lib/SDL3/src/main/SDL_runapp.c",
    "lib/SDL3/src/misc/SDL_libusb.c",
    "lib/SDL3/src/misc/SDL_url.c",
    "lib/SDL3/src/power/SDL_power.c",
    "lib/SDL3/src/process/SDL_process.c",
    "lib/SDL3/src/render/SDL_render.c",
    "lib/SDL3/src/render/SDL_render_unsupported.c",
    "lib/SDL3/src/render/SDL_yuv_sw.c",
    "lib/SDL3/src/sensor/SDL_sensor.c",
    "lib/SDL3/src/stdlib/SDL_crc16.c",
    "lib/SDL3/src/stdlib/SDL_crc32.c",
    "lib/SDL3/src/stdlib/SDL_getenv.c",
    "lib/SDL3/src/stdlib/SDL_iconv.c",
    "lib/SDL3/src/stdlib/SDL_malloc.c",
    "lib/SDL3/src/stdlib/SDL_memcpy.c",
    "lib/SDL3/src/stdlib/SDL_memmove.c",
    "lib/SDL3/src/stdlib/SDL_memset.c",
    "lib/SDL3/src/stdlib/SDL_mslibc.c",
    "lib/SDL3/src/stdlib/SDL_murmur3.c",
    "lib/SDL3/src/stdlib/SDL_qsort.c",
    "lib/SDL3/src/stdlib/SDL_random.c",
    "lib/SDL3/src/stdlib/SDL_stdlib.c",
    "lib/SDL3/src/stdlib/SDL_string.c",
    "lib/SDL3/src/stdlib/SDL_strtokr.c",
    "lib/SDL3/src/storage/SDL_storage.c",
    "lib/SDL3/src/thread/SDL_thread.c",
    "lib/SDL3/src/time/SDL_time.c",
    "lib/SDL3/src/timer/SDL_timer.c",
    "lib/SDL3/src/tray/SDL_tray_utils.c",
    "lib/SDL3/src/video/SDL_RLEaccel.c",
    "lib/SDL3/src/video/SDL_blit.c",
    "lib/SDL3/src/video/SDL_blit_0.c",
    "lib/SDL3/src/video/SDL_blit_1.c",
    "lib/SDL3/src/video/SDL_blit_A.c",
    "lib/SDL3/src/video/SDL_blit_N.c",
    "lib/SDL3/src/video/SDL_blit_auto.c",
    "lib/SDL3/src/video/SDL_blit_copy.c",
    "lib/SDL3/src/video/SDL_blit_slow.c",
    "lib/SDL3/src/video/SDL_bmp.c",
    "lib/SDL3/src/video/SDL_clipboard.c",
    "lib/SDL3/src/video/SDL_egl.c",
    "lib/SDL3/src/video/SDL_fillrect.c",
    "lib/SDL3/src/video/SDL_pixels.c",
    "lib/SDL3/src/video/SDL_rect.c",
    "lib/SDL3/src/video/SDL_rotate.c",
    "lib/SDL3/src/video/SDL_stb.c",
    "lib/SDL3/src/video/SDL_stretch.c",
    "lib/SDL3/src/video/SDL_surface.c",
    "lib/SDL3/src/video/SDL_video.c",
    "lib/SDL3/src/video/SDL_video_unsupported.c",
    "lib/SDL3/src/video/SDL_vulkan_utils.c",
    "lib/SDL3/src/video/SDL_yuv.c",

    /* Dummy & offscreen video (shared by all platforms) */
    "lib/SDL3/src/video/dummy/SDL_nullevents.c",
    "lib/SDL3/src/video/dummy/SDL_nullframebuffer.c",
    "lib/SDL3/src/video/dummy/SDL_nullvideo.c",
    "lib/SDL3/src/video/offscreen/SDL_offscreenevents.c",
    "lib/SDL3/src/video/offscreen/SDL_offscreenframebuffer.c",
    "lib/SDL3/src/video/offscreen/SDL_offscreenopengles.c",
    "lib/SDL3/src/video/offscreen/SDL_offscreenvideo.c",
    "lib/SDL3/src/video/offscreen/SDL_offscreenvulkan.c",
    "lib/SDL3/src/video/offscreen/SDL_offscreenwindow.c",
    "lib/SDL3/src/video/yuv2rgb/yuv_rgb_lsx.c",
    "lib/SDL3/src/video/yuv2rgb/yuv_rgb_sse.c",
    "lib/SDL3/src/video/yuv2rgb/yuv_rgb_std.c",

    /* Render drivers (shared) */
    "lib/SDL3/src/render/opengl/SDL_render_gl.c",
    "lib/SDL3/src/render/opengl/SDL_shaders_gl.c",
    "lib/SDL3/src/render/opengles2/SDL_render_gles2.c",
    "lib/SDL3/src/render/opengles2/SDL_shaders_gles2.c",
    "lib/SDL3/src/render/software/SDL_blendfillrect.c",
    "lib/SDL3/src/render/software/SDL_blendline.c",
    "lib/SDL3/src/render/software/SDL_blendpoint.c",
    "lib/SDL3/src/render/software/SDL_drawline.c",
    "lib/SDL3/src/render/software/SDL_drawpoint.c",
    "lib/SDL3/src/render/software/SDL_render_sw.c",
    "lib/SDL3/src/render/software/SDL_triangle.c",
    "lib/SDL3/src/render/gpu/SDL_pipeline_gpu.c",
    "lib/SDL3/src/render/gpu/SDL_render_gpu.c",
    "lib/SDL3/src/render/gpu/SDL_shaders_gpu.c",
    "lib/SDL3/src/render/vulkan/SDL_render_vulkan.c",
    "lib/SDL3/src/render/vulkan/SDL_shaders_vulkan.c",

    /* GPU drivers (shared) */
    "lib/SDL3/src/gpu/vulkan/SDL_gpu_vulkan.c",
    "lib/SDL3/src/gpu/xr/SDL_gpu_openxr.c",
    "lib/SDL3/src/gpu/xr/SDL_openxrdyn.c",

    /* libm */
    "lib/SDL3/src/libm/e_atan2.c",
    "lib/SDL3/src/libm/e_exp.c",
    "lib/SDL3/src/libm/e_fmod.c",
    "lib/SDL3/src/libm/e_log.c",
    "lib/SDL3/src/libm/e_log10.c",
    "lib/SDL3/src/libm/e_pow.c",
    "lib/SDL3/src/libm/e_rem_pio2.c",
    "lib/SDL3/src/libm/e_sqrt.c",
    "lib/SDL3/src/libm/k_cos.c",
    "lib/SDL3/src/libm/k_rem_pio2.c",
    "lib/SDL3/src/libm/k_sin.c",
    "lib/SDL3/src/libm/k_tan.c",
    "lib/SDL3/src/libm/s_atan.c",
    "lib/SDL3/src/libm/s_copysign.c",
    "lib/SDL3/src/libm/s_cos.c",
    "lib/SDL3/src/libm/s_fabs.c",
    "lib/SDL3/src/libm/s_floor.c",
    "lib/SDL3/src/libm/s_isinf.c",
    "lib/SDL3/src/libm/s_isinff.c",
    "lib/SDL3/src/libm/s_isnan.c",
    "lib/SDL3/src/libm/s_isnanf.c",
    "lib/SDL3/src/libm/s_modf.c",
    "lib/SDL3/src/libm/s_scalbn.c",
    "lib/SDL3/src/libm/s_sin.c",
    "lib/SDL3/src/libm/s_tan.c"
};

#ifdef _WIN32
/* SDL3 Windows-specific sources */
static const char *sdl3_sources_platform[] = {
    /* Audio */
    "lib/SDL3/src/audio/directsound/SDL_directsound.c",
    "lib/SDL3/src/audio/wasapi/SDL_wasapi.c",
    "lib/SDL3/src/audio/dummy/SDL_dummyaudio.c",
    "lib/SDL3/src/audio/disk/SDL_diskaudio.c",

    /* Camera */
    "lib/SDL3/src/camera/mediafoundation/SDL_camera_mediafoundation.c",
    "lib/SDL3/src/camera/dummy/SDL_camera_dummy.c",

    /* Core */
    "lib/SDL3/src/core/windows/SDL_gameinput.cpp",
    "lib/SDL3/src/core/windows/SDL_hid.c",
    "lib/SDL3/src/core/windows/SDL_immdevice.c",
    "lib/SDL3/src/core/windows/SDL_windows.c",
    "lib/SDL3/src/core/windows/SDL_xinput.c",

    /* Dialog */
    "lib/SDL3/src/dialog/windows/SDL_windowsdialog.c",

    /* Filesystem */
    "lib/SDL3/src/filesystem/windows/SDL_sysfilesystem.c",
    "lib/SDL3/src/filesystem/windows/SDL_sysfsops.c",

    /* Haptic */
    "lib/SDL3/src/haptic/windows/SDL_dinputhaptic.c",
    "lib/SDL3/src/haptic/windows/SDL_windowshaptic.c",
    "lib/SDL3/src/haptic/hidapi/SDL_hidapihaptic.c",
    "lib/SDL3/src/haptic/hidapi/SDL_hidapihaptic_lg4ff.c",

    /* HID */
    "lib/SDL3/src/hidapi/windows/hid.c",

    /* I/O */
    "lib/SDL3/src/io/generic/SDL_asyncio_generic.c",
    "lib/SDL3/src/io/windows/SDL_asyncio_windows_ioring.c",

    /* Joystick */
    "lib/SDL3/src/joystick/windows/SDL_dinputjoystick.c",
    "lib/SDL3/src/joystick/windows/SDL_rawinputjoystick.c",
    "lib/SDL3/src/joystick/windows/SDL_windows_gaming_input.c",
    "lib/SDL3/src/joystick/windows/SDL_windowsjoystick.c",
    "lib/SDL3/src/joystick/windows/SDL_xinputjoystick.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapijoystick.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_combined.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_gamecube.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_luna.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_ps3.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_ps4.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_ps5.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_rumble.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_shield.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_stadia.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_steam.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_steam_hori.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_steamdeck.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_switch.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_wii.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_xbox360.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_xbox360w.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_xboxone.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_gip.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_steam_triton.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_lg4ff.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_8bitdo.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_flydigi.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_gamesir.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_sinput.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_zuiki.c",
    "lib/SDL3/src/joystick/hidapi/SDL_report_descriptor.c",
    "lib/SDL3/src/joystick/virtual/SDL_virtualjoystick.c",
    "lib/SDL3/src/joystick/gdk/SDL_gameinputjoystick.cpp",
    "lib/SDL3/src/joystick/dummy/SDL_sysjoystick.c",

    /* Loadso */
    "lib/SDL3/src/loadso/windows/SDL_sysloadso.c",

    /* Locale */
    "lib/SDL3/src/locale/windows/SDL_syslocale.c",

    /* Main */
    "lib/SDL3/src/main/windows/SDL_sysmain_runapp.c",
    "lib/SDL3/src/main/generic/SDL_sysmain_callbacks.c",

    /* Misc */
    "lib/SDL3/src/misc/windows/SDL_sysurl.c",

    /* Power */
    "lib/SDL3/src/power/windows/SDL_syspower.c",

    /* Process */
    "lib/SDL3/src/process/windows/SDL_windowsprocess.c",

    /* Sensor */
    "lib/SDL3/src/sensor/windows/SDL_windowssensor.c",

    /* Storage */
    "lib/SDL3/src/storage/generic/SDL_genericstorage.c",
    "lib/SDL3/src/storage/steam/SDL_steamstorage.c",

    /* Thread */
    "lib/SDL3/src/thread/windows/SDL_syscond_cv.c",
    "lib/SDL3/src/thread/windows/SDL_sysmutex.c",
    "lib/SDL3/src/thread/windows/SDL_sysrwlock_srw.c",
    "lib/SDL3/src/thread/windows/SDL_syssem.c",
    "lib/SDL3/src/thread/windows/SDL_systhread.c",
    "lib/SDL3/src/thread/windows/SDL_systls.c",
    "lib/SDL3/src/thread/generic/SDL_syscond.c",
    "lib/SDL3/src/thread/generic/SDL_sysrwlock.c",

    /* Time & Timer */
    "lib/SDL3/src/time/windows/SDL_systime.c",
    "lib/SDL3/src/timer/windows/SDL_systimer.c",

    /* Tray */
    "lib/SDL3/src/tray/windows/SDL_tray.c",

    /* Video - Windows */
    "lib/SDL3/src/video/windows/SDL_windowsclipboard.c",
    "lib/SDL3/src/video/windows/SDL_windowsevents.c",
    "lib/SDL3/src/video/windows/SDL_windowsframebuffer.c",
    "lib/SDL3/src/video/windows/SDL_windowsgameinput.cpp",
    "lib/SDL3/src/video/windows/SDL_windowskeyboard.c",
    "lib/SDL3/src/video/windows/SDL_windowsmessagebox.c",
    "lib/SDL3/src/video/windows/SDL_windowsmodes.c",
    "lib/SDL3/src/video/windows/SDL_windowsmouse.c",
    "lib/SDL3/src/video/windows/SDL_windowsopengl.c",
    "lib/SDL3/src/video/windows/SDL_windowsopengles.c",
    "lib/SDL3/src/video/windows/SDL_windowsrawinput.c",
    "lib/SDL3/src/video/windows/SDL_windowsshape.c",
    "lib/SDL3/src/video/windows/SDL_windowsvideo.c",
    "lib/SDL3/src/video/windows/SDL_windowsvulkan.c",
    "lib/SDL3/src/video/windows/SDL_windowswindow.c",

    /* Render - D3D */
    "lib/SDL3/src/render/direct3d/SDL_render_d3d.c",
    "lib/SDL3/src/render/direct3d/SDL_shaders_d3d.c",
    "lib/SDL3/src/render/direct3d11/SDL_render_d3d11.c",
    "lib/SDL3/src/render/direct3d11/SDL_shaders_d3d11.c",
    "lib/SDL3/src/render/direct3d12/SDL_render_d3d12.c",
    "lib/SDL3/src/render/direct3d12/SDL_shaders_d3d12.c",

    /* GPU - D3D12 */
    "lib/SDL3/src/gpu/d3d12/SDL_gpu_d3d12.c"
};

#else
/* SDL3 Linux-specific sources */
static const char *sdl3_sources_platform[] = {
    /* Audio */
    "lib/SDL3/src/audio/alsa/SDL_alsa_audio.c",
    "lib/SDL3/src/audio/pulseaudio/SDL_pulseaudio.c",
    "lib/SDL3/src/audio/dummy/SDL_dummyaudio.c",
    "lib/SDL3/src/audio/disk/SDL_diskaudio.c",

    /* Camera */
    "lib/SDL3/src/camera/v4l2/SDL_camera_v4l2.c",
    "lib/SDL3/src/camera/dummy/SDL_camera_dummy.c",

    /* Core */
    "lib/SDL3/src/core/linux/SDL_dbus.c",
    "lib/SDL3/src/core/linux/SDL_evdev.c",
    "lib/SDL3/src/core/linux/SDL_evdev_capabilities.c",
    "lib/SDL3/src/core/linux/SDL_evdev_kbd.c",
    "lib/SDL3/src/core/linux/SDL_progressbar.c",
    "lib/SDL3/src/core/linux/SDL_threadprio.c",
    "lib/SDL3/src/core/linux/SDL_udev.c",
    "lib/SDL3/src/core/unix/SDL_appid.c",
    "lib/SDL3/src/core/unix/SDL_poll.c",

    /* Dialog */
    "lib/SDL3/src/dialog/unix/SDL_portaldialog.c",
    "lib/SDL3/src/dialog/unix/SDL_unixdialog.c",
    "lib/SDL3/src/dialog/unix/SDL_zenitydialog.c",
    "lib/SDL3/src/dialog/unix/SDL_zenitymessagebox.c",

    /* Filesystem */
    "lib/SDL3/src/filesystem/unix/SDL_sysfilesystem.c",
    "lib/SDL3/src/filesystem/posix/SDL_sysfsops.c",

    /* Haptic */
    "lib/SDL3/src/haptic/linux/SDL_syshaptic.c",
    "lib/SDL3/src/haptic/hidapi/SDL_hidapihaptic.c",
    "lib/SDL3/src/haptic/hidapi/SDL_hidapihaptic_lg4ff.c",

    /* HID */
    "lib/SDL3/src/hidapi/linux/hid.c",

    /* I/O */
    "lib/SDL3/src/io/generic/SDL_asyncio_generic.c",

    /* Joystick */
    "lib/SDL3/src/joystick/linux/SDL_sysjoystick.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapijoystick.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_combined.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_gamecube.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_luna.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_ps3.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_ps4.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_ps5.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_rumble.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_shield.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_stadia.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_steam.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_steam_hori.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_steamdeck.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_switch.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_wii.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_xbox360.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_xbox360w.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_xboxone.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_gip.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_steam_triton.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_lg4ff.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_8bitdo.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_flydigi.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_gamesir.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_sinput.c",
    "lib/SDL3/src/joystick/hidapi/SDL_hidapi_zuiki.c",
    "lib/SDL3/src/joystick/hidapi/SDL_report_descriptor.c",
    "lib/SDL3/src/joystick/virtual/SDL_virtualjoystick.c",
    "lib/SDL3/src/joystick/dummy/SDL_sysjoystick.c",

    /* Loadso */
    "lib/SDL3/src/loadso/dlopen/SDL_sysloadso.c",

    /* Locale */
    "lib/SDL3/src/locale/unix/SDL_syslocale.c",

    /* Main */
    "lib/SDL3/src/main/generic/SDL_sysmain_callbacks.c",

    /* Misc */
    "lib/SDL3/src/misc/unix/SDL_sysurl.c",

    /* Power */
    "lib/SDL3/src/power/linux/SDL_syspower.c",

    /* Process */
    "lib/SDL3/src/process/posix/SDL_posixprocess.c",

    /* Sensor */
    "lib/SDL3/src/sensor/dummy/SDL_dummysensor.c",

    /* Storage */
    "lib/SDL3/src/storage/generic/SDL_genericstorage.c",
    "lib/SDL3/src/storage/steam/SDL_steamstorage.c",

    /* Thread */
    "lib/SDL3/src/thread/pthread/SDL_syscond.c",
    "lib/SDL3/src/thread/pthread/SDL_sysmutex.c",
    "lib/SDL3/src/thread/pthread/SDL_sysrwlock.c",
    "lib/SDL3/src/thread/pthread/SDL_syssem.c",
    "lib/SDL3/src/thread/pthread/SDL_systhread.c",
    "lib/SDL3/src/thread/pthread/SDL_systls.c",

    /* Time & Timer */
    "lib/SDL3/src/time/unix/SDL_systime.c",
    "lib/SDL3/src/timer/unix/SDL_systimer.c",

    /* Tray */
    "lib/SDL3/src/tray/unix/SDL_tray.c",

    /* Video - X11 */
    "lib/SDL3/src/video/x11/edid-parse.c",
    "lib/SDL3/src/video/x11/SDL_x11clipboard.c",
    "lib/SDL3/src/video/x11/SDL_x11dyn.c",
    "lib/SDL3/src/video/x11/SDL_x11events.c",
    "lib/SDL3/src/video/x11/SDL_x11framebuffer.c",
    "lib/SDL3/src/video/x11/SDL_x11keyboard.c",
    "lib/SDL3/src/video/x11/SDL_x11messagebox.c",
    "lib/SDL3/src/video/x11/SDL_x11modes.c",
    "lib/SDL3/src/video/x11/SDL_x11mouse.c",
    "lib/SDL3/src/video/x11/SDL_x11opengl.c",
    "lib/SDL3/src/video/x11/SDL_x11opengles.c",
    "lib/SDL3/src/video/x11/SDL_x11pen.c",
    "lib/SDL3/src/video/x11/SDL_x11settings.c",
    "lib/SDL3/src/video/x11/SDL_x11shape.c",
    "lib/SDL3/src/video/x11/SDL_x11toolkit.c",
    "lib/SDL3/src/video/x11/SDL_x11touch.c",
    "lib/SDL3/src/video/x11/SDL_x11video.c",
    "lib/SDL3/src/video/x11/SDL_x11vulkan.c",
    "lib/SDL3/src/video/x11/SDL_x11window.c",
    "lib/SDL3/src/video/x11/SDL_x11xfixes.c",
    "lib/SDL3/src/video/x11/SDL_x11xinput2.c",
    "lib/SDL3/src/video/x11/SDL_x11xsync.c",
    "lib/SDL3/src/video/x11/SDL_x11xtest.c",
    "lib/SDL3/src/video/x11/xsettings-client.c"
};
#endif

static int build_sdl3(void)
{
    int common_count = sizeof(sdl3_sources_common) / sizeof(sdl3_sources_common[0]);
    int platform_count = sizeof(sdl3_sources_platform) / sizeof(sdl3_sources_platform[0]);
    int total_count = common_count + platform_count;
    int i;
    int any_rebuilt = 0;
    char cmd[CMD_MAX];
    char obj[PATH_SIZE];
    FILE *rsp;
    const char *src;
    const char *ext;
    int is_cpp;

    printf("\n=== Building SDL3 ===\n");
    if (ensure_dirs() != 0) return 1;

    /* Compile each source file individually */
    for (i = 0; i < total_count; i++) {
        src = (i < common_count)
            ? sdl3_sources_common[i]
            : sdl3_sources_platform[i - common_count];

        sdl3_obj_path(obj, sizeof(obj), src);

        if (!needs_rebuild(src, obj))
            continue;

        /* Detect C++ by extension */
        ext = strrchr(src, '.');
        is_cpp = (ext && strcmp(ext, ".cpp") == 0);

#ifdef _WIN32
        if (is_cpp) {
            snprintf(cmd, sizeof(cmd),
                "\"%s\" /std:c++20 /EHsc /MD /O2 /nologo /c "
                "/DSDL_BUILDING_SDL3 /DDLL_EXPORT /D_WINDOWS /DWIN32 "
                "/Ilib/SDL3/include /Ilib/SDL3/include/build_config "
                "/Ilib/SDL3/src "
                "/Fo%s "
                "/Fd" OBJ_SDL3_DIR "/sdl3.pdb "
                "%s",
                tool_cxx, obj, src);
        } else {
            snprintf(cmd, sizeof(cmd),
                "\"%s\" /TC /MD /O2 /nologo /c "
                "/DSDL_BUILDING_SDL3 /DDLL_EXPORT /D_WINDOWS /DWIN32 "
                "/Ilib/SDL3/include /Ilib/SDL3/include/build_config "
                "/Ilib/SDL3/src "
                "/Fo%s "
                "/Fd" OBJ_SDL3_DIR "/sdl3.pdb "
                "%s",
                tool_cc, obj, src);
        }
#else
        if (is_cpp) {
            snprintf(cmd, sizeof(cmd),
                "%s -std=c++20 -fPIC -O2 -c "
                "-DSDL_BUILDING_SDL3 -DDLL_EXPORT "
                "-Ilib/SDL3/include -Ilib/SDL3/include/build_config "
                "-Ilib/SDL3/src "
                "-o %s "
                "%s",
                tool_cxx, obj, src);
        } else {
            snprintf(cmd, sizeof(cmd),
                "%s -fPIC -O2 -c "
                "-DSDL_BUILDING_SDL3 -DDLL_EXPORT "
                "-Ilib/SDL3/include -Ilib/SDL3/include/build_config "
                "-Ilib/SDL3/src "
                "-o %s "
                "%s",
                tool_cc, obj, src);
        }
#endif
        if (run_cmd(cmd) != 0) return 1;
        any_rebuilt = 1;
    }

    /* Link step: generate response file then link into DLL */
    if (any_rebuilt ||
#ifdef _WIN32
        needs_rebuild(sdl3_sources_common[0], DEBUG_DIR "/SDL3" DLL_EXT)) {
#else
        needs_rebuild(sdl3_sources_common[0], DEBUG_DIR "/libSDL3" DLL_EXT)) {
#endif

        /* Write response file with all .obj paths */
        rsp = fopen(OBJ_SDL3_DIR "/sdl3_objs.txt", "w");
        if (!rsp) {
            printf("!! failed to create response file\n");
            return 1;
        }
        for (i = 0; i < total_count; i++) {
            src = (i < common_count)
                ? sdl3_sources_common[i]
                : sdl3_sources_platform[i - common_count];
            sdl3_obj_path(obj, sizeof(obj), src);
            fprintf(rsp, "%s\n", obj);
        }
        fclose(rsp);

#ifdef _WIN32
        snprintf(cmd, sizeof(cmd),
            "\"%s\" /nologo /DLL /DEBUG "
            "/OUT:" DEBUG_DIR "/SDL3" DLL_EXT " "
            "/IMPLIB:" DEBUG_DIR "/SDL3" LIB_EXT " "
            "@" OBJ_SDL3_DIR "/sdl3_objs.txt "
            "user32.lib gdi32.lib winmm.lib imm32.lib "
            "ole32.lib oleaut32.lib version.lib advapi32.lib "
            "setupapi.lib shell32.lib cfgmgr32.lib",
            tool_link);
#else
        snprintf(cmd, sizeof(cmd),
            "%s -shared -o " DEBUG_DIR "/libSDL3" DLL_EXT " "
            "@" OBJ_SDL3_DIR "/sdl3_objs.txt "
            "-lpthread -ldl -lm",
            tool_link);
#endif
        if (run_cmd(cmd) != 0) return 1;
    } else {
        printf("   SDL3 is up to date.\n");
    }

    return 0;
}

/* ------- spirv-cross (DLL) ---------------------------------------------- */
static int build_spirvcross(void)
{
    static const char *sources[] = {
        "lib/SDL_shadercross/external/SPIRV-Cross/spirv_cfg.cpp",
        "lib/SDL_shadercross/external/SPIRV-Cross/spirv_cpp.cpp",
        "lib/SDL_shadercross/external/SPIRV-Cross/spirv_cross.cpp",
        "lib/SDL_shadercross/external/SPIRV-Cross/spirv_cross_c.cpp",
        "lib/SDL_shadercross/external/SPIRV-Cross/spirv_cross_parsed_ir.cpp",
        "lib/SDL_shadercross/external/SPIRV-Cross/spirv_cross_util.cpp",
        "lib/SDL_shadercross/external/SPIRV-Cross/spirv_glsl.cpp",
        "lib/SDL_shadercross/external/SPIRV-Cross/spirv_hlsl.cpp",
        "lib/SDL_shadercross/external/SPIRV-Cross/spirv_msl.cpp",
        "lib/SDL_shadercross/external/SPIRV-Cross/spirv_parser.cpp",
        "lib/SDL_shadercross/external/SPIRV-Cross/spirv_reflect.cpp"
    };
    static const char *objs[] = {
        OBJ_SPIRVCROSS_DIR "/spirv_cfg" OBJ_EXT,
        OBJ_SPIRVCROSS_DIR "/spirv_cpp" OBJ_EXT,
        OBJ_SPIRVCROSS_DIR "/spirv_cross" OBJ_EXT,
        OBJ_SPIRVCROSS_DIR "/spirv_cross_c" OBJ_EXT,
        OBJ_SPIRVCROSS_DIR "/spirv_cross_parsed_ir" OBJ_EXT,
        OBJ_SPIRVCROSS_DIR "/spirv_cross_util" OBJ_EXT,
        OBJ_SPIRVCROSS_DIR "/spirv_glsl" OBJ_EXT,
        OBJ_SPIRVCROSS_DIR "/spirv_hlsl" OBJ_EXT,
        OBJ_SPIRVCROSS_DIR "/spirv_msl" OBJ_EXT,
        OBJ_SPIRVCROSS_DIR "/spirv_parser" OBJ_EXT,
        OBJ_SPIRVCROSS_DIR "/spirv_reflect" OBJ_EXT
    };
    int count = sizeof(sources) / sizeof(sources[0]);
    int i;
    int any_rebuilt = 0;
    char cmd[CMD_MAX];
    FILE *rsp;

    printf("\n=== Building SPIRV-Cross ===\n");
    if (ensure_dirs() != 0) return 1;

    for (i = 0; i < count; i++) {
        if (!needs_rebuild(sources[i], objs[i]))
            continue;

#ifdef _WIN32
        snprintf(cmd, sizeof(cmd),
            "\"%s\" /std:c++17 /EHsc /MD /O2 /nologo /c "
            "/DSPVC_EXPORT_SYMBOLS "
            "/DSPIRV_CROSS_C_API_GLSL=1 "
            "/DSPIRV_CROSS_C_API_HLSL=1 "
            "/DSPIRV_CROSS_C_API_MSL=1 "
            "/DSPIRV_CROSS_C_API_CPP=1 "
            "/DSPIRV_CROSS_C_API_REFLECT=1 "
            "/Ilib/SDL_shadercross/external/SPIRV-Cross "
            "/Fo%s "
            "/Fd" OBJ_SPIRVCROSS_DIR "/spirvcross.pdb "
            "%s",
            tool_cxx, objs[i], sources[i]);
#else
        snprintf(cmd, sizeof(cmd),
            "%s -std=c++17 -fPIC -O2 -c "
            "-DSPVC_EXPORT_SYMBOLS "
            "-DSPIRV_CROSS_C_API_GLSL=1 "
            "-DSPIRV_CROSS_C_API_HLSL=1 "
            "-DSPIRV_CROSS_C_API_MSL=1 "
            "-DSPIRV_CROSS_C_API_CPP=1 "
            "-DSPIRV_CROSS_C_API_REFLECT=1 "
            "-Ilib/SDL_shadercross/external/SPIRV-Cross "
            "-o %s "
            "%s",
            tool_cxx, objs[i], sources[i]);
#endif
        if (run_cmd(cmd) != 0) return 1;
        any_rebuilt = 1;
    }

    if (any_rebuilt ||
#ifdef _WIN32
        needs_rebuild(objs[0], DEBUG_DIR "/spirv-cross-c-shared" DLL_EXT)) {
#else
        needs_rebuild(objs[0], DEBUG_DIR "/libspirv-cross-c-shared" DLL_EXT)) {
#endif

        /* Write response file with all .obj paths */
        rsp = fopen(OBJ_SPIRVCROSS_DIR "/spirvcross_objs.txt", "w");
        if (!rsp) {
            printf("!! failed to create response file\n");
            return 1;
        }
        for (i = 0; i < count; i++) {
            fprintf(rsp, "%s\n", objs[i]);
        }
        fclose(rsp);

#ifdef _WIN32
        snprintf(cmd, sizeof(cmd),
            "\"%s\" /nologo /DLL /DEBUG "
            "/OUT:" DEBUG_DIR "/spirv-cross-c-shared" DLL_EXT " "
            "/IMPLIB:" DEBUG_DIR "/spirv-cross-c-shared" LIB_EXT " "
            "@" OBJ_SPIRVCROSS_DIR "/spirvcross_objs.txt",
            tool_link);
#else
        snprintf(cmd, sizeof(cmd),
            "%s -shared -o " DEBUG_DIR "/libspirv-cross-c-shared" DLL_EXT " "
            "@" OBJ_SPIRVCROSS_DIR "/spirvcross_objs.txt",
            tool_cxx);
#endif
        if (run_cmd(cmd) != 0) return 1;
    } else {
        printf("   SPIRV-Cross is up to date.\n");
    }

    return 0;
}

/* ------- shadercross (DLL) ---------------------------------------------- */
static int build_shadercross(void)
{
    char cmd[CMD_MAX];
    int any_rebuilt = 0;

    printf("\n=== Building SDL_shadercross ===\n");
    if (ensure_dirs() != 0) return 1;

    if (needs_rebuild("lib/SDL_shadercross/src/SDL_shadercross.c",
                      OBJ_SHADERCROSS_DIR "/SDL_shadercross" OBJ_EXT)) {
#ifdef _WIN32
        snprintf(cmd, sizeof(cmd),
            "\"%s\" /TC /MD /O2 /nologo /c "
            "/DDLL_EXPORT "
            "/DSDL_SHADERCROSS_DXC "
            "/Ilib/SDL_shadercross/include "
            "/Ilib/SDL3/include "
            "/Ilib/SDL_shadercross/external/SPIRV-Cross "
            "/Ilib/SDL_shadercross/external/prebuilt/inc "
            "/Fo" OBJ_SHADERCROSS_DIR "/SDL_shadercross" OBJ_EXT " "
            "/Fd" OBJ_SHADERCROSS_DIR "/shadercross.pdb "
            "lib/SDL_shadercross/src/SDL_shadercross.c",
            tool_cc);
#else
        snprintf(cmd, sizeof(cmd),
            "%s -fPIC -O2 -c "
            "-DDLL_EXPORT "
            "-Ilib/SDL_shadercross/include "
            "-Ilib/SDL3/include "
            "-Ilib/SDL_shadercross/external/SPIRV-Cross "
            "-Ilib/SDL_shadercross/external/prebuilt/inc "
            "-o " OBJ_SHADERCROSS_DIR "/SDL_shadercross" OBJ_EXT " "
            "lib/SDL_shadercross/src/SDL_shadercross.c",
            tool_cc);
#endif
        if (run_cmd(cmd) != 0) return 1;
        any_rebuilt = 1;
    }

    if (any_rebuilt ||
#ifdef _WIN32
        needs_rebuild(OBJ_SHADERCROSS_DIR "/SDL_shadercross" OBJ_EXT,
                      DEBUG_DIR "/SDL3_shadercross" DLL_EXT)) {
        snprintf(cmd, sizeof(cmd),
            "\"%s\" /nologo /DLL /DEBUG "
            "/OUT:" DEBUG_DIR "/SDL3_shadercross" DLL_EXT " "
            "/IMPLIB:" DEBUG_DIR "/SDL3_shadercross" LIB_EXT " "
            OBJ_SHADERCROSS_DIR "/SDL_shadercross" OBJ_EXT " "
            DEBUG_DIR "/SDL3" LIB_EXT " "
            DEBUG_DIR "/spirv-cross-c-shared" LIB_EXT " "
            "lib/SDL_shadercross/external/prebuilt/lib/x64/dxcompiler.lib",
            tool_link);
        if (run_cmd(cmd) != 0) return 1;
    } else {
        printf("   SDL_shadercross is up to date.\n");
    }

    /* Copy DXC runtime DLLs to build output */
    run_cmd("copy lib\\SDL_shadercross\\external\\prebuilt\\bin\\x64\\dxcompiler.dll "
            DEBUG_DIR "\\");
    run_cmd("copy lib\\SDL_shadercross\\external\\prebuilt\\bin\\x64\\dxil.dll "
            DEBUG_DIR "\\");
#else
        needs_rebuild(OBJ_SHADERCROSS_DIR "/SDL_shadercross" OBJ_EXT,
                      DEBUG_DIR "/libSDL3_shadercross" DLL_EXT)) {
        snprintf(cmd, sizeof(cmd),
            "%s -shared -o " DEBUG_DIR "/libSDL3_shadercross" DLL_EXT " "
            OBJ_SHADERCROSS_DIR "/SDL_shadercross" OBJ_EXT " "
            "-L" DEBUG_DIR " -lSDL3 -lspirv-cross-c-shared",
            tool_link);
        if (run_cmd(cmd) != 0) return 1;
    } else {
        printf("   SDL_shadercross is up to date.\n");
    }
    /* No DXC on Linux */
#endif

    return 0;
}

/* ------- shaders -------------------------------------------------------- */
#define SHADER_SRC_DIR   "assets/shaders"
#define SHADER_OUT_DIR   "assets/shaders/compiled"

static const char *shader_sources[] = {
    "sprite_vs.glsl",
    "sprite_fs.glsl",
    "debug_lines_vs.glsl",
    "debug_lines_fs.glsl",
    "ui_rect_vs.glsl",
    "ui_rect_fs.glsl",
    "font_vs.glsl",
    "font_fs.glsl",
    NULL
};

static int build_shaders(void)
{
    char cmd[CMD_MAX];
    char src[PATH_SIZE], dst[PATH_SIZE];
    const char *stage;
    int i;

    printf("=== Compiling GLSL -> SPIR-V ===\n");

    if (ensure_dir(SHADER_OUT_DIR)) return 1;

    for (i = 0; shader_sources[i]; i++) {
        const char *name = shader_sources[i];
        size_t len = strlen(name);

        /* Determine stage from filename suffix: _vs.glsl = vertex, _fs.glsl = fragment */
        if (len > 8 && name[len - 8] == '_' && name[len - 7] == 'v' && name[len - 6] == 's')
            stage = "vertex";
        else if (len > 8 && name[len - 8] == '_' && name[len - 7] == 'f' && name[len - 6] == 's')
            stage = "fragment";
        else {
            printf("!! cannot determine shader stage for: %s\n", name);
            return 1;
        }

        snprintf(src, sizeof(src), "%s/%s", SHADER_SRC_DIR, name);

        /* Build output name: replace .glsl with .spv */
        snprintf(dst, sizeof(dst), "%s/", SHADER_OUT_DIR);
        {
            size_t base_len = len - 5; /* strip ".glsl" */
            strncat(dst, name, base_len);
            strcat(dst, ".spv");
        }

        if (!needs_rebuild(src, dst)) continue;

        snprintf(cmd, sizeof(cmd), "glslc -fshader-stage=%s %s -o %s", stage, src, dst);
        if (run_cmd(cmd) != 0) return 1;
    }

    printf("   Shaders compiled.\n");
    return 0;
}

/* ------- all ------------------------------------------------------------ */
static int build_all(void)
{
    printf("=== Building all targets ===\n\n");

    if (build_sdl3() != 0) return 1;
    if (build_spirvcross() != 0) return 1;
    if (build_shadercross() != 0) return 1;
    if (build_shaders() != 0) return 1;
    if (build_tracy() != 0) return 1;
    force_rebuild = 1;
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
#ifdef _WIN32
    return run_cmd("if exist build rmdir /s /q build");
#else
    return run_cmd("rm -rf build");
#endif
}

/* ========================================================================= */
/* main                                                                       */
/* ========================================================================= */

static void print_usage(void)
{
#ifdef _WIN32
    printf("Usage: build.exe [target]\n");
#else
    printf("Usage: ./builder [target]\n");
#endif
    printf("\n");
    printf("Targets:\n");
    printf("  all          Build everything (default)\n");
    printf("  tracy        Build TracyClient static library\n");
    printf("  sdl3         Build SDL3 shared library\n");
    printf("  spirvcross   Build SPIRV-Cross shared library\n");
    printf("  shadercross  Build SDL_shadercross shared library\n");
    printf("  shaders      Compile GLSL shaders to SPIR-V\n");
    printf("  externals    Build externals shared library\n");
    printf("  core         Build core shared library\n");
    printf("  engine       Build engine shared library\n");
    printf("  exe          Build AnitraEngine executable\n");
    printf("  watch        Watch src/engine and recompile on change (forge)\n");
    printf("  clean        Delete build directory\n");
    printf("  help         Show this message\n");
}

int main(int argc, char *argv[])
{
    const char *target;
    int rc;

    if (find_tools() != 0) return 1;

    if (argc < 2) {
        target = "all";
    } else {
        target = argv[1];
    }

    if (strcmp(target, "all") == 0) {
        rc = build_all();
    } else if (strcmp(target, "tracy") == 0) {
        rc = build_tracy();
    } else if (strcmp(target, "sdl3") == 0) {
        rc = build_sdl3();
    } else if (strcmp(target, "spirvcross") == 0) {
        rc = build_spirvcross();
    } else if (strcmp(target, "shadercross") == 0) {
        rc = build_shadercross();
    } else if (strcmp(target, "shaders") == 0) {
        rc = build_shaders();
    } else if (strcmp(target, "externals") == 0) {
        rc = build_externals();
    } else if (strcmp(target, "core") == 0) {
        rc = build_core();
    } else if (strcmp(target, "engine") == 0) {
        rc = build_engine();
    } else if (strcmp(target, "exe") == 0) {
        rc = build_exe();
    } else if (strcmp(target, "watch") == 0) {
        rc = watch_and_rebuild();
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
