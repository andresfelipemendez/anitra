/*
 * build_macos.c - macOS platform implementation for build.c
 *
 * Included by build.c on __APPLE__. Provides:
 *   - kqueue include
 *   - TCC command defines
 *   - SDL3 platform source list (Cocoa, Metal, CoreAudio, etc.)
 *   - watch_and_rebuild() using kqueue
 */

#include <sys/event.h>
#include <dirent.h>

/* Forward declaration */
static int watch_and_rebuild(void);

/* ------- TCC commands --------------------------------------------------- */

#define TCC_MACOS_DEFS " -DMAC_OS_X_VERSION_MIN_REQUIRED=1100"

#define TCC_COMPILE_CMD \
    "lib/tcc/macos/tcc -Blib/tcc/macos -shared" \
    TCC_MACOS_DEFS \
    " -DSTBI_NO_THREAD_LOCALS" \
    " -o build/Debug/libengine.dylib" \
    " -Isrc -Isrc/engine -Isrc/editor -Ilib/SDL3/include -Ilib/cgltf" \
    " src/engine/*.c"

#define TCC_EDITOR_CMD \
    "lib/tcc/macos/tcc -Blib/tcc/macos -shared" \
    TCC_MACOS_DEFS \
    " -o build/Debug/libeditor.dylib" \
    " -DCLAY_DISABLE_SIMD -DCPU_PROF_USE_FPTRS" \
    " -Isrc -Isrc/editor -Isrc/engine -Isrc/collab" \
    " -Ilib/SDL3/include -Ilib/clay" \
    " src/editor/editor.c" \
    " src/collab/collab_ops.c" \
    " src/collab/collab_client.c"

#define TCC_CORE_CMD \
    "lib/tcc/macos/tcc -Blib/tcc/macos -shared" \
    TCC_MACOS_DEFS \
    " -DSTBI_NO_THREAD_LOCALS -DCLAY_DISABLE_SIMD" \
    " -o build/Debug/libcore.dylib" \
    " -DCPU_PROF_USE_TRACY -DTRACY_ENABLE -DTRACY_ON_DEMAND" \
    " -Isrc -Isrc/core -Isrc/engine -Isrc/editor" \
    " -Ilib/SDL3/include -Ilib/SDL_shadercross/include" \
    " -Ilib/tracy/public -Ilib/harfbuzz-src/src -Ilib/clay -Ilib/cgltf" \
    " -Ilib/sqlite -Ilib/toml-c -Ilib/nanoprof" \
    " src/core/core.c" \
    " src/core/loadlibrary_macos.c" \
    " src/core/externals_runtime.c src/core/externals.c" \
    " src/project.c lib/sqlite/sqlite3.c" \
    " -L" DEBUG_DIR " -lSDL3 -lSDL3_shadercross -lharfbuzz -lTracyClient" \
    " -lpthread -lm"

#define TCC_EXE_CMD \
    "lib/tcc/macos/tcc -Blib/tcc/macos" \
    TCC_MACOS_DEFS \
    " -o build/Debug/AnitraEngine" \
    " -Isrc -Isrc/core -Isrc/engine -Isrc/editor" \
    " -Ilib/SDL3/include" \
    " src/main.c" \
    " src/core/loadlibrary_macos.c"

#define TCC_TEST_CMD \
    "lib/tcc/macos/tcc -Blib/tcc/macos" \
    " -o build/Debug/test_dock" \
    " -Isrc/editor" \
    " tests/test_dock.c"

#define TCC_TEST_INSPECTOR_CMD \
    "lib/tcc/macos/tcc -Blib/tcc/macos" \
    " -o build/Debug/test_inspector" \
    " -Isrc -Isrc/editor" \
    " tests/test_inspector.c"

#define TCC_TEST_HOTRELOAD_CMD \
    "lib/tcc/macos/tcc -Blib/tcc/macos" \
    " -o build/Debug/test_hotreload" \
    " -DCLAY_DISABLE_SIMD" \
    " -Isrc -Isrc/editor -Ilib/clay" \
    " tests/test_hotreload.c"

