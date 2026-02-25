/*
 * build.c - nobuild build system for the Anitra game engine
 *
 * Compile:  cl build.c
 * Usage:    build.exe [target]
 *
 * Targets:  all (default), tracy, sdl3, spirvcross, shadercross, externals, core, engine, exe, clean
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
#define OBJ_EXT_DIR     "build\\obj\\externals"
#define OBJ_CORE_DIR    "build\\obj\\core"
#define OBJ_ENGINE_DIR  "build\\obj\\engine"
#define OBJ_EXE_DIR     "build\\obj\\exe"
#define OBJ_SDL3_DIR        "build\\obj\\sdl3"
#define OBJ_SPIRVCROSS_DIR  "build\\obj\\spirvcross"
#define OBJ_SHADERCROSS_DIR "build\\obj\\shadercross"

/* Common include paths used by externals, core, engine, and exe targets */
#define COMMON_INCLUDES \
    "/Isrc /Isrc\\core /Isrc\\engine /Isrc\\externals /Iinclude " \
    "/Ilib\\SDL3\\include /Ilib\\SDL_shadercross\\include " \
    "/Ilib\\SDL_shadercross\\external\\SPIRV-Cross " \
    "/Ilib\\SDL_shadercross\\external\\prebuilt\\inc " \
    "/Ilib\\tracy\\public"

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

/* ------- externals (DLL) ------------------------------------------------ */
static int build_externals(void)
{
    static const char *sources[] = {
        "src\\externals\\externals.cpp"
    };
    static const char *objs[] = {
        OBJ_EXT_DIR "\\externals.obj"
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
                "/DTRACY_ENABLE "
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
            DEBUG_DIR "\\SDL3.lib "
            DEBUG_DIR "\\SDL3_shadercross.lib "
            DEBUG_DIR "\\TracyClient.lib "
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
            DEBUG_DIR "\\SDL3.lib "
            "ws2_32.lib dbghelp.lib advapi32.lib user32.lib",
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
        "src\\engine\\engine.c",
        "src\\engine\\renderer.c",
        "src\\engine\\physics.c",
        "src\\engine\\scene.c",
        "src\\engine\\debug_render.c"
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
                "\"%s\" /TC /MD /Zi /Od /nologo /c "
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
            DEBUG_DIR "\\TracyClient.lib "
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
            DEBUG_DIR "\\SDL3.lib "
            "user32.lib",
            msvc_link, obj_list);
        if (run_cmd(cmd) != 0) return 1;
    } else {
        printf("   exe is up to date.\n");
    }

    return 0;
}

/* ------- sdl3 (DLL) ----------------------------------------------------- */

/*
 * Helper: flatten an SDL3 source path into an obj filename.
 * E.g. "src\\audio\\wasapi\\SDL_wasapi.c" -> "audio_wasapi_SDL_wasapi.obj"
 * We strip the leading "src\\" prefix and replace remaining backslashes
 * with underscores, then swap the extension to .obj.
 */
static void sdl3_obj_path(char *out, size_t out_size, const char *src)
{
    const char *p;
    char *o;
    char *end;
    char *dot;

    /* Skip "lib\\SDL3\\src\\" prefix (13 chars) */
    p = src + 13;

    o = out;
    end = out + out_size - 5; /* room for ".obj\0" */

    /* Write OBJ_SDL3_DIR prefix */
    {
        const char *dir = OBJ_SDL3_DIR "\\";
        while (*dir && o < end) *o++ = *dir++;
    }

    /* Copy rest, replacing backslashes with underscores */
    while (*p && o < end) {
        if (*p == '\\') {
            *o++ = '_';
        } else {
            *o++ = *p;
        }
        p++;
    }
    *o = '\0';

    /* Replace .c or .cpp extension with .obj */
    dot = strrchr(out, '.');
    if (dot) {
        strcpy(dot, ".obj");
    }
}

