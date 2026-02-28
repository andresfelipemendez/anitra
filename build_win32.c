/*
 * build_win32.c - Windows platform implementation for build.c
 *
 * Included by build.c on _WIN32. Provides:
 *   - Hot-reload event name defines
 *   - TCC command defines
 *   - SDL3 platform source list
 *   - watch_and_rebuild() using ReadDirectoryChangesW
 */

/* ------- hot-reload event names ----------------------------------------- */

#define HOTRELOAD_EVENT_NAME        "Global\\ReloadEvent"
#define HOTRELOAD_EDITOR_EVENT_NAME "Global\\ReloadEditorEvent"
#define HOTRELOAD_CORE_EVENT_NAME   "Global\\ReloadCoreEvent"

/* ------- TCC commands --------------------------------------------------- */

#define TCC_COMPILE_CMD \
    ".\\tcc.exe -Blib/tcc-windows -shared" \
    " -o build/Debug/engine.dll" \
    " -Isrc -Isrc/engine -Isrc/editor -Ilib/SDL3/include -Ilib/cgltf" \
    " src/engine/engine.c" \
    " src/engine/renderer.c" \
    " src/engine/physics.c" \
    " src/engine/scene.c" \
    " src/engine/debug_render.c" \
    " src/engine/anim.c" \
    " src/engine/gltf_loader.c" \
    " build/Debug/SDL3.def"
#define TCC_EDITOR_CMD \
    ".\\tcc.exe -Blib/tcc-windows -shared" \
    " -o build/Debug/editor.dll" \
    " -DCLAY_DISABLE_SIMD" \
    " -Isrc -Isrc/editor -Isrc/engine" \
    " -Ilib/SDL3/include -Ilib/clay" \
    " src/editor/editor.c" \
    " build/Debug/SDL3.def"

#define TCC_CORE_CMD \
    ".\\tcc.exe -Blib/tcc-windows -shared" \
    " -o build/Debug/core.dll" \
    " -Isrc -Isrc/core -Isrc/engine -Isrc/editor -Isrc/externals" \
    " -Ilib/SDL3/include -Ilib/toml-c" \
    " src/core/core.c" \
    " src/core/loadlibrary_windows.c" \
    " src/project.c" \
    " build/Debug/SDL3.def" \
    " build/Debug/externals.def"

#define TCC_EXE_CMD \
    ".\\tcc.exe -Blib/tcc-windows" \
    " -o build/Debug/AnitraEngine.exe" \
    " -Isrc -Isrc/core -Isrc/engine -Isrc/editor -Isrc/externals" \
    " -Ilib/SDL3/include" \
    " src/main.c" \
    " src/core/loadlibrary_windows.c"

#define TCC_TEST_CMD \
    ".\\tcc.exe -Blib/tcc-windows" \
    " -o build/Debug/test_dock.exe" \
    " -Isrc/editor" \
    " tests/test_dock.c"

/* ------- SDL3 platform sources ------------------------------------------ */

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

    /* HID — windows/hid.c is #included by SDL_hidapi.c via SDL_hidapi_windows.h */

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

/* ------- watch (forge) — Windows ReadDirectoryChangesW ------------------ */

static HANDLE g_engine_process = NULL;

