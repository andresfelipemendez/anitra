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
    " -DCLAY_DISABLE_SIMD" \
    " -Isrc -Isrc/editor -Isrc/engine" \
    " -Ilib/SDL3/include -Ilib/clay" \
    " src/editor/editor.c"

#define TCC_CORE_CMD \
    "lib/tcc/macos/tcc -Blib/tcc/macos -shared" \
    TCC_MACOS_DEFS \
    " -o build/Debug/libcore.dylib" \
    " -Isrc -Isrc/core -Isrc/engine -Isrc/editor -Isrc/externals" \
    " -Ilib/SDL3/include -Ilib/toml-c" \
    " src/core/core.c" \
    " src/core/loadlibrary_macos.c" \
    " src/project.c" \
    " -Lbuild/Debug -lexternals"

#define TCC_EXE_CMD \
    "lib/tcc/macos/tcc -Blib/tcc/macos" \
    TCC_MACOS_DEFS \
    " -o build/Debug/AnitraEngine" \
    " -Isrc -Isrc/core -Isrc/engine -Isrc/editor -Isrc/externals" \
    " -Ilib/SDL3/include" \
    " src/main.c" \
    " src/core/loadlibrary_macos.c"

#define TCC_TEST_CMD \
    "lib/tcc/macos/tcc -Blib/tcc/macos" \
    " -o build/Debug/test_dock" \
    " -Isrc/editor" \
    " tests/test_dock.c"

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
    int kq, engine_fd, editor_fd, core_fd, externals_fd;
    struct kevent changes[4];
    struct kevent events[4];
    time_t engine_mtime = 0, editor_mtime = 0, core_mtime = 0, externals_mtime = 0;

    printf("=== Forge: watching src/engine + src/editor + src/core + src/externals for changes ===\n");
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

    externals_fd = open("src/externals", O_RDONLY | O_DIRECTORY);
    if (externals_fd < 0) {
        printf("!! failed to open src/externals for watching\n");
        close(core_fd);
        close(editor_fd);
        close(engine_fd);
        close(kq);
        return 1;
    }

    EV_SET(&changes[0], engine_fd, EVFILT_VNODE,
           EV_ADD | EV_CLEAR, NOTE_WRITE | NOTE_RENAME | NOTE_DELETE, 0, NULL);
    EV_SET(&changes[1], editor_fd, EVFILT_VNODE,
           EV_ADD | EV_CLEAR, NOTE_WRITE | NOTE_RENAME | NOTE_DELETE, 0, NULL);
    EV_SET(&changes[2], core_fd, EVFILT_VNODE,
           EV_ADD | EV_CLEAR, NOTE_WRITE | NOTE_RENAME | NOTE_DELETE, 0, NULL);
    EV_SET(&changes[3], externals_fd, EVFILT_VNODE,
           EV_ADD | EV_CLEAR, NOTE_WRITE | NOTE_RENAME | NOTE_DELETE, 0, NULL);

    if (kevent(kq, changes, 4, NULL, 0, NULL) < 0) {
        printf("!! kevent registration failed\n");
        close(externals_fd);
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
    externals_mtime = newest_mtime_recursive("src/externals");

    /* Watch loop */
    while (1) {
        int nev, i;
        int engine_changed = 0, editor_changed = 0, core_changed = 0, externals_changed = 0;
        struct timespec timeout;

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
            if ((int)events[i].ident == externals_fd) externals_changed = 1;
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
                    if ((int)events[i].ident == externals_fd) externals_changed = 1;
                }
            }
        }

        /* Poll fallback: catches in-place writes that may not fire vnode events on dirs. */
        {
            time_t now_engine = newest_mtime_recursive("src/engine");
            time_t now_editor = newest_mtime_recursive("src/editor");
            time_t now_core = newest_mtime_recursive("src/core");
            time_t now_externals = newest_mtime_recursive("src/externals");

            if (now_engine > engine_mtime) { engine_changed = 1; engine_mtime = now_engine; }
            if (now_editor > editor_mtime) { editor_changed = 1; editor_mtime = now_editor; }
            if (now_core > core_mtime) { core_changed = 1; core_mtime = now_core; }
            if (now_externals > externals_mtime) { externals_changed = 1; externals_mtime = now_externals; }
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

        if (externals_changed) {
            printf("\n--- Externals change detected, recompiling... ---\n");
            fflush(stdout);
            if (build_externals() == 0) {
                fclose(fopen(DEBUG_DIR "/.core-reload-signal", "w"));
                printf("   Compile OK. Externals rebuilt. Core reload signal written.\n");
            } else {
                printf("!! Externals compile failed.\n");
            }
        }
        fflush(stdout);
    }

    close(externals_fd);
    close(core_fd);
    close(editor_fd);
    close(engine_fd);
    close(kq);
    return 0;
}
