/*
 * build_win32.c - Windows platform implementation for build.c
 *
 * Included by build.c on _WIN32. Provides:
 *   - TCC command defines
 *   - SDL3 platform source list
 *   - watch_and_rebuild() using ReadDirectoryChangesW
 */

/* Forward declaration */
static int watch_and_rebuild(void);

/* ------- TCC commands --------------------------------------------------- */

#define TCC_COMPILE_CMD \
    "tcc\\tcc.exe -Blib/tcc-windows -g -shared" \
    " -o build/Debug/engine.dll" \
    " -Isrc -Isrc/engine -Isrc/editor -Ilib/SDL3/include -Ilib/cgltf -Ilib/toml-c" \
    " src/engine/engine.c" \
    " src/engine/physics.c" \
    " src/engine/debug_render.c" \
    " src/engine/anim.c" \
    " src/engine/gltf_loader.c" \
    " src/project.c" \
    " build/Debug/SDL3.def"
#define TCC_EDITOR_CMD \
    "tcc\\tcc.exe -Blib/tcc-windows -g -shared" \
    " -o build/Debug/editor.dll" \
    " -DCLAY_DISABLE_SIMD -DCACHE_PROF_IMPL -DCPU_PROF_USE_FPTRS" \
    " -Isrc -Isrc/editor -Isrc/engine -Isrc/collab" \
    " -Ilib/SDL3/include -Ilib/clay -Ilib/toml-c -Ilib/sqlite" \
    " src/editor/editor.c" \
    " src/collab/collab_ops.c" \
    " src/collab/collab_client.c" \
    " src/project.c" \
    " lib/sqlite/sqlite3.c" \
    " build/Debug/SDL3.def" \
    " lib/tcc-windows/lib/ws2_32.def"

#define TCC_CORE_CMD \
    "tcc\\tcc.exe -Blib/tcc-windows -g -shared" \
    " -o build/Debug/core.dll" \
    " -DCPU_PROF_USE_TRACY -DTRACY_ENABLE -DTRACY_ON_DEMAND -DTRACY_IMPORTS" \
    " -DSTBI_NO_SIMD -DCLAY_DISABLE_SIMD" \
    " -Isrc -Isrc/core -Isrc/engine -Isrc/editor" \
    " -Ilib/SDL3/include -Ilib/SDL_shadercross/include" \
    " -Ilib/tracy/public -Ilib/harfbuzz-src/src -Ilib/clay -Ilib/cgltf" \
    " -Ilib/sqlite -Ilib/toml-c -Ilib/nanoprof" \
    " src/core/core.c" \
    " src/core/loadlibrary_windows.c" \
    " src/core/externals_runtime.c src/core/externals.c" \
    " src/project.c lib/sqlite/sqlite3.c" \
    " build/Debug/TracyClient.def" \
    " build/Debug/SDL3.def" \
    " build/Debug/SDL3_shadercross.def" \
    " build/Debug/harfbuzz.def" \
    " lib/tcc-windows/lib/ws2_32.def" \
    " lib/tcc-windows/lib/winmm.def" \
    " lib/tcc-windows/lib/kernel32.def"

#define TCC_EXE_CMD \
    "tcc\\tcc.exe -Blib/tcc-windows" \
    " -o build/Debug/AnitraEngine.exe" \
    " -Isrc -Isrc/core" \
    " src/main.c" \
    " src/core/loadlibrary_windows.c"

#define TCC_BUILDER_DLL_CMD \
    "tcc\\tcc.exe -Blib/tcc-windows -shared" \
    " -DBUILD_DLL" \
    " -o build/Debug/build.dll" \
    " src/builder/build.c"

/* MSVC cl commands — produce PDBs for raddbg debugging */
#define CL_EXE_CMD \
    "cl /nologo /Zi /Fe:build/Debug/AnitraEngine.exe /Fd:build/Debug/AnitraEngine.pdb" \
    " /Isrc /Isrc/core" \
    " src/main.c src/core/loadlibrary_windows.c" \
    " /link /DEBUG"