static int build_sdl3(void)
{
    static const char *sources[] = {
        /* Root src/ */
        "lib\\SDL3\\src\\SDL.c",
        "lib\\SDL3\\src\\SDL_assert.c",
        "lib\\SDL3\\src\\SDL_error.c",
        "lib\\SDL3\\src\\SDL_guid.c",
        "lib\\SDL3\\src\\SDL_hashtable.c",
        "lib\\SDL3\\src\\SDL_hints.c",
        "lib\\SDL3\\src\\SDL_list.c",
        "lib\\SDL3\\src\\SDL_log.c",
        "lib\\SDL3\\src\\SDL_properties.c",
        "lib\\SDL3\\src\\SDL_utils.c",

        /* Subsystem generic files */
        "lib\\SDL3\\src\\atomic\\SDL_atomic.c",
        "lib\\SDL3\\src\\atomic\\SDL_spinlock.c",
        "lib\\SDL3\\src\\audio\\SDL_audio.c",
        "lib\\SDL3\\src\\audio\\SDL_audiocvt.c",
        "lib\\SDL3\\src\\audio\\SDL_audiodev.c",
        "lib\\SDL3\\src\\audio\\SDL_audioqueue.c",
        "lib\\SDL3\\src\\audio\\SDL_audioresample.c",
        "lib\\SDL3\\src\\audio\\SDL_audiotypecvt.c",
        "lib\\SDL3\\src\\audio\\SDL_mixer.c",
        "lib\\SDL3\\src\\audio\\SDL_wave.c",
        "lib\\SDL3\\src\\camera\\SDL_camera.c",
        "lib\\SDL3\\src\\core\\SDL_core_unsupported.c",
        "lib\\SDL3\\src\\cpuinfo\\SDL_cpuinfo.c",
        "lib\\SDL3\\src\\dialog\\SDL_dialog.c",
        "lib\\SDL3\\src\\dialog\\SDL_dialog_utils.c",
        "lib\\SDL3\\src\\dynapi\\SDL_dynapi.c",
        "lib\\SDL3\\src\\events\\SDL_categories.c",
        "lib\\SDL3\\src\\events\\SDL_clipboardevents.c",
        "lib\\SDL3\\src\\events\\SDL_displayevents.c",
        "lib\\SDL3\\src\\events\\SDL_dropevents.c",
        "lib\\SDL3\\src\\events\\SDL_events.c",
        "lib\\SDL3\\src\\events\\SDL_eventwatch.c",
        "lib\\SDL3\\src\\events\\SDL_keyboard.c",
        "lib\\SDL3\\src\\events\\SDL_keymap.c",
        "lib\\SDL3\\src\\events\\SDL_keysym_to_keycode.c",
        "lib\\SDL3\\src\\events\\SDL_keysym_to_scancode.c",
        "lib\\SDL3\\src\\events\\SDL_mouse.c",
        "lib\\SDL3\\src\\events\\SDL_pen.c",
        "lib\\SDL3\\src\\events\\SDL_quit.c",
        "lib\\SDL3\\src\\events\\SDL_scancode_tables.c",
        "lib\\SDL3\\src\\events\\SDL_touch.c",
        "lib\\SDL3\\src\\events\\SDL_windowevents.c",
        "lib\\SDL3\\src\\events\\imKStoUCS.c",
        "lib\\SDL3\\src\\filesystem\\SDL_filesystem.c",
        "lib\\SDL3\\src\\gpu\\SDL_gpu.c",
        "lib\\SDL3\\src\\haptic\\SDL_haptic.c",
        "lib\\SDL3\\src\\hidapi\\SDL_hidapi.c",
        "lib\\SDL3\\src\\io\\SDL_asyncio.c",
        "lib\\SDL3\\src\\io\\SDL_iostream.c",
        "lib\\SDL3\\src\\joystick\\SDL_gamepad.c",
        "lib\\SDL3\\src\\joystick\\SDL_joystick.c",
        "lib\\SDL3\\src\\joystick\\SDL_steam_virtual_gamepad.c",
        "lib\\SDL3\\src\\joystick\\controller_type.c",
        "lib\\SDL3\\src\\locale\\SDL_locale.c",
        "lib\\SDL3\\src\\main\\SDL_main_callbacks.c",
        "lib\\SDL3\\src\\main\\SDL_runapp.c",
        "lib\\SDL3\\src\\misc\\SDL_libusb.c",
        "lib\\SDL3\\src\\misc\\SDL_url.c",
        "lib\\SDL3\\src\\power\\SDL_power.c",
        "lib\\SDL3\\src\\process\\SDL_process.c",
        "lib\\SDL3\\src\\render\\SDL_render.c",
        "lib\\SDL3\\src\\render\\SDL_render_unsupported.c",
        "lib\\SDL3\\src\\render\\SDL_yuv_sw.c",
        "lib\\SDL3\\src\\sensor\\SDL_sensor.c",
        "lib\\SDL3\\src\\stdlib\\SDL_crc16.c",
        "lib\\SDL3\\src\\stdlib\\SDL_crc32.c",
        "lib\\SDL3\\src\\stdlib\\SDL_getenv.c",
        "lib\\SDL3\\src\\stdlib\\SDL_iconv.c",
        "lib\\SDL3\\src\\stdlib\\SDL_malloc.c",
        "lib\\SDL3\\src\\stdlib\\SDL_memcpy.c",
        "lib\\SDL3\\src\\stdlib\\SDL_memmove.c",
        "lib\\SDL3\\src\\stdlib\\SDL_memset.c",
        "lib\\SDL3\\src\\stdlib\\SDL_mslibc.c",
        "lib\\SDL3\\src\\stdlib\\SDL_murmur3.c",
        "lib\\SDL3\\src\\stdlib\\SDL_qsort.c",
        "lib\\SDL3\\src\\stdlib\\SDL_random.c",
        "lib\\SDL3\\src\\stdlib\\SDL_stdlib.c",
        "lib\\SDL3\\src\\stdlib\\SDL_string.c",
        "lib\\SDL3\\src\\stdlib\\SDL_strtokr.c",
        "lib\\SDL3\\src\\storage\\SDL_storage.c",
        "lib\\SDL3\\src\\thread\\SDL_thread.c",
        "lib\\SDL3\\src\\time\\SDL_time.c",
        "lib\\SDL3\\src\\timer\\SDL_timer.c",
        "lib\\SDL3\\src\\tray\\SDL_tray_utils.c",
        "lib\\SDL3\\src\\video\\SDL_RLEaccel.c",
        "lib\\SDL3\\src\\video\\SDL_blit.c",
        "lib\\SDL3\\src\\video\\SDL_blit_0.c",
        "lib\\SDL3\\src\\video\\SDL_blit_1.c",
        "lib\\SDL3\\src\\video\\SDL_blit_A.c",
        "lib\\SDL3\\src\\video\\SDL_blit_N.c",
        "lib\\SDL3\\src\\video\\SDL_blit_auto.c",
        "lib\\SDL3\\src\\video\\SDL_blit_copy.c",
        "lib\\SDL3\\src\\video\\SDL_blit_slow.c",
        "lib\\SDL3\\src\\video\\SDL_bmp.c",
        "lib\\SDL3\\src\\video\\SDL_clipboard.c",
        "lib\\SDL3\\src\\video\\SDL_egl.c",
        "lib\\SDL3\\src\\video\\SDL_fillrect.c",
        "lib\\SDL3\\src\\video\\SDL_pixels.c",
        "lib\\SDL3\\src\\video\\SDL_rect.c",
        "lib\\SDL3\\src\\video\\SDL_rotate.c",
        "lib\\SDL3\\src\\video\\SDL_stb.c",
        "lib\\SDL3\\src\\video\\SDL_stretch.c",
        "lib\\SDL3\\src\\video\\SDL_surface.c",
        "lib\\SDL3\\src\\video\\SDL_video.c",
        "lib\\SDL3\\src\\video\\SDL_video_unsupported.c",
        "lib\\SDL3\\src\\video\\SDL_vulkan_utils.c",
        "lib\\SDL3\\src\\video\\SDL_yuv.c",

        /* Windows-specific */
        "lib\\SDL3\\src\\audio\\directsound\\SDL_directsound.c",
        "lib\\SDL3\\src\\audio\\wasapi\\SDL_wasapi.c",
        "lib\\SDL3\\src\\audio\\dummy\\SDL_dummyaudio.c",
        "lib\\SDL3\\src\\audio\\disk\\SDL_diskaudio.c",
        "lib\\SDL3\\src\\camera\\mediafoundation\\SDL_camera_mediafoundation.c",
        "lib\\SDL3\\src\\camera\\dummy\\SDL_camera_dummy.c",
        "lib\\SDL3\\src\\core\\windows\\SDL_gameinput.cpp",    /* C++ */
        "lib\\SDL3\\src\\core\\windows\\SDL_hid.c",
        "lib\\SDL3\\src\\core\\windows\\SDL_immdevice.c",
        "lib\\SDL3\\src\\core\\windows\\SDL_windows.c",
        "lib\\SDL3\\src\\core\\windows\\SDL_xinput.c",
        "lib\\SDL3\\src\\dialog\\windows\\SDL_windowsdialog.c",
        "lib\\SDL3\\src\\filesystem\\windows\\SDL_sysfilesystem.c",
        "lib\\SDL3\\src\\filesystem\\windows\\SDL_sysfsops.c",
        "lib\\SDL3\\src\\haptic\\windows\\SDL_dinputhaptic.c",
        "lib\\SDL3\\src\\haptic\\windows\\SDL_windowshaptic.c",
        "lib\\SDL3\\src\\haptic\\hidapi\\SDL_hidapihaptic.c",
        "lib\\SDL3\\src\\haptic\\hidapi\\SDL_hidapihaptic_lg4ff.c",
        "lib\\SDL3\\src\\hidapi\\windows\\hid.c",
        "lib\\SDL3\\src\\io\\generic\\SDL_asyncio_generic.c",
        "lib\\SDL3\\src\\io\\windows\\SDL_asyncio_windows_ioring.c",
        "lib\\SDL3\\src\\joystick\\windows\\SDL_dinputjoystick.c",
        "lib\\SDL3\\src\\joystick\\windows\\SDL_rawinputjoystick.c",
        "lib\\SDL3\\src\\joystick\\windows\\SDL_windows_gaming_input.c",
        "lib\\SDL3\\src\\joystick\\windows\\SDL_windowsjoystick.c",
        "lib\\SDL3\\src\\joystick\\windows\\SDL_xinputjoystick.c",
        "lib\\SDL3\\src\\joystick\\hidapi\\SDL_hidapijoystick.c",
        "lib\\SDL3\\src\\joystick\\hidapi\\SDL_hidapi_combined.c",
        "lib\\SDL3\\src\\joystick\\hidapi\\SDL_hidapi_gamecube.c",
        "lib\\SDL3\\src\\joystick\\hidapi\\SDL_hidapi_luna.c",
        "lib\\SDL3\\src\\joystick\\hidapi\\SDL_hidapi_ps3.c",
        "lib\\SDL3\\src\\joystick\\hidapi\\SDL_hidapi_ps4.c",
        "lib\\SDL3\\src\\joystick\\hidapi\\SDL_hidapi_ps5.c",
        "lib\\SDL3\\src\\joystick\\hidapi\\SDL_hidapi_rumble.c",
        "lib\\SDL3\\src\\joystick\\hidapi\\SDL_hidapi_shield.c",
        "lib\\SDL3\\src\\joystick\\hidapi\\SDL_hidapi_stadia.c",
        "lib\\SDL3\\src\\joystick\\hidapi\\SDL_hidapi_steam.c",
        "lib\\SDL3\\src\\joystick\\hidapi\\SDL_hidapi_steam_hori.c",
        "lib\\SDL3\\src\\joystick\\hidapi\\SDL_hidapi_steamdeck.c",
        "lib\\SDL3\\src\\joystick\\hidapi\\SDL_hidapi_switch.c",
        "lib\\SDL3\\src\\joystick\\hidapi\\SDL_hidapi_wii.c",
        "lib\\SDL3\\src\\joystick\\hidapi\\SDL_hidapi_xbox360.c",
        "lib\\SDL3\\src\\joystick\\hidapi\\SDL_hidapi_xbox360w.c",
        "lib\\SDL3\\src\\joystick\\hidapi\\SDL_hidapi_xboxone.c",
        "lib\\SDL3\\src\\joystick\\hidapi\\SDL_hidapi_gip.c",
        "lib\\SDL3\\src\\joystick\\hidapi\\SDL_hidapi_steam_triton.c",
        "lib\\SDL3\\src\\joystick\\hidapi\\SDL_hidapi_lg4ff.c",
        "lib\\SDL3\\src\\joystick\\hidapi\\SDL_hidapi_8bitdo.c",
        "lib\\SDL3\\src\\joystick\\hidapi\\SDL_hidapi_flydigi.c",
        "lib\\SDL3\\src\\joystick\\hidapi\\SDL_hidapi_gamesir.c",
        "lib\\SDL3\\src\\joystick\\hidapi\\SDL_hidapi_sinput.c",
        "lib\\SDL3\\src\\joystick\\hidapi\\SDL_hidapi_zuiki.c",
        "lib\\SDL3\\src\\joystick\\hidapi\\SDL_report_descriptor.c",
        "lib\\SDL3\\src\\joystick\\virtual\\SDL_virtualjoystick.c",
        "lib\\SDL3\\src\\joystick\\gdk\\SDL_gameinputjoystick.cpp",  /* C++ */
        "lib\\SDL3\\src\\joystick\\dummy\\SDL_sysjoystick.c",
        "lib\\SDL3\\src\\loadso\\windows\\SDL_sysloadso.c",
        "lib\\SDL3\\src\\locale\\windows\\SDL_syslocale.c",
        "lib\\SDL3\\src\\main\\windows\\SDL_sysmain_runapp.c",
        "lib\\SDL3\\src\\main\\generic\\SDL_sysmain_callbacks.c",
        "lib\\SDL3\\src\\misc\\windows\\SDL_sysurl.c",
        "lib\\SDL3\\src\\power\\windows\\SDL_syspower.c",
        "lib\\SDL3\\src\\process\\windows\\SDL_windowsprocess.c",
        "lib\\SDL3\\src\\sensor\\windows\\SDL_windowssensor.c",
        "lib\\SDL3\\src\\storage\\generic\\SDL_genericstorage.c",
        "lib\\SDL3\\src\\storage\\steam\\SDL_steamstorage.c",
        "lib\\SDL3\\src\\thread\\windows\\SDL_syscond_cv.c",
        "lib\\SDL3\\src\\thread\\windows\\SDL_sysmutex.c",
        "lib\\SDL3\\src\\thread\\windows\\SDL_sysrwlock_srw.c",
        "lib\\SDL3\\src\\thread\\windows\\SDL_syssem.c",
        "lib\\SDL3\\src\\thread\\windows\\SDL_systhread.c",
        "lib\\SDL3\\src\\thread\\windows\\SDL_systls.c",
        "lib\\SDL3\\src\\thread\\generic\\SDL_syscond.c",
        "lib\\SDL3\\src\\thread\\generic\\SDL_sysrwlock.c",
        "lib\\SDL3\\src\\time\\windows\\SDL_systime.c",
        "lib\\SDL3\\src\\timer\\windows\\SDL_systimer.c",
        "lib\\SDL3\\src\\tray\\windows\\SDL_tray.c",
        "lib\\SDL3\\src\\video\\windows\\SDL_windowsclipboard.c",
        "lib\\SDL3\\src\\video\\windows\\SDL_windowsevents.c",
        "lib\\SDL3\\src\\video\\windows\\SDL_windowsframebuffer.c",
        "lib\\SDL3\\src\\video\\windows\\SDL_windowsgameinput.cpp",  /* C++ */
        "lib\\SDL3\\src\\video\\windows\\SDL_windowskeyboard.c",
        "lib\\SDL3\\src\\video\\windows\\SDL_windowsmessagebox.c",
        "lib\\SDL3\\src\\video\\windows\\SDL_windowsmodes.c",
        "lib\\SDL3\\src\\video\\windows\\SDL_windowsmouse.c",
        "lib\\SDL3\\src\\video\\windows\\SDL_windowsopengl.c",
        "lib\\SDL3\\src\\video\\windows\\SDL_windowsopengles.c",
        "lib\\SDL3\\src\\video\\windows\\SDL_windowsrawinput.c",
        "lib\\SDL3\\src\\video\\windows\\SDL_windowsshape.c",
        "lib\\SDL3\\src\\video\\windows\\SDL_windowsvideo.c",
        "lib\\SDL3\\src\\video\\windows\\SDL_windowsvulkan.c",
        "lib\\SDL3\\src\\video\\windows\\SDL_windowswindow.c",
        "lib\\SDL3\\src\\video\\dummy\\SDL_nullevents.c",
        "lib\\SDL3\\src\\video\\dummy\\SDL_nullframebuffer.c",
        "lib\\SDL3\\src\\video\\dummy\\SDL_nullvideo.c",
        "lib\\SDL3\\src\\video\\offscreen\\SDL_offscreenevents.c",
        "lib\\SDL3\\src\\video\\offscreen\\SDL_offscreenframebuffer.c",
        "lib\\SDL3\\src\\video\\offscreen\\SDL_offscreenopengles.c",
        "lib\\SDL3\\src\\video\\offscreen\\SDL_offscreenvideo.c",
        "lib\\SDL3\\src\\video\\offscreen\\SDL_offscreenvulkan.c",
        "lib\\SDL3\\src\\video\\offscreen\\SDL_offscreenwindow.c",
        "lib\\SDL3\\src\\video\\yuv2rgb\\yuv_rgb_lsx.c",
        "lib\\SDL3\\src\\video\\yuv2rgb\\yuv_rgb_sse.c",
        "lib\\SDL3\\src\\video\\yuv2rgb\\yuv_rgb_std.c",

        /* Render drivers */
        "lib\\SDL3\\src\\render\\direct3d\\SDL_render_d3d.c",
        "lib\\SDL3\\src\\render\\direct3d\\SDL_shaders_d3d.c",
        "lib\\SDL3\\src\\render\\direct3d11\\SDL_render_d3d11.c",
        "lib\\SDL3\\src\\render\\direct3d11\\SDL_shaders_d3d11.c",
        "lib\\SDL3\\src\\render\\direct3d12\\SDL_render_d3d12.c",
        "lib\\SDL3\\src\\render\\direct3d12\\SDL_shaders_d3d12.c",
        "lib\\SDL3\\src\\render\\opengl\\SDL_render_gl.c",
        "lib\\SDL3\\src\\render\\opengl\\SDL_shaders_gl.c",
        "lib\\SDL3\\src\\render\\opengles2\\SDL_render_gles2.c",
        "lib\\SDL3\\src\\render\\opengles2\\SDL_shaders_gles2.c",
        "lib\\SDL3\\src\\render\\software\\SDL_blendfillrect.c",
        "lib\\SDL3\\src\\render\\software\\SDL_blendline.c",
        "lib\\SDL3\\src\\render\\software\\SDL_blendpoint.c",
        "lib\\SDL3\\src\\render\\software\\SDL_drawline.c",
        "lib\\SDL3\\src\\render\\software\\SDL_drawpoint.c",
        "lib\\SDL3\\src\\render\\software\\SDL_render_sw.c",
        "lib\\SDL3\\src\\render\\software\\SDL_triangle.c",
        "lib\\SDL3\\src\\render\\gpu\\SDL_pipeline_gpu.c",
        "lib\\SDL3\\src\\render\\gpu\\SDL_render_gpu.c",
        "lib\\SDL3\\src\\render\\gpu\\SDL_shaders_gpu.c",
        "lib\\SDL3\\src\\render\\vulkan\\SDL_render_vulkan.c",
        "lib\\SDL3\\src\\render\\vulkan\\SDL_shaders_vulkan.c",

        /* GPU drivers */
        "lib\\SDL3\\src\\gpu\\d3d12\\SDL_gpu_d3d12.c",
        "lib\\SDL3\\src\\gpu\\vulkan\\SDL_gpu_vulkan.c",
        "lib\\SDL3\\src\\gpu\\xr\\SDL_gpu_openxr.c",
        "lib\\SDL3\\src\\gpu\\xr\\SDL_openxrdyn.c",

        /* libm */
        "lib\\SDL3\\src\\libm\\e_atan2.c",
        "lib\\SDL3\\src\\libm\\e_exp.c",
        "lib\\SDL3\\src\\libm\\e_fmod.c",
        "lib\\SDL3\\src\\libm\\e_log.c",
        "lib\\SDL3\\src\\libm\\e_log10.c",
        "lib\\SDL3\\src\\libm\\e_pow.c",
        "lib\\SDL3\\src\\libm\\e_rem_pio2.c",
        "lib\\SDL3\\src\\libm\\e_sqrt.c",
        "lib\\SDL3\\src\\libm\\k_cos.c",
        "lib\\SDL3\\src\\libm\\k_rem_pio2.c",
        "lib\\SDL3\\src\\libm\\k_sin.c",
        "lib\\SDL3\\src\\libm\\k_tan.c",
        "lib\\SDL3\\src\\libm\\s_atan.c",
        "lib\\SDL3\\src\\libm\\s_copysign.c",
        "lib\\SDL3\\src\\libm\\s_cos.c",
        "lib\\SDL3\\src\\libm\\s_fabs.c",
        "lib\\SDL3\\src\\libm\\s_floor.c",
        "lib\\SDL3\\src\\libm\\s_isinf.c",
        "lib\\SDL3\\src\\libm\\s_isinff.c",
        "lib\\SDL3\\src\\libm\\s_isnan.c",
        "lib\\SDL3\\src\\libm\\s_isnanf.c",
        "lib\\SDL3\\src\\libm\\s_modf.c",
        "lib\\SDL3\\src\\libm\\s_scalbn.c",
        "lib\\SDL3\\src\\libm\\s_sin.c",
        "lib\\SDL3\\src\\libm\\s_tan.c"
    };

    int count = sizeof(sources) / sizeof(sources[0]);
    int i;
    int any_rebuilt = 0;
    char cmd[CMD_MAX];
    char obj[MAX_PATH];
    FILE *rsp;
    const char *ext;
    int is_cpp;

    printf("\n=== Building SDL3 ===\n");
    if (ensure_dirs() != 0) return 1;

    /* Compile each source file individually */
    for (i = 0; i < count; i++) {
        sdl3_obj_path(obj, sizeof(obj), sources[i]);

        if (!needs_rebuild(sources[i], obj))
            continue;

        /* Detect C++ by extension */
        ext = strrchr(sources[i], '.');
        is_cpp = (ext && strcmp(ext, ".cpp") == 0);

        if (is_cpp) {
            snprintf(cmd, sizeof(cmd),
                "\"%s\" /std:c++20 /EHsc /MD /O2 /nologo /c "
                "/DSDL_BUILDING_SDL3 /DDLL_EXPORT /D_WINDOWS /DWIN32 "
                "/Ilib\\SDL3\\include /Ilib\\SDL3\\include\\build_config "
                "/Ilib\\SDL3\\src "
                "/Fo%s "
                "/Fd" OBJ_SDL3_DIR "\\sdl3.pdb "
                "%s",
                msvc_cl, obj, sources[i]);
        } else {
            snprintf(cmd, sizeof(cmd),
                "\"%s\" /TC /MD /O2 /nologo /c "
                "/DSDL_BUILDING_SDL3 /DDLL_EXPORT /D_WINDOWS /DWIN32 "
                "/Ilib\\SDL3\\include /Ilib\\SDL3\\include\\build_config "
                "/Ilib\\SDL3\\src "
                "/Fo%s "
                "/Fd" OBJ_SDL3_DIR "\\sdl3.pdb "
                "%s",
                msvc_cl, obj, sources[i]);
        }
        if (run_cmd(cmd) != 0) return 1;
        any_rebuilt = 1;
    }

    /* Link step: generate response file then link into DLL */
    if (any_rebuilt ||
        needs_rebuild(sources[0], DEBUG_DIR "\\SDL3.dll")) {

        /* Write response file with all .obj paths */
        rsp = fopen(OBJ_SDL3_DIR "\\sdl3_objs.txt", "w");
        if (!rsp) {
            printf("!! failed to create response file\n");
            return 1;
        }
        for (i = 0; i < count; i++) {
            sdl3_obj_path(obj, sizeof(obj), sources[i]);
            fprintf(rsp, "%s\n", obj);
        }
        fclose(rsp);

        snprintf(cmd, sizeof(cmd),
            "\"%s\" /nologo /DLL /DEBUG "
            "/OUT:" DEBUG_DIR "\\SDL3.dll "
            "/IMPLIB:" DEBUG_DIR "\\SDL3.lib "
            "@" OBJ_SDL3_DIR "\\sdl3_objs.txt "
            "user32.lib gdi32.lib winmm.lib imm32.lib "
            "ole32.lib oleaut32.lib version.lib advapi32.lib "
            "setupapi.lib shell32.lib cfgmgr32.lib",
            msvc_link);
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
        "lib\\SDL_shadercross\\external\\SPIRV-Cross\\spirv_cfg.cpp",
        "lib\\SDL_shadercross\\external\\SPIRV-Cross\\spirv_cpp.cpp",
        "lib\\SDL_shadercross\\external\\SPIRV-Cross\\spirv_cross.cpp",
        "lib\\SDL_shadercross\\external\\SPIRV-Cross\\spirv_cross_c.cpp",
        "lib\\SDL_shadercross\\external\\SPIRV-Cross\\spirv_cross_parsed_ir.cpp",
        "lib\\SDL_shadercross\\external\\SPIRV-Cross\\spirv_cross_util.cpp",
        "lib\\SDL_shadercross\\external\\SPIRV-Cross\\spirv_glsl.cpp",
        "lib\\SDL_shadercross\\external\\SPIRV-Cross\\spirv_hlsl.cpp",
        "lib\\SDL_shadercross\\external\\SPIRV-Cross\\spirv_msl.cpp",
        "lib\\SDL_shadercross\\external\\SPIRV-Cross\\spirv_parser.cpp",
        "lib\\SDL_shadercross\\external\\SPIRV-Cross\\spirv_reflect.cpp"
    };
    static const char *objs[] = {
        OBJ_SPIRVCROSS_DIR "\\spirv_cfg.obj",
        OBJ_SPIRVCROSS_DIR "\\spirv_cpp.obj",
        OBJ_SPIRVCROSS_DIR "\\spirv_cross.obj",
        OBJ_SPIRVCROSS_DIR "\\spirv_cross_c.obj",
        OBJ_SPIRVCROSS_DIR "\\spirv_cross_parsed_ir.obj",
        OBJ_SPIRVCROSS_DIR "\\spirv_cross_util.obj",
        OBJ_SPIRVCROSS_DIR "\\spirv_glsl.obj",
        OBJ_SPIRVCROSS_DIR "\\spirv_hlsl.obj",
        OBJ_SPIRVCROSS_DIR "\\spirv_msl.obj",
        OBJ_SPIRVCROSS_DIR "\\spirv_parser.obj",
        OBJ_SPIRVCROSS_DIR "\\spirv_reflect.obj"
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

        snprintf(cmd, sizeof(cmd),
            "\"%s\" /std:c++17 /EHsc /MD /O2 /nologo /c "
            "/DSPVC_EXPORT_SYMBOLS "
            "/DSPIRV_CROSS_C_API_GLSL=1 "
            "/DSPIRV_CROSS_C_API_HLSL=1 "
            "/DSPIRV_CROSS_C_API_MSL=1 "
            "/DSPIRV_CROSS_C_API_CPP=1 "
            "/DSPIRV_CROSS_C_API_REFLECT=1 "
            "/Ilib\\SDL_shadercross\\external\\SPIRV-Cross "
            "/Fo%s "
            "/Fd" OBJ_SPIRVCROSS_DIR "\\spirvcross.pdb "
            "%s",
            msvc_cl, objs[i], sources[i]);
        if (run_cmd(cmd) != 0) return 1;
        any_rebuilt = 1;
    }

    if (any_rebuilt ||
        needs_rebuild(objs[0], DEBUG_DIR "\\spirv-cross-c-shared.dll")) {

        /* Write response file with all .obj paths */
        rsp = fopen(OBJ_SPIRVCROSS_DIR "\\spirvcross_objs.txt", "w");
        if (!rsp) {
            printf("!! failed to create response file\n");
            return 1;
        }
        for (i = 0; i < count; i++) {
            fprintf(rsp, "%s\n", objs[i]);
        }
        fclose(rsp);

        snprintf(cmd, sizeof(cmd),
            "\"%s\" /nologo /DLL /DEBUG "
            "/OUT:" DEBUG_DIR "\\spirv-cross-c-shared.dll "
            "/IMPLIB:" DEBUG_DIR "\\spirv-cross-c-shared.lib "
            "@" OBJ_SPIRVCROSS_DIR "\\spirvcross_objs.txt",
            msvc_link);
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

    if (needs_rebuild("lib\\SDL_shadercross\\src\\SDL_shadercross.c",
                      OBJ_SHADERCROSS_DIR "\\SDL_shadercross.obj")) {
        snprintf(cmd, sizeof(cmd),
            "\"%s\" /TC /MD /O2 /nologo /c "
            "/DDLL_EXPORT "
            "/DSDL_SHADERCROSS_DXC "
            "/Ilib\\SDL_shadercross\\include "
            "/Ilib\\SDL3\\include "
            "/Ilib\\SDL_shadercross\\external\\SPIRV-Cross "
            "/Ilib\\SDL_shadercross\\external\\prebuilt\\inc "
            "/Fo" OBJ_SHADERCROSS_DIR "\\SDL_shadercross.obj "
            "/Fd" OBJ_SHADERCROSS_DIR "\\shadercross.pdb "
            "lib\\SDL_shadercross\\src\\SDL_shadercross.c",
            msvc_cl);
        if (run_cmd(cmd) != 0) return 1;
        any_rebuilt = 1;
    }

    if (any_rebuilt ||
        needs_rebuild(OBJ_SHADERCROSS_DIR "\\SDL_shadercross.obj",
                      DEBUG_DIR "\\SDL3_shadercross.dll")) {
        snprintf(cmd, sizeof(cmd),
            "\"%s\" /nologo /DLL /DEBUG "
            "/OUT:" DEBUG_DIR "\\SDL3_shadercross.dll "
            "/IMPLIB:" DEBUG_DIR "\\SDL3_shadercross.lib "
            OBJ_SHADERCROSS_DIR "\\SDL_shadercross.obj "
            DEBUG_DIR "\\SDL3.lib "
            DEBUG_DIR "\\spirv-cross-c-shared.lib "
            "lib\\SDL_shadercross\\external\\prebuilt\\lib\\x64\\dxcompiler.lib",
            msvc_link);
        if (run_cmd(cmd) != 0) return 1;
    } else {
        printf("   SDL_shadercross is up to date.\n");
    }

    /* Copy DXC runtime DLLs to build output */
    run_cmd("copy lib\\SDL_shadercross\\external\\prebuilt\\bin\\x64\\dxcompiler.dll "
            DEBUG_DIR "\\");
    run_cmd("copy lib\\SDL_shadercross\\external\\prebuilt\\bin\\x64\\dxil.dll "
            DEBUG_DIR "\\");

    return 0;
}

/* ------- all ------------------------------------------------------------ */
static int build_all(void)
{
    printf("=== Building all targets ===\n\n");

    if (build_sdl3() != 0) return 1;
    if (build_spirvcross() != 0) return 1;
    if (build_shadercross() != 0) return 1;
    if (build_tracy() != 0) return 1;
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
    printf("  all          Build everything (default)\n");
    printf("  tracy        Build TracyClient static library\n");
    printf("  sdl3         Build SDL3 DLL\n");
    printf("  spirvcross   Build SPIRV-Cross shared DLL\n");
    printf("  shadercross  Build SDL_shadercross DLL\n");
    printf("  externals    Build externals DLL\n");
    printf("  core         Build core DLL\n");
    printf("  engine       Build engine DLL\n");
    printf("  exe          Build AnitraEngine executable\n");
    printf("  clean        Delete build directory\n");
    printf("  help         Show this message\n");
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
    } else if (strcmp(target, "sdl3") == 0) {
        rc = build_sdl3();
    } else if (strcmp(target, "spirvcross") == 0) {
        rc = build_spirvcross();
    } else if (strcmp(target, "shadercross") == 0) {
        rc = build_shadercross();
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