#define TCC_TEST_COLLAB_OT_CMD \
    "lib/tcc/macos/tcc -Blib/tcc/macos" \
    " -o build/Debug/test_collab_ot" \
    " -Isrc -Isrc/collab" \
    " tests/test_collab_ot.c"

/* ------- SDL3 platform sources ------------------------------------------ */

static const char *sdl3_sources_platform[] = {
    /* Audio */
    "lib/SDL3/src/audio/coreaudio/SDL_coreaudio.m",
    "lib/SDL3/src/audio/dummy/SDL_dummyaudio.c",
    "lib/SDL3/src/audio/disk/SDL_diskaudio.c",

    /* Camera */
    "lib/SDL3/src/camera/coremedia/SDL_camera_coremedia.m",
    "lib/SDL3/src/camera/dummy/SDL_camera_dummy.c",

    /* Core */
    "lib/SDL3/src/core/unix/SDL_appid.c",
    "lib/SDL3/src/core/unix/SDL_poll.c",

    /* Dialog */
    "lib/SDL3/src/dialog/cocoa/SDL_cocoadialog.m",

    /* Filesystem */
    "lib/SDL3/src/filesystem/cocoa/SDL_sysfilesystem.m",
    "lib/SDL3/src/filesystem/posix/SDL_sysfsops.c",

    /* GPU */
    "lib/SDL3/src/gpu/metal/SDL_gpu_metal.m",

    /* Haptic */
    "lib/SDL3/src/haptic/darwin/SDL_syshaptic.c",
    "lib/SDL3/src/haptic/hidapi/SDL_hidapihaptic.c",
    "lib/SDL3/src/haptic/hidapi/SDL_hidapihaptic_lg4ff.c",

    /* HID — mac/hid.c is #included by SDL_hidapi.c via SDL_hidapi_mac.h */

    /* I/O */
    "lib/SDL3/src/io/generic/SDL_asyncio_generic.c",

    /* Joystick */
    "lib/SDL3/src/joystick/darwin/SDL_iokitjoystick.c",
    "lib/SDL3/src/joystick/apple/SDL_mfijoystick.m",
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
    "lib/SDL3/src/locale/macos/SDL_syslocale.m",

    /* Main */
    "lib/SDL3/src/main/generic/SDL_sysmain_callbacks.c",

    /* Misc */
    "lib/SDL3/src/misc/macos/SDL_sysurl.m",

    /* Power */
    "lib/SDL3/src/power/macos/SDL_syspower.c",

    /* Process */
    "lib/SDL3/src/process/posix/SDL_posixprocess.c",

    /* Render */
    "lib/SDL3/src/render/metal/SDL_render_metal.m",

    /* Sensor */
    "lib/SDL3/src/sensor/dummy/SDL_dummysensor.c",

    /* Storage */
    "lib/SDL3/src/storage/generic/SDL_genericstorage.c",
    "lib/SDL3/src/storage/steam/SDL_steamstorage.c",

    /* Thread (pthread — same as Linux) */
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
    "lib/SDL3/src/tray/cocoa/SDL_tray.m",

    /* Video - Cocoa */
    "lib/SDL3/src/video/cocoa/SDL_cocoaclipboard.m",
    "lib/SDL3/src/video/cocoa/SDL_cocoaevents.m",
    "lib/SDL3/src/video/cocoa/SDL_cocoakeyboard.m",
    "lib/SDL3/src/video/cocoa/SDL_cocoamessagebox.m",
    "lib/SDL3/src/video/cocoa/SDL_cocoametalview.m",
    "lib/SDL3/src/video/cocoa/SDL_cocoamodes.m",
    "lib/SDL3/src/video/cocoa/SDL_cocoamouse.m",
    "lib/SDL3/src/video/cocoa/SDL_cocoaopengl.m",
    "lib/SDL3/src/video/cocoa/SDL_cocoaopengles.m",
    "lib/SDL3/src/video/cocoa/SDL_cocoapen.m",
    "lib/SDL3/src/video/cocoa/SDL_cocoashape.m",
    "lib/SDL3/src/video/cocoa/SDL_cocoavideo.m",
    "lib/SDL3/src/video/cocoa/SDL_cocoavulkan.m",
    "lib/SDL3/src/video/cocoa/SDL_cocoawindow.m"
};