#define CL_ENGINE_CMD \
    "cl /nologo /Zi /LD /Fe:build/Debug/engine.dll /Fd:build/Debug/engine.pdb" \
    " /Isrc /Isrc/engine /Isrc/editor /Ilib/SDL3/include /Ilib/cgltf /Ilib/toml-c" \
    " src/engine/engine.c src/engine/physics.c src/engine/debug_render.c" \
    " src/engine/anim.c src/engine/gltf_loader.c src/project.c" \
    " /link /DEBUG build/Debug/SDL3.lib"

#define TCC_TEST_CMD \
    "tcc\\tcc.exe -Blib/tcc-windows -g" \
    " -o build/Debug/test_dock.exe" \
    " -Isrc -Isrc/editor" \
    " tests/test_dock.c"

#define TCC_TEST_INSPECTOR_CMD \
    "tcc\\tcc.exe -Blib/tcc-windows -g" \
    " -o build/Debug/test_inspector.exe" \
    " -Isrc -Isrc/editor" \
    " tests/test_inspector.c"

#define TCC_TEST_HOTRELOAD_CMD \
    "tcc\\tcc.exe -Blib/tcc-windows -g" \
    " -o build/Debug/test_hotreload.exe" \
    " -DCLAY_DISABLE_SIMD" \
    " -Isrc -Isrc/editor -Ilib/clay" \
    " tests/test_hotreload.c"

#define TCC_TEST_COLLAB_OT_CMD \
    "tcc\\tcc.exe -Blib/tcc-windows" \
    " -o build/Debug/test_collab_ot.exe" \
    " -Isrc -Isrc/collab" \
    " tests/test_collab_ot.c"

#define TCC_TEST_GYM_SCENE_CMD \
    "tcc\\tcc.exe -Blib/tcc-windows -g" \
    " -o build/Debug/test_gym_scene.exe" \
    " -Isrc -Isrc/engine -Isrc/editor" \
    " -Ilib/SDL3/include -Ilib/cgltf -Ilib/toml-c" \
    " tests/test_gym_scene.c"

/* test_editor.dll: real editor.c with SDL/externals stubs compiled in */
#define TCC_TEST_DLL_EDITOR_CMD \
    "tcc\\tcc.exe -Blib/tcc-windows -g -shared" \
    " -o build/Debug/test_editor.dll" \
    " -DCLAY_DISABLE_SIMD -DCACHE_PROF_IMPL -DCPU_PROF_USE_FPTRS" \
    " -Isrc -Isrc/editor -Isrc/engine -Isrc/collab" \
    " -Ilib/SDL3/include -Ilib/clay -Ilib/toml-c -Ilib/sqlite" \
    " src/editor/editor.c" \
    " tests/test_dll_stubs.c"

/* test_hotreload_dll.exe: exercises real DLL load/unload/reload */
#define TCC_TEST_DLL_CMD \
    "tcc\\tcc.exe -Blib/tcc-windows -g" \
    " -o build/Debug/test_hotreload_dll.exe" \
    " -Isrc -Isrc/editor -Isrc/engine" \
    " tests/test_hotreload_dll.c"

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

    printf(">> " TCC_TEST_DLL_EDITOR_CMD "\n");
    fflush(stdout);
    if (system(TCC_TEST_DLL_EDITOR_CMD) != 0) {
        printf("!! test_editor.dll build failed.\n");
        return 1;
    }

    printf(">> " TCC_TEST_DLL_CMD "\n");
    fflush(stdout);
    if (system(TCC_TEST_DLL_CMD) != 0) {
        printf("!! test_hotreload_dll build failed.\n");
        return 1;
    }

    printf(">> " TCC_TEST_COLLAB_OT_CMD "\n");
    fflush(stdout);
    if (system(TCC_TEST_COLLAB_OT_CMD) != 0) {
        printf("!! test_collab_ot build failed.\n");
        return 1;
    }

    printf(">> " TCC_TEST_GYM_SCENE_CMD "\n");
    fflush(stdout);
    if (system(TCC_TEST_GYM_SCENE_CMD) != 0) {
        printf("!! test_gym_scene build failed.\n");
        return 1;
    }

    printf("\n=== Running tests ===\n");
    fflush(stdout);
    if (system("build\\Debug\\test_dock.exe") != 0) {
        printf("!! test_dock failed.\n");
        failed = 1;
    }
    if (system("build\\Debug\\test_inspector.exe") != 0) {
        printf("!! test_inspector failed.\n");
        failed = 1;
    }
    if (system("build\\Debug\\test_hotreload.exe") != 0) {
        printf("!! test_hotreload failed.\n");
        failed = 1;
    }
    if (system("build\\Debug\\test_hotreload_dll.exe") != 0) {
        printf("!! test_hotreload_dll failed.\n");
        failed = 1;
    }
    if (system("build\\Debug\\test_collab_ot.exe") != 0) {
        printf("!! test_collab_ot failed.\n");
        failed = 1;
    }
    if (system("build\\Debug\\test_gym_scene.exe") != 0) {
        printf("!! test_gym_scene failed.\n");
        failed = 1;
    }

    if (failed) {
        printf("!! some tests failed.\n");
        return 1;
    }
    return 0;
}

