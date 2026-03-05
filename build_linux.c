/*
 * build_linux.c - Linux platform implementation for build.c
 *
 * Included by build.c on __linux__. Provides:
 *   - inotify include
 *   - TCC command defines
 *   - SDL3 platform source list
 *   - watch_and_rebuild() using inotify
 */

#include <sys/inotify.h>

/* ------- TCC commands --------------------------------------------------- */

#define TCC_COMPILE_CMD \
    "./tcc -Blib/tcc-linux -shared" \
    " -o build/Debug/engine.so" \
    " -Isrc -Isrc/engine -Isrc/editor -Ilib/SDL3/include -Ilib/cgltf" \
    " src/engine/*.c"

#define TCC_EDITOR_CMD \
    "./tcc -Blib/tcc-linux -shared" \
    " -o build/Debug/editor.so" \
    " -DCLAY_DISABLE_SIMD" \
    " -Isrc -Isrc/editor -Isrc/engine -Isrc/collab" \
    " -Ilib/SDL3/include -Ilib/clay" \
    " src/editor/editor.c" \
    " src/collab/collab_ops.c" \
    " src/collab/collab_client.c"

#define TCC_CORE_CMD \
    "./tcc -Blib/tcc-linux -shared" \
    " -o build/Debug/core.so" \
    " -Isrc -Isrc/core -Isrc/engine -Isrc/editor -Isrc/externals" \
    " -Ilib/SDL3/include" \
    " src/core/core.c" \
    " src/core/loadlibrary_linux.cpp"

#define TCC_EXE_CMD \
    "./tcc -Blib/tcc-linux" \
    " -o build/Debug/AnitraEngine" \
    " -Isrc -Isrc/core -Isrc/engine -Isrc/editor -Isrc/externals" \
    " -Ilib/SDL3/include" \
    " src/main.c" \
    " src/core/loadlibrary_linux.cpp" \
    " -ldl"

#define TCC_TEST_CMD \
    "./tcc -Blib/tcc-linux" \
    " -o build/Debug/test_dock" \
    " -Isrc/editor" \
    " tests/test_dock.c"

#define TCC_TEST_INSPECTOR_CMD \
    "./tcc -Blib/tcc-linux" \
    " -o build/Debug/test_inspector" \
    " -Isrc -Isrc/editor" \
    " tests/test_inspector.c"

#define TCC_TEST_HOTRELOAD_CMD \
    "./tcc -Blib/tcc-linux" \
    " -o build/Debug/test_hotreload" \
    " -DCLAY_DISABLE_SIMD" \
    " -Isrc -Isrc/editor -Ilib/clay" \
    " tests/test_hotreload.c"

/* ------- SDL3 platform sources ------------------------------------------ */

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

    /* HID — linux/hid.c is #included by SDL_hidapi.c via SDL_hidapi_linux.h */

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

/* ------- watch (forge) — Linux inotify ---------------------------------- */

static int watch_and_rebuild(void)
{
    int ifd, engine_wfd, editor_wfd, core_wfd, externals_wfd;
    struct pollfd pfd;
    char buf[4096];

    printf("=== Forge: watching src/engine + src/editor + src/core + src/externals for changes ===\n");
    fflush(stdout);

    ifd = inotify_init();
    if (ifd < 0) {
        printf("!! inotify_init failed\n");
        return 1;
    }

    engine_wfd = inotify_add_watch(ifd, "src/engine",
        IN_MODIFY | IN_CREATE | IN_MOVED_TO);
    if (engine_wfd < 0) {
        printf("!! inotify_add_watch failed on src/engine\n");
        close(ifd);
        return 1;
    }

    editor_wfd = inotify_add_watch(ifd, "src/editor",
        IN_MODIFY | IN_CREATE | IN_MOVED_TO);
    if (editor_wfd < 0) {
        printf("!! inotify_add_watch failed on src/editor\n");
        close(ifd);
        return 1;
    }

    core_wfd = inotify_add_watch(ifd, "src/core",
        IN_MODIFY | IN_CREATE | IN_MOVED_TO);
    if (core_wfd < 0) {
        printf("!! inotify_add_watch failed on src/core\n");
        close(ifd);
        return 1;
    }

    externals_wfd = inotify_add_watch(ifd, "src/externals",
        IN_MODIFY | IN_CREATE | IN_MOVED_TO);
    if (externals_wfd < 0) {
        printf("!! inotify_add_watch failed on src/externals\n");
        close(ifd);
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

    /* Watch loop */
    pfd.fd = ifd;
    pfd.events = POLLIN;

    while (1) {
        int ret = poll(&pfd, 1, -1);
        if (ret < 0) {
            printf("!! poll failed\n");
            break;
        }

        /* Drain inotify events — track which directories changed */
        int engine_changed = 0, editor_changed = 0, core_changed = 0, externals_changed = 0;
        {
            ssize_t n = read(ifd, buf, sizeof(buf));
            ssize_t i = 0;
            while (i < n) {
                struct inotify_event *ev = (struct inotify_event *)(buf + i);
                if (ev->wd == engine_wfd) engine_changed = 1;
                if (ev->wd == editor_wfd) editor_changed = 1;
                if (ev->wd == core_wfd)   core_changed = 1;
                if (ev->wd == externals_wfd) externals_changed = 1;
                i += sizeof(struct inotify_event) + ev->len;
            }
        }

        /* Debounce: wait 50ms and drain again */
        usleep(50000);
        {
            struct pollfd drain_pfd;
            drain_pfd.fd = ifd;
            drain_pfd.events = POLLIN;
            while (poll(&drain_pfd, 1, 0) > 0) {
                ssize_t n = read(ifd, buf, sizeof(buf));
                ssize_t i = 0;
                while (i < n) {
                    struct inotify_event *ev = (struct inotify_event *)(buf + i);
                    if (ev->wd == engine_wfd) engine_changed = 1;
                    if (ev->wd == editor_wfd) editor_changed = 1;
                    if (ev->wd == core_wfd)   core_changed = 1;
                    if (ev->wd == externals_wfd) externals_changed = 1;
                    i += sizeof(struct inotify_event) + ev->len;
                }
            }
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

    close(ifd);
    return 0;
}