/* ------- test (compile and run unit tests) ------------------------------- */

static int build_test(void)
{
    int failed = 0;
    printf("\n=== Building tests ===\n");
    if (ensure_dirs() != 0) return 1;

    printf(">> " TCC_TEST_CMD "\n");
    fflush(stdout);
    if (system(TCC_TEST_CMD) != 0) {
        printf("!! test_dock build failed.\n");
        return 1;
    }

    printf(">> " TCC_TEST_INSPECTOR_CMD "\n");
    fflush(stdout);
    if (system(TCC_TEST_INSPECTOR_CMD) != 0) {
        printf("!! test_inspector build failed.\n");
        return 1;
    }

    printf(">> " TCC_TEST_HOTRELOAD_CMD "\n");
    fflush(stdout);
    if (system(TCC_TEST_HOTRELOAD_CMD) != 0) {
        printf("!! test_hotreload build failed.\n");
        return 1;
    }

    printf(">> " TCC_TEST_COLLAB_OT_CMD "\n");
    fflush(stdout);
    if (system(TCC_TEST_COLLAB_OT_CMD) != 0) {
        printf("!! test_collab_ot build failed.\n");
        return 1;
    }

    printf("\n=== Running tests ===\n");
    fflush(stdout);
    if (system("build/Debug/test_dock") != 0) {
        printf("!! test_dock failed.\n");
        failed = 1;
    }
    if (system("build/Debug/test_inspector") != 0) {
        printf("!! test_inspector failed.\n");
        failed = 1;
    }
    if (system("build/Debug/test_hotreload") != 0) {
        printf("!! test_hotreload failed.\n");
        failed = 1;
    }
    if (system("build/Debug/test_collab_ot") != 0) {
        printf("!! test_collab_ot failed.\n");
        failed = 1;
    }

    if (failed) {
        printf("!! some tests failed.\n");
        return 1;
    }
    return 0;
}

/* ------- run (build + launch + watch) ----------------------------------- */

static pid_t g_engine_pid;

static int build_and_run(void)
{
    char project_path[PATH_SIZE];
    const char *project_include;
    pid_t pid;

    printf("=== Building and running engine ===\n\n");
    if (build_all() != 0) return 1;

    project_include = resolve_project(project_path, sizeof(project_path));
    if (project_include) {
        printf("   Using project include: %s\n", project_include);
    } else {
        printf("   No project include found (checked %s and %s)\n",
               PROJECT_INCLUDE_FILE, DEFAULT_PROJECT_TOML);
    }

    printf("\n=== Launching engine and starting watch mode ===\n");

    pid = fork();
    if (pid < 0) {
        printf("!! Failed to fork\n");
        return 1;
    }

    if (pid == 0) {
        char engine_path[PATH_SIZE];
        snprintf(engine_path, PATH_SIZE, "%s/AnitraEngine", DEBUG_DIR);
        if (project_include) {
            execl(engine_path, "AnitraEngine", project_include, NULL);
        } else {
            execl(engine_path, "AnitraEngine", NULL);
        }
        printf("!! Failed to exec engine\n");
        exit(1);
    }

    printf("   Engine launched (PID: %d)\n", pid);
    g_engine_pid = pid;
    return watch_and_rebuild();
}

/* ------- play_test (build + launch gym scene, no watch) ----------------- */

static int build_play_test(void)
{
    pid_t pid;

    printf("=== Building play test ===\n\n");
    if (build_all() != 0) return 1;

    printf("\n=== Launching gym scene ===\n");

    pid = fork();
    if (pid < 0) { printf("!! Failed to fork\n"); return 1; }
    if (pid == 0) {
        char engine_path[PATH_SIZE];
        snprintf(engine_path, PATH_SIZE, "%s/AnitraEngine", DEBUG_DIR);
        execl(engine_path, "AnitraEngine", GYM_SCENE_TOML, NULL);
        printf("!! Failed to exec engine\n");
        _exit(1);
    }
    printf("   Engine launched (PID: %d) — press Play in the editor to start\n", pid);
    return 0;
}

/* ------- collab (server + 2 editors + watch) ----------------------------- */