/* ------- run (build + launch + watch) ----------------------------------- */

static HANDLE g_engine_process = NULL;

static int build_forge(int argc, char **argv)
{
    DWORD target_pid = (argc > 1) ? (DWORD)atol(argv[1]) : 0;

    if (target_pid) {
        g_engine_process = OpenProcess(SYNCHRONIZE, FALSE, target_pid);
        if (!g_engine_process) {
            printf("!! Could not open process %lu (error %lu)\n",
                   (unsigned long)target_pid, GetLastError());
            return 1;
        }
        printf("=== Forge: watching files, attached to PID %lu ===\n",
               (unsigned long)target_pid);
    } else {
        printf("=== Forge: watching files (standalone) ===\n");
    }
    fflush(stdout);

    {
        int rc = watch_and_rebuild();
        if (g_engine_process) {
            CloseHandle(g_engine_process);
            g_engine_process = NULL;
        }
        return rc;
    }
}

/* ------- play_test (build + launch gym scene, no watch) ----------------- */

static int build_play_test(void)
{
    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi = {0};
    char engine_path[PATH_SIZE];
    char engine_cmdline[CMD_MAX];

    printf("=== Building play test ===\n\n");
    if (build_all() != 0) return 1;

    printf("\n=== Launching gym scene ===\n");

    si.cb = sizeof(si);
    snprintf(engine_path, PATH_SIZE, "%s/AnitraEngine.exe", DEBUG_DIR);
    snprintf(engine_cmdline, sizeof(engine_cmdline),
             "\"%s\" \"%s\"", engine_path, GYM_SCENE_TOML);
    if (!CreateProcessA(engine_path, engine_cmdline,
                        NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("!! Failed to launch engine (error %lu)\n", GetLastError());
        return 1;
    }
    printf("   Engine launched (PID: %lu) — press Play in the editor to start\n",
           (unsigned long)pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

/* ------- collab (server + 2 editors + watch) ----------------------------- */

static int build_collab(void)
{
    char project_path[PATH_SIZE];
    const char *project_include;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi_server = {0}, pi_editor1 = {0}, pi_editor2 = {0};
    char cmdline[CMD_MAX];

    printf("=== Building collab (server + 2 editors) ===\n\n");
    if (build_all() != 0) return 1;
    if (build_server() != 0) return 1;

    project_include = resolve_project(project_path, sizeof(project_path));

    printf("\n=== Launching collab server + 2 editors ===\n");
    SetEnvironmentVariableA("ANITRA_COLLAB", "1");

    /* 1. Launch collab server */
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (project_include) {
        snprintf(cmdline, sizeof(cmdline),
                 "\"%s/collab_server.exe\" --port 7777 --project \"%s\"",
                 DEBUG_DIR, project_include);
    } else {
        snprintf(cmdline, sizeof(cmdline),
                 "\"%s/collab_server.exe\" --port 7777", DEBUG_DIR);
    }
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, 0,
                        NULL, NULL, &si, &pi_server)) {
        printf("!! Failed to launch collab server (error %lu)\n", GetLastError());
        return 1;
    }
    printf("   Server launched (PID: %lu)\n", (unsigned long)pi_server.dwProcessId);
    CloseHandle(pi_server.hThread);
    Sleep(500);

    /* 2. Launch editor 1 */
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (project_include) {
        snprintf(cmdline, sizeof(cmdline),
                 "\"%s/AnitraEngine.exe\" \"%s\"",
                 DEBUG_DIR, project_include);
    } else {
        snprintf(cmdline, sizeof(cmdline),
                 "\"%s/AnitraEngine.exe\"", DEBUG_DIR);
    }
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, 0,
                        NULL, NULL, &si, &pi_editor1)) {
        printf("!! Failed to launch editor 1 (error %lu)\n", GetLastError());
        TerminateProcess(pi_server.hProcess, 0);
        CloseHandle(pi_server.hProcess);
        return 1;
    }
    printf("   Editor 1 launched (PID: %lu)\n", (unsigned long)pi_editor1.dwProcessId);
    CloseHandle(pi_editor1.hThread);

    /* 3. Launch editor 2 */
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, 0,
                        NULL, NULL, &si, &pi_editor2)) {
        printf("!! Failed to launch editor 2 (error %lu)\n", GetLastError());
        TerminateProcess(pi_server.hProcess, 0);
        CloseHandle(pi_server.hProcess);
        CloseHandle(pi_editor1.hProcess);
        return 1;
    }
    printf("   Editor 2 launched (PID: %lu)\n", (unsigned long)pi_editor2.dwProcessId);
    CloseHandle(pi_editor2.hThread);

    g_engine_process = pi_editor1.hProcess;
    {
        int rc = watch_and_rebuild();
        printf("\n--- Stopping collab session... ---\n");
        TerminateProcess(pi_server.hProcess, 0);
        TerminateProcess(pi_editor2.hProcess, 0);
        CloseHandle(pi_server.hProcess);
        CloseHandle(pi_editor1.hProcess);
        CloseHandle(pi_editor2.hProcess);
        g_engine_process = NULL;
        return rc;
    }
}