static int watch_and_rebuild(void)
{
    HANDLE hEngineDir, hEditorDir, hCoreDir;
    HANDLE hEngineEvent, hEditorEvent, hCoreEvent;
    char engine_buf[4096], editor_buf[4096], core_buf[4096];
    OVERLAPPED engine_ov = {0}, editor_ov = {0}, core_ov = {0};
    DWORD bytes;

    printf("=== Forge: watching src/engine + src/editor + src/core for changes ===\n");
    fflush(stdout);

    /* Open directory handles for watching */
    hEngineDir = CreateFileA(
        "src/engine", FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
    if (hEngineDir == INVALID_HANDLE_VALUE) {
        printf("!! Failed to open src/engine for watching (error %lu)\n", GetLastError());
        return 1;
    }

    hEditorDir = CreateFileA(
        "src/editor", FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
    if (hEditorDir == INVALID_HANDLE_VALUE) {
        printf("!! Failed to open src/editor for watching (error %lu)\n", GetLastError());
        CloseHandle(hEngineDir);
        return 1;
    }

    hCoreDir = CreateFileA(
        "src/core", FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
    if (hCoreDir == INVALID_HANDLE_VALUE) {
        printf("!! Failed to open src/core for watching (error %lu)\n", GetLastError());
        CloseHandle(hEngineDir);
        CloseHandle(hEditorDir);
        return 1;
    }

    engine_ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    editor_ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    core_ov.hEvent   = CreateEventA(NULL, TRUE, FALSE, NULL);

    /* Named events for signaling the running application */
    hEngineEvent = CreateEventA(NULL, TRUE, FALSE, HOTRELOAD_EVENT_NAME);
    hEditorEvent = CreateEventA(NULL, TRUE, FALSE, HOTRELOAD_EDITOR_EVENT_NAME);
    hCoreEvent   = CreateEventA(NULL, TRUE, FALSE, HOTRELOAD_CORE_EVENT_NAME);
    if (!hEngineEvent || !hEditorEvent || !hCoreEvent) {
        printf("!! Failed to create reload events (error %lu)\n", GetLastError());
        return 1;
    }

    if (ensure_dirs() != 0) return 1;

    /* Initial compiles (app handles copying to _copy on load) */
    printf(">> " TCC_COMPILE_CMD "\n");
    fflush(stdout);
    if (system(TCC_COMPILE_CMD) == 0) {
        printf("   Initial engine compile OK.\n");
    } else {
        printf("!! Initial engine compile failed.\n");
    }

    printf(">> " TCC_EDITOR_CMD "\n");
    fflush(stdout);
    if (system(TCC_EDITOR_CMD) == 0) {
        printf("   Initial editor compile OK.\n");
    } else {
        printf("!! Initial editor compile failed.\n");
    }

    printf(">> " TCC_CORE_CMD "\n");
    fflush(stdout);
    if (system(TCC_CORE_CMD) == 0) {
        printf("   Initial core compile OK.\n");
    } else {
        printf("!! Initial core compile failed.\n");
    }

    /* Watch loop: wait on directory changes or engine process exit */
    while (1) {
        HANDLE waitHandles[4];
        DWORD handleCount = 3;
        DWORD waitResult;

        ReadDirectoryChangesW(hEngineDir, engine_buf, sizeof(engine_buf), TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE,
            &bytes, &engine_ov, NULL);
        ReadDirectoryChangesW(hEditorDir, editor_buf, sizeof(editor_buf), TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE,
            &bytes, &editor_ov, NULL);
        ReadDirectoryChangesW(hCoreDir, core_buf, sizeof(core_buf), TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE,
            &bytes, &core_ov, NULL);

        waitHandles[0] = engine_ov.hEvent;
        waitHandles[1] = editor_ov.hEvent;
        waitHandles[2] = core_ov.hEvent;
        if (g_engine_process) {
            waitHandles[3] = g_engine_process;
            handleCount = 4;
        }
        waitResult = WaitForMultipleObjects(handleCount, waitHandles, FALSE, INFINITE);

        /* Engine process exited */
        if (g_engine_process && waitResult == WAIT_OBJECT_0 + 3) {
            printf("\n--- Engine exited, stopping watch. ---\n");
            fflush(stdout);
            break;
        }

        /* Debounce */
        Sleep(50);
        CancelIo(hEngineDir);
        CancelIo(hEditorDir);
        CancelIo(hCoreDir);
        ResetEvent(engine_ov.hEvent);
        ResetEvent(editor_ov.hEvent);
        ResetEvent(core_ov.hEvent);

        if (waitResult == WAIT_OBJECT_0) {
            /* Engine directory changed */
            printf("\n--- Engine change detected, recompiling... ---\n");
            fflush(stdout);
            printf(">> " TCC_COMPILE_CMD "\n");
            fflush(stdout);
            if (system(TCC_COMPILE_CMD) == 0) {
                printf("   Compile OK. Signaling engine reload...\n");
                SetEvent(hEngineEvent);
                ResetEvent(hEngineEvent);
            } else {
                printf("!! Engine compile failed.\n");
            }
        } else if (waitResult == WAIT_OBJECT_0 + 1) {
            /* Editor directory changed */
            printf("\n--- Editor change detected, recompiling... ---\n");
            fflush(stdout);
            printf(">> " TCC_EDITOR_CMD "\n");
            fflush(stdout);
            if (system(TCC_EDITOR_CMD) == 0) {
                printf("   Compile OK. Signaling editor reload...\n");
                SetEvent(hEditorEvent);
                ResetEvent(hEditorEvent);
            } else {
                printf("!! Editor compile failed.\n");
            }
        } else if (waitResult == WAIT_OBJECT_0 + 2) {
            /* Core directory changed */
            printf("\n--- Core change detected, recompiling... ---\n");
            fflush(stdout);
            printf(">> " TCC_CORE_CMD "\n");
            fflush(stdout);
            if (system(TCC_CORE_CMD) == 0) {
                printf("   Compile OK. Signaling core reload...\n");
                SetEvent(hCoreEvent);
                ResetEvent(hCoreEvent);
            } else {
                printf("!! Core compile failed.\n");
            }
        }
        fflush(stdout);
    }

    CloseHandle(hEngineEvent);
    CloseHandle(hEditorEvent);
    CloseHandle(hCoreEvent);
    CloseHandle(engine_ov.hEvent);
    CloseHandle(editor_ov.hEvent);
    CloseHandle(core_ov.hEvent);
    CloseHandle(hEngineDir);
    CloseHandle(hEditorDir);
    CloseHandle(hCoreDir);
    return 0;
}