static int build_collab(void)
{
    char project_path[PATH_SIZE];
    const char *project_include;
    pid_t server_pid, editor2_pid;
    char engine_path[PATH_SIZE];
    char server_path[PATH_SIZE];

    printf("=== Building collab (server + 2 editors) ===\n\n");
    if (build_all() != 0) return 1;
    if (build_server() != 0) return 1;

    project_include = resolve_project(project_path, sizeof(project_path));

    printf("\n=== Launching collab server + 2 editors ===\n");
    setenv("ANITRA_COLLAB", "1", 1);

    snprintf(engine_path, PATH_SIZE, "%s/AnitraEngine", DEBUG_DIR);
    snprintf(server_path, PATH_SIZE, "%s/collab_server", DEBUG_DIR);

    /* 1. Fork collab server */
    server_pid = fork();
    if (server_pid < 0) { printf("!! Failed to fork server\n"); return 1; }
    if (server_pid == 0) {
        if (project_include)
            execl(server_path, "collab_server",
                  "--port", "7777", "--project", project_include, NULL);
        else
            execl(server_path, "collab_server", "--port", "7777", NULL);
        printf("!! Failed to exec server\n");
        exit(1);
    }
    printf("   Server launched (PID: %d)\n", server_pid);
    usleep(500000);

    /* 2. Fork editor 1 */
    {
        pid_t pid = fork();
        if (pid < 0) { printf("!! Failed to fork editor 1\n");
                       kill(server_pid, SIGTERM); return 1; }
        if (pid == 0) {
            if (project_include)
                execl(engine_path, "AnitraEngine", project_include, NULL);
            else
                execl(engine_path, "AnitraEngine", NULL);
            printf("!! Failed to exec editor 1\n");
            exit(1);
        }
        printf("   Editor 1 launched (PID: %d)\n", pid);
        g_engine_pid = pid;
    }

    /* 3. Fork editor 2 */
    editor2_pid = fork();
    if (editor2_pid < 0) { printf("!! Failed to fork editor 2\n");
                           kill(server_pid, SIGTERM); return 1; }
    if (editor2_pid == 0) {
        if (project_include)
            execl(engine_path, "AnitraEngine", project_include, NULL);
        else
            execl(engine_path, "AnitraEngine", NULL);
        printf("!! Failed to exec editor 2\n");
        exit(1);
    }
    printf("   Editor 2 launched (PID: %d)\n", editor2_pid);

    {
        int rc = watch_and_rebuild();
        printf("\n--- Stopping collab session... ---\n");
        kill(server_pid, SIGTERM);
        kill(editor2_pid, SIGTERM);
        return rc;
    }
}

/* ------- watch (forge) — macOS kqueue ----------------------------------- */

static time_t newest_mtime_recursive(const char *path)
{
    struct stat st;
    time_t newest = 0;
    DIR *dir;
    struct dirent *de;

    if (stat(path, &st) != 0) return 0;
    newest = st.st_mtime;
    if (!S_ISDIR(st.st_mode)) return newest;

    dir = opendir(path);
    if (!dir) return newest;

    while ((de = readdir(dir)) != NULL) {
        char child[PATH_SIZE];
        time_t child_newest;

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
        child_newest = newest_mtime_recursive(child);
        if (child_newest > newest) newest = child_newest;
    }

    closedir(dir);
    return newest;
}