/* ------- profile (AMD uProf) -------------------------------------------- */

#define AMDUPROF_CLI "C:\\Program Files\\AMD\\AMDuProf\\bin\\AMDuProfCLI.exe"
#define PROFILE_DIR  "profiling/cache_run"

static int run_uprof(const char *cmdline)
{
    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi = {0};
    char buf[CMD_MAX];
    DWORD exitCode;

    si.cb = sizeof(si);
    snprintf(buf, sizeof(buf), "%s", cmdline);
    printf(">> %s\n", buf);
    fflush(stdout);

    if (!CreateProcessA(AMDUPROF_CLI, buf, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("!! Failed to launch AMDuProfCLI (error %lu)\n", GetLastError());
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)exitCode;
}

static int build_and_profile(void)
{
    char project_path[PATH_SIZE];
    const char *project_include;
    char cmd[CMD_MAX];

    printf("=== Building and profiling (AMD uProf cache misses) ===\n\n");
    if (build_all() != 0) return 1;

    project_include = resolve_project(project_path, sizeof(project_path));

    system("rmdir /s /q " PROFILE_DIR " 2>nul");

    if (project_include) {
        snprintf(cmd, sizeof(cmd),
            "\"" AMDUPROF_CLI "\" collect"
            " -e L1_DC_ACCESSES_ALL"
            " -e L1_DC_REFILLS_ALL"
            " -e L2_CACHE_ACCESS_FROM_L1_DC_MISS"
            " -e L2_CACHE_MISS_FROM_L1_DC_MISS"
            " -e IC_TAG_IC_MISS"
            " -e RETIRED_INST"
            " -o " PROFILE_DIR
            " -d 30"
            " %s/AnitraEngine.exe \"%s\"",
            DEBUG_DIR, project_include);
    } else {
        snprintf(cmd, sizeof(cmd),
            "\"" AMDUPROF_CLI "\" collect"
            " -e L1_DC_ACCESSES_ALL"
            " -e L1_DC_REFILLS_ALL"
            " -e L2_CACHE_ACCESS_FROM_L1_DC_MISS"
            " -e L2_CACHE_MISS_FROM_L1_DC_MISS"
            " -e IC_TAG_IC_MISS"
            " -e RETIRED_INST"
            " -o " PROFILE_DIR
            " -d 30"
            " %s/AnitraEngine.exe",
            DEBUG_DIR);
    }

    printf("\n=== Collecting cache profile for 30 seconds ===\n");
    if (run_uprof(cmd) != 0) {
        fprintf(stderr, "!! AMD uProf collection failed\n");
        return 1;
    }

    {
        WIN32_FIND_DATAA fd;
        HANDLE hFind;
        char search[PATH_SIZE];
        char data_dir[PATH_SIZE];

        snprintf(search, sizeof(search), "%s\\AMDuProf-*", PROFILE_DIR);
        hFind = FindFirstFileA(search, &fd);
        if (hFind == INVALID_HANDLE_VALUE) {
            fprintf(stderr, "!! Could not find profiling data\n");
            return 1;
        }
        snprintf(data_dir, sizeof(data_dir), "%s/%s", PROFILE_DIR, fd.cFileName);
        FindClose(hFind);

        snprintf(cmd, sizeof(cmd),
            "\"" AMDUPROF_CLI "\" report -i \"%s\"",
            data_dir);

        printf("\n=== Generating report ===\n");
        if (run_uprof(cmd) != 0) {
            fprintf(stderr, "!! Report generation failed\n");
            return 1;
        }

        printf("\n=== Report: %s/report.csv ===\n", data_dir);
    }

    return 0;
}

/* ------- debug (raddbg) ------------------------------------------------- */

#define RADDBG_PROJECT "raddbg/anitra.raddbg_project"

static void write_raddbg_project(const char *project_include)
{
    char cwd[PATH_SIZE];
    FILE *f;

    GetCurrentDirectoryA(sizeof(cwd), cwd);

    f = fopen(RADDBG_PROJECT, "w");
    if (!f) {
        printf("!! Could not write %s\n", RADDBG_PROJECT);
        return;
    }

    fprintf(f, "/// auto-generated by build.c\n");
    fprintf(f, "target:\n{\n");
    fprintf(f, "  executable: \"%s/%s/AnitraEngine.exe\"\n", cwd, DEBUG_DIR);
    fprintf(f, "  working_directory: \"%s\"\n", cwd);
    if (project_include) {
        fprintf(f, "  arguments: \"%s\"\n", project_include);
    }
    fprintf(f, "  enabled: 1\n");
    fprintf(f, "}\n\n");

    fprintf(f, "file_path_map: { src: \"%s/src\" }\n", cwd);
    fprintf(f, "file_path_map: { include: \"%s/include\" }\n", cwd);

    fclose(f);
    printf("   Wrote %s\n", RADDBG_PROJECT);
}

static int build_and_debug(void)
{
    char project_path[PATH_SIZE];
    const char *project_include;
    char cmd[CMD_MAX];
    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi = {0};

    printf("=== Building and launching under RAD Debugger ===\n\n");
    if (build_all() != 0) return 1;

    project_include = resolve_project(project_path, sizeof(project_path));
    write_raddbg_project(project_include);

    snprintf(cmd, sizeof(cmd),
        "raddbg\\raddbg.exe --project:%s --auto_run",
        RADDBG_PROJECT);

    printf("   %s\n\n", cmd);

    si.cb = sizeof(si);
    if (!CreateProcessA(
            "raddbg\\raddbg.exe", cmd,
            NULL, NULL, FALSE, 0, NULL, NULL,
            &si, &pi)) {
        printf("!! Failed to launch raddbg (error %lu)\n", GetLastError());
        return 1;
    }

    printf("   RAD Debugger launched (PID: %lu)\n", (unsigned long)pi.dwProcessId);
    CloseHandle(pi.hThread);
    g_engine_process = pi.hProcess;

    {
        int rc = watch_and_rebuild();
        CloseHandle(g_engine_process);
        g_engine_process = NULL;
        return rc;
    }
}

/* ------- forge thread state --------------------------------------------- */

static HANDLE forge_stop_event   = NULL;
static HANDLE forge_thread_handle = NULL;

/* ------- watch (forge) — Windows ReadDirectoryChangesW ------------------ */

/* Watch indices (order in waitHandles array) */
#define WATCH_ENGINE  0
#define WATCH_EDITOR  1
#define WATCH_CORE    2
#define WATCH_SRC     3
#define WATCH_COLLAB  4
#define WATCH_SHADERS 5
#define WATCH_DIR_COUNT 6

static HANDLE open_watch_dir(const char *path) {
    return CreateFileA(path, FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
}

static int watch_and_rebuild(void)
{
    HANDLE hDirs[WATCH_DIR_COUNT];
    char bufs[WATCH_DIR_COUNT][4096];
    OVERLAPPED ovs[WATCH_DIR_COUNT];
    DWORD bytes;
    int i;

    static const char *dir_paths[WATCH_DIR_COUNT] = {
        "src/engine", "src/editor", "src/core",
        "src", "src/collab", "assets/shaders"
    };
    /* src/ root watched non-recursively; all others recursive */
    static const int dir_recursive[WATCH_DIR_COUNT] = {1, 1, 1, 0, 1, 1};

    printf("=== Forge: watching src/{engine,editor,core,collab} + src/ + assets/shaders ===\n");
    fflush(stdout);

    memset(ovs, 0, sizeof(ovs));

    /* Open all directory handles */
    for (i = 0; i < WATCH_DIR_COUNT; i++) {
        hDirs[i] = open_watch_dir(dir_paths[i]);
        if (hDirs[i] == INVALID_HANDLE_VALUE) {
            printf("!! Failed to open %s for watching (error %lu)\n",
                   dir_paths[i], GetLastError());
            while (--i >= 0) CloseHandle(hDirs[i]);
            return 1;
        }
        ovs[i].hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    }

    if (ensure_dirs() != 0) {
        for (i = 0; i < WATCH_DIR_COUNT; i++) {
            CloseHandle(ovs[i].hEvent);
            CloseHandle(hDirs[i]);
        }
        return 1;
    }

    /* Watch loop */
    while (1) {
        HANDLE waitHandles[WATCH_DIR_COUNT + 2];
        DWORD handleCount = WATCH_DIR_COUNT;
        DWORD waitResult;
        int stop_idx = -1, proc_idx = -1;

        for (i = 0; i < WATCH_DIR_COUNT; i++) {
            ReadDirectoryChangesW(hDirs[i], bufs[i], sizeof(bufs[i]),
                dir_recursive[i],
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE,
                &bytes, &ovs[i], NULL);
            waitHandles[i] = ovs[i].hEvent;
        }
        if (forge_stop_event) {
            stop_idx = (int)handleCount;
            waitHandles[handleCount++] = forge_stop_event;
        }
        if (g_engine_process) {
            proc_idx = (int)handleCount;
            waitHandles[handleCount++] = g_engine_process;
        }
        waitResult = WaitForMultipleObjects(handleCount, waitHandles, FALSE, INFINITE);

        /* Stop event signaled (forge thread mode) */
        if (stop_idx >= 0 && waitResult == WAIT_OBJECT_0 + stop_idx) {
            printf("\n--- Forge: stop requested, exiting watch. ---\n");
            fflush(stdout);
            break;
        }

        /* Target process exited (standalone mode) */
        if (proc_idx >= 0 && waitResult == WAIT_OBJECT_0 + proc_idx) {
            printf("\n--- Target process exited, stopping watch. ---\n");
            fflush(stdout);
            break;
        }

        /* Debounce */
        Sleep(50);
        for (i = 0; i < WATCH_DIR_COUNT; i++) {
            CancelIo(hDirs[i]);
            ResetEvent(ovs[i].hEvent);
        }

        switch (waitResult) {
        case WAIT_OBJECT_0 + WATCH_ENGINE:
            printf("\n--- Engine change detected, recompiling... ---\n");
            fflush(stdout);
            generate_migration_code();
            printf(">> " TCC_COMPILE_CMD "\n");
            fflush(stdout);
            if (system(TCC_COMPILE_CMD) == 0) {
                printf("   Compile OK.\n");
            } else {
                printf("!! Engine compile failed.\n");
            }
            break;

        case WAIT_OBJECT_0 + WATCH_EDITOR:
            printf("\n--- Editor change detected, recompiling... ---\n");
            fflush(stdout);
            generate_migration_code();
            printf(">> " TCC_EDITOR_CMD "\n");
            fflush(stdout);
            if (system(TCC_EDITOR_CMD) == 0) {
                printf("   Compile OK.\n");
            } else {
                printf("!! Editor compile failed.\n");
            }
            break;

        case WAIT_OBJECT_0 + WATCH_CORE:
            printf("\n--- Core change detected, recompiling... ---\n");
            fflush(stdout);
            generate_migration_code();
            printf(">> " TCC_CORE_CMD "\n");
            fflush(stdout);
            if (system(TCC_CORE_CMD) == 0) {
                printf("   Compile OK.\n");
            } else {
                printf("!! Core compile failed.\n");
            }
            break;

        case WAIT_OBJECT_0 + WATCH_SRC:
            /* Shared header changed — regenerate migration, rebuild all */
            printf("\n--- Shared header change detected, rebuilding all... ---\n");
            fflush(stdout);
            generate_migration_code();
            printf(">> " TCC_COMPILE_CMD "\n"); fflush(stdout);
            system(TCC_COMPILE_CMD);
            printf(">> " TCC_EDITOR_CMD "\n"); fflush(stdout);
            system(TCC_EDITOR_CMD);
            printf(">> " TCC_CORE_CMD "\n"); fflush(stdout);
            system(TCC_CORE_CMD);
            printf("   Rebuild all done.\n");
            break;

        case WAIT_OBJECT_0 + WATCH_COLLAB:
            /* Collab sources compiled into editor.dll */
            printf("\n--- Collab change detected, recompiling editor... ---\n");
            fflush(stdout);
            generate_migration_code();
            printf(">> " TCC_EDITOR_CMD "\n");
            fflush(stdout);
            if (system(TCC_EDITOR_CMD) == 0) {
                printf("   Compile OK.\n");
            } else {
                printf("!! Editor compile failed.\n");
            }
            break;

        case WAIT_OBJECT_0 + WATCH_SHADERS:
            /* GLSL changed — recompile to SPIR-V */
            printf("\n--- Shader change detected, recompiling shaders... ---\n");
            fflush(stdout);
            if (build_shaders() == 0) {
                printf("   Shaders compiled OK.\n");
            } else {
                printf("!! Shader compile failed.\n");
            }
            break;
        }
        fflush(stdout);
    }

    for (i = 0; i < WATCH_DIR_COUNT; i++) {
        CloseHandle(ovs[i].hEvent);
        CloseHandle(hDirs[i]);
    }
    return 0;
}

/* ------- forge thread entry point -------------------------------------- */

static DWORD WINAPI forge_thread_proc(LPVOID param)
{
    (void)param;
    watch_and_rebuild();
    return 0;
}

static int start_forge_thread(void)
{
    if (forge_thread_handle) return 0;
    forge_stop_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!forge_stop_event) return 1;
    forge_thread_handle = CreateThread(NULL, 0, forge_thread_proc, NULL, 0, NULL);
    if (!forge_thread_handle) {
        CloseHandle(forge_stop_event);
        forge_stop_event = NULL;
        return 1;
    }
    return 0;
}

static void stop_forge_thread(void)
{
    if (!forge_thread_handle) return;
    SetEvent(forge_stop_event);
    WaitForSingleObject(forge_thread_handle, 5000);
    CloseHandle(forge_thread_handle);
    CloseHandle(forge_stop_event);
    forge_thread_handle = NULL;
    forge_stop_event = NULL;
}