static int watch_and_rebuild(void)
{
    int kq, engine_fd, editor_fd, core_fd, project_fd;
    struct kevent changes[4];
    struct kevent events[4];
    time_t engine_mtime = 0, editor_mtime = 0, core_mtime = 0;
    time_t project_mtime = 0;
    /* Extract project directory from DEFAULT_PROJECT_TOML (e.g. "dungeon1") */
    char project_dir[256];
    {
        const char *slash;
        strncpy(project_dir, DEFAULT_PROJECT_TOML, sizeof(project_dir) - 1);
        project_dir[sizeof(project_dir) - 1] = '\0';
        slash = strrchr(project_dir, '/');
        if (slash) project_dir[slash - project_dir] = '\0';
        else       project_dir[0] = '\0';
    }

    printf("=== Forge: watching src/engine + src/editor + src/core + %s for changes ===\n", project_dir);
    fflush(stdout);

    kq = kqueue();
    if (kq < 0) {
        printf("!! kqueue failed\n");
        return 1;
    }

    engine_fd = open("src/engine", O_RDONLY | O_DIRECTORY);
    if (engine_fd < 0) {
        printf("!! failed to open src/engine for watching\n");
        close(kq);
        return 1;
    }

    editor_fd = open("src/editor", O_RDONLY | O_DIRECTORY);
    if (editor_fd < 0) {
        printf("!! failed to open src/editor for watching\n");
        close(engine_fd);
        close(kq);
        return 1;
    }

    core_fd = open("src/core", O_RDONLY | O_DIRECTORY);
    if (core_fd < 0) {
        printf("!! failed to open src/core for watching\n");
        close(editor_fd);
        close(engine_fd);
        close(kq);
        return 1;
    }

    project_fd = project_dir[0] ? open(project_dir, O_RDONLY | O_DIRECTORY) : -1;
    if (project_fd < 0 && project_dir[0]) {
        printf("!! failed to open %s for watching (project reload disabled)\n", project_dir);
    }

    EV_SET(&changes[0], engine_fd, EVFILT_VNODE,
           EV_ADD | EV_CLEAR, NOTE_WRITE | NOTE_RENAME | NOTE_DELETE, 0, NULL);
    EV_SET(&changes[1], editor_fd, EVFILT_VNODE,
           EV_ADD | EV_CLEAR, NOTE_WRITE | NOTE_RENAME | NOTE_DELETE, 0, NULL);
    EV_SET(&changes[2], core_fd, EVFILT_VNODE,
           EV_ADD | EV_CLEAR, NOTE_WRITE | NOTE_RENAME | NOTE_DELETE, 0, NULL);
    if (project_fd >= 0) {
        EV_SET(&changes[3], project_fd, EVFILT_VNODE,
               EV_ADD | EV_CLEAR, NOTE_WRITE | NOTE_RENAME | NOTE_DELETE, 0, NULL);
    }

    if (kevent(kq, changes, project_fd >= 0 ? 4 : 3, NULL, 0, NULL) < 0) {
        printf("!! kevent registration failed\n");
        if (project_fd >= 0) close(project_fd);
        close(core_fd);
        close(editor_fd);
        close(engine_fd);
        close(kq);
        return 1;
    }

    /* Ensure build output directory exists */
    if (ensure_dirs() != 0) return 1;

    /* Initial compile — engine */
    printf(">> " TCC_COMPILE_CMD "\n");
    fflush(stdout);
    if (system(TCC_COMPILE_CMD) == 0) {
        printf("   Initial engine compile OK.\n");
    } else {
        printf("!! Initial engine compile failed. Waiting for changes...\n");
    }

    /* Initial compile — editor */
    printf(">> " TCC_EDITOR_CMD "\n");
    fflush(stdout);
    if (system(TCC_EDITOR_CMD) == 0) {
        printf("   Initial editor compile OK.\n");
    } else {
        printf("!! Initial editor compile failed. Waiting for changes...\n");
    }

    /* Initial compile — core */
    printf(">> " TCC_CORE_CMD "\n");
    fflush(stdout);
    if (system(TCC_CORE_CMD) == 0) {
        printf("   Initial core compile OK.\n");
    } else {
        printf("!! Initial core compile failed. Waiting for changes...\n");
    }

    /* Baseline source mtimes for polling fallback (kqueue on directories
       misses in-place file edits in some workflows/editors). */
    engine_mtime = newest_mtime_recursive("src/engine");
    editor_mtime = newest_mtime_recursive("src/editor");
    core_mtime = newest_mtime_recursive("src/core");
    if (project_dir[0]) project_mtime = newest_mtime_recursive(project_dir);

    /* Watch loop */
    while (1) {
        int nev, i;
        int engine_changed = 0, editor_changed = 0, core_changed = 0, project_changed = 0;
        struct timespec timeout;

        /* Check if the engine process has exited */
        if (g_engine_pid > 0) {
            int status;
            pid_t result = waitpid(g_engine_pid, &status, WNOHANG);
            if (result == g_engine_pid) {
                printf("\n--- Engine exited, stopping watch. ---\n");
                break;
            }
        }

        timeout.tv_sec = 0;
        timeout.tv_nsec = 200 * 1000 * 1000; /* 200ms poll fallback cadence */

        nev = kevent(kq, NULL, 0, events, 4, &timeout);
        if (nev < 0) {
            printf("!! kevent wait failed\n");
            break;
        }

        for (i = 0; i < nev; i++) {
            if ((int)events[i].ident == engine_fd) engine_changed = 1;
            if ((int)events[i].ident == editor_fd) editor_changed = 1;
            if ((int)events[i].ident == core_fd)   core_changed = 1;
            if (project_fd >= 0 && (int)events[i].ident == project_fd) project_changed = 1;
        }

        /* Debounce: wait 50ms and drain */
        usleep(50000);
        {
            struct timespec zero = {0, 0};
            int drained;
            while ((drained = kevent(kq, NULL, 0, events, 4, &zero)) > 0) {
                for (i = 0; i < drained; i++) {
                    if ((int)events[i].ident == engine_fd) engine_changed = 1;
                    if ((int)events[i].ident == editor_fd) editor_changed = 1;
                    if ((int)events[i].ident == core_fd)   core_changed = 1;
                    if (project_fd >= 0 && (int)events[i].ident == project_fd) project_changed = 1;
                }
            }
        }

        /* Poll fallback: catches in-place writes that may not fire vnode events on dirs. */
        {
            time_t now_engine = newest_mtime_recursive("src/engine");
            time_t now_editor = newest_mtime_recursive("src/editor");
            time_t now_core = newest_mtime_recursive("src/core");

            if (now_engine > engine_mtime) { engine_changed = 1; engine_mtime = now_engine; }
            if (now_editor > editor_mtime) { editor_changed = 1; editor_mtime = now_editor; }
            if (now_core > core_mtime) { core_changed = 1; core_mtime = now_core; }

            if (project_dir[0]) {
                time_t now_project = newest_mtime_recursive(project_dir);
                if (now_project > project_mtime) { project_changed = 1; project_mtime = now_project; }
            }
        }

        if (engine_changed || editor_changed || core_changed) {
            generate_migration_code();
        }

        if (engine_changed) {
            printf("\n--- Engine change detected, recompiling... ---\n");
            fflush(stdout);
            printf(">> " TCC_COMPILE_CMD "\n");
            fflush(stdout);
            if (system(TCC_COMPILE_CMD) == 0) {
                fclose(fopen(DEBUG_DIR "/.reload-signal", "w"));
                printf("   Compile OK. Engine reload signal written.\n");
            } else {
                printf("!! Engine compile failed.\n");
            }
        }

        if (editor_changed) {
            printf("\n--- Editor change detected, recompiling... ---\n");
            fflush(stdout);
            printf(">> " TCC_EDITOR_CMD "\n");
            fflush(stdout);
            if (system(TCC_EDITOR_CMD) == 0) {
                fclose(fopen(DEBUG_DIR "/.editor-reload-signal", "w"));
                printf("   Compile OK. Editor reload signal written.\n");
            } else {
                printf("!! Editor compile failed.\n");
            }
        }

        if (core_changed) {
            printf("\n--- Core change detected, recompiling... ---\n");
            fflush(stdout);
            printf(">> " TCC_CORE_CMD "\n");
            fflush(stdout);
            if (system(TCC_CORE_CMD) == 0) {
                fclose(fopen(DEBUG_DIR "/.core-reload-signal", "w"));
                printf("   Compile OK. Core reload signal written.\n");
            } else {
                printf("!! Core compile failed.\n");
            }
        }

        if (project_changed && !engine_changed) {
            printf("\n--- Project change detected, triggering scene reload... ---\n");
            fclose(fopen(DEBUG_DIR "/.reload-signal", "w"));
            printf("   Engine reload signal written.\n");
        }
        fflush(stdout);
    }

    if (project_fd >= 0) close(project_fd);
    close(core_fd);
    close(editor_fd);
    close(engine_fd);
    close(kq);
    return 0;
}
