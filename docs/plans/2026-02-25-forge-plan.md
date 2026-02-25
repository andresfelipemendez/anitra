# Forge Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Extract file watching and TCC compilation from core.cpp into a `build.exe watch` mode (forge), so the engine never invokes the compiler.

**Architecture:** Two-process model — forge (`build.exe watch`) watches `src/engine/` for changes, compiles via TCC, copies the DLL, and signals a named event. The engine (`AnitraEngine.exe`) waits for the event, unloads the old DLL, loads the copy, and resumes. Communication is via the existing `Global\ReloadEvent` named event.

**Tech Stack:** C (build.c), C++ (core.cpp), Win32 API (ReadDirectoryChangesW, Named Events), TCC

---

### Task 1: Add `watch_and_rebuild()` function to build.c

**Files:**
- Modify: `build.c` (after `build_engine()` around line 467)

**Step 1: Add the HOTRELOAD_EVENT_NAME constant and watch function**

Insert this block after the `build_engine()` function (after line 467, before the `/* ------- exe` comment):

```c
/* ------- watch (forge) -------------------------------------------------- */

#define HOTRELOAD_EVENT_NAME "Global\\ReloadEvent"

static int watch_and_rebuild(void)
{
    HANDLE hDir;
    HANDLE hEvent;
    char buffer[4096];
    DWORD bytesReturned;
    OVERLAPPED overlapped = {0};
    char cmd[CMD_MAX];
    char src_path[MAX_PATH];
    char dst_path[MAX_PATH];
    DWORD cwd_len;

    printf("=== Forge: watching src\\engine for changes ===\n");
    fflush(stdout);

    /* Open directory handle for watching */
    hDir = CreateFileA(
        "src\\engine",
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        NULL);
    if (hDir == INVALID_HANDLE_VALUE) {
        printf("!! Failed to open src\\engine for watching (error %lu)\n",
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
    cwd_len = GetCurrentDirectoryA(MAX_PATH, src_path);
    snprintf(src_path + cwd_len, MAX_PATH - cwd_len,
             "\\build\\Debug\\engine.dll");
    GetCurrentDirectoryA(MAX_PATH, dst_path);
    snprintf(dst_path + cwd_len, MAX_PATH - cwd_len,
             "\\build\\Debug\\engine_copy.dll");

    /* Do an initial compile so engine_copy.dll exists on disk */
    snprintf(cmd, sizeof(cmd),
        ".\\tcc.exe -Blib/tcc -shared"
        " -o build\\Debug\\engine.dll"
        " -Isrc -Isrc/engine"
        " src/engine/engine.c"
        " src/engine/renderer.c"
        " src/engine/physics.c"
        " src/engine/scene.c"
        " src/engine/debug_render.c");
    printf(">> %s\n", cmd);
    fflush(stdout);
    if (system(cmd) == 0) {
        CopyFileA(src_path, dst_path, FALSE);
        printf("   Initial compile OK, engine_copy.dll ready.\n");
        SetEvent(hEvent);
        ResetEvent(hEvent);
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
            /* Cancel immediately — just draining the queue */
            CancelIo(hDir);
            CloseHandle(drain.hEvent);
        }

        printf("\n--- Change detected, recompiling... ---\n");
        fflush(stdout);

        snprintf(cmd, sizeof(cmd),
            ".\\tcc.exe -Blib/tcc -shared"
            " -o build\\Debug\\engine.dll"
            " -Isrc -Isrc/engine"
            " src/engine/engine.c"
            " src/engine/renderer.c"
            " src/engine/physics.c"
            " src/engine/scene.c"
            " src/engine/debug_render.c");
        printf(">> %s\n", cmd);
        fflush(stdout);

        if (system(cmd) == 0) {
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
}
```

**Step 2: Verify build.c still compiles**

Run: `cl build.c`
Expected: compiles without errors

**Step 3: Commit**

```bash
git add build.c
git commit -m "forge: add watch_and_rebuild function to build.c"
```

---

### Task 2: Wire up the `watch` target in build.c main()

**Files:**
- Modify: `build.c:1104-1166` (print_usage and main)

**Step 1: Add "watch" to print_usage**

In `print_usage()` (line 1119), add after the `exe` line:

```c
    printf("  watch        Watch src/engine and recompile on change (forge)\n");
```

**Step 2: Add "watch" to the target dispatch in main()**

In `main()`, add this branch after the `exe` case (after line 1152):

```c
    } else if (strcmp(target, "watch") == 0) {
        rc = watch_and_rebuild();
```

**Step 3: Verify build.c compiles and help shows watch**

Run: `cl build.c && build.exe help`
Expected: output includes `watch        Watch src/engine and recompile on change (forge)`

**Step 4: Commit**

```bash
git add build.c
git commit -m "forge: wire up watch target in build.c main"
```

---

### Task 3: Gut core.cpp — remove compiler and watcher code

**Files:**
- Modify: `src/core/core.cpp`

**Step 1: Remove includes that are no longer needed**

Remove these three lines (lines 10-13):

```cpp
#include <iostream>
#include <thread>

#include <filesystem>
```

**Step 2: Remove `compile_dll()` forward declaration, function, and `copy_dll()`**

Remove line 22 (forward declaration):
```cpp
void compile_dll();
```

Remove the `compile_dll()` function (lines 78-92):
```cpp
void compile_dll() { ... }
```

Remove the `copy_dll()` function (lines 94-96):
```cpp
void copy_dll(const std::string &src, const std::string &dest) { ... }
```

**Step 3: Remove `watch_src_directory()` and `begin_watch_src_directory()`**

Remove `watch_src_directory()` (lines 98-146) and `begin_watch_src_directory()` (lines 148-153).

**Step 4: Remove `getCurrentWorkingDirectory()` helper**

Remove the function (lines 39-43):
```cpp
std::string getCurrentWorkingDirectory() { ... }
```

**Step 5: Simplify `init_core()` — remove compile and copy, just load DLL**

Replace `init_core()` with:

```cpp
EXPORT void init_core() {
  printf("Core initialized\n");

  HANDLE hEvent = CreateEvent(NULL, TRUE, FALSE, HOTRELOAD_EVENT_NAME);
  if (hEvent == NULL) {
    printf("CreateEvent failed (%lu)\n", GetLastError());
  }

  game g = {};

  engine_lib = loadlibrary("engine_copy");
  assign_init((init)getfunction(engine_lib, "init_engine"));
  assign_destroy((destroy)getfunction(engine_lib, "destroy_engine"));
  assign_update((update)getfunction(engine_lib, "update_engine"));

  init_externals(&g);
  init_engine(&g);

  /* Start reload listener thread */
  HANDLE hThread = CreateThread(NULL, 0,
    (LPTHREAD_START_ROUTINE)waitforreloadsignal, hEvent, 0, NULL);
  if (hThread) CloseHandle(hThread);

  begin_game_loop(g);
}
```

**Step 6: Simplify `waitforreloadsignal()` — use printf instead of iostream**

Replace:
```cpp
void waitforreloadsignal(HANDLE hEvent) {
  std::cout << "Current wating for reload signal" << std::endl;
  while (true) {
    DWORD dwWaitResult = WaitForSingleObject(hEvent, INFINITE);
    if (dwWaitResult == WAIT_OBJECT_0) {
      printf("Hot reload signal received\n");
      reloadFlag.store(true);
      break;
    }
  }
}
```

With:
```cpp
static DWORD WINAPI waitforreloadsignal(LPVOID param) {
  HANDLE hEvent = (HANDLE)param;
  printf("Waiting for reload signal...\n");
  while (1) {
    DWORD result = WaitForSingleObject(hEvent, INFINITE);
    if (result == WAIT_OBJECT_0) {
      printf("Hot reload signal received\n");
      reloadFlag = 1;
      ResetEvent(hEvent);
    }
  }
  return 0;
}
```

Note: This changes from `std::thread` to `CreateThread` (Win32), and the function now loops forever (re-waiting after each signal) instead of breaking after one signal. This means the engine can hot-reload multiple times without restarting.

**Step 7: Simplify `begin_game_loop()` — remove DLL copy (forge does it)**

Replace:
```cpp
void begin_game_loop(game &g) {
  while (g.play) {
    if (reloadFlag.load()) {
      reloadFlag.store(false);
      destroy_engine(&g);
      printf("Reloading...\n");

      unloadlibrary(engine_lib);

      std::string cwd = getCurrentWorkingDirectory();
      std::string src = cwd + "\\build\\Debug\\engine.dll";
      std::string dest = cwd + "\\build\\Debug\\engine_copy.dll";
      std::filesystem::copy_file(src, dest, std::filesystem::copy_options::overwrite_existing);

      engine_lib = loadlibrary("engine_copy");
      assign_init((init)getfunction(engine_lib, "init_engine"));
      assign_destroy((destroy)getfunction(engine_lib, "destroy_engine"));
      assign_update((update)getfunction(engine_lib, "update_engine"));
      init_engine(&g);
    }
    update_externals(&g);
    FrameMark;
  }
}
```

With:
```cpp
void begin_game_loop(game &g) {
  while (g.play) {
    if (reloadFlag) {
      reloadFlag = 0;
      destroy_engine(&g);
      printf("Reloading engine_copy.dll...\n");

      unloadlibrary(engine_lib);
      engine_lib = loadlibrary("engine_copy");
      assign_init((init)getfunction(engine_lib, "init_engine"));
      assign_destroy((destroy)getfunction(engine_lib, "destroy_engine"));
      assign_update((update)getfunction(engine_lib, "update_engine"));
      init_engine(&g);
    }
    update_externals(&g);
    FrameMark;
  }
}
```

Key changes: removed `std::filesystem::copy_file` (forge does the copy now), replaced `reloadFlag.load()/store()` with plain volatile int.

**Step 8: Change `reloadFlag` from `std::atomic<bool>` to `volatile int`**

Replace:
```cpp
std::atomic<bool> reloadFlag(false);
```
With:
```cpp
static volatile int reloadFlag = 0;
```

**Step 9: Verify core.cpp compiles**

Run: `build.exe core`
Expected: builds without errors

**Step 10: Commit**

```bash
git add src/core/core.cpp
git commit -m "core: remove compiler and watcher, engine only loads DLLs"
```

---

### Task 4: Update core.h — remove begin_watch_src_directory declaration

**Files:**
- Modify: `src/core/core.h`

**Step 1: Remove the declaration**

Remove line 9:
```cpp
void begin_watch_src_directory(game &g);
```

Final `core.h`:
```cpp
#ifndef CORE_H
#define CORE_H

#include <export.h>

DECLARE_FUNC_VOID(init_core)

void begin_game_loop(game &g);

#endif // CORE_H
```

**Step 2: Verify full build**

Run: `build.exe all`
Expected: everything builds cleanly

**Step 3: Commit**

```bash
git add src/core/core.h
git commit -m "core: remove begin_watch_src_directory from header"
```

---

### Task 5: End-to-end test

**Step 1: Build everything**

Run: `build.exe all`
Expected: clean build, no errors

**Step 2: Start forge in Terminal 1**

Run: `build.exe watch`
Expected: prints `=== Forge: watching src\engine for changes ===` and initial compile success

**Step 3: Start engine in Terminal 2**

Run: `build\Debug\AnitraEngine.exe`
Expected: engine starts, loads `engine_copy.dll`, runs game loop

**Step 4: Trigger hot-reload**

Edit any file in `src/engine/` (e.g., add a `printf` to `init_engine` in `src/engine/engine.c`).
Expected in Terminal 1: `--- Change detected, recompiling... ---`, compile success, `Signaling engine...`
Expected in Terminal 2: `Hot reload signal received`, `Reloading engine_copy.dll...`, engine continues with new code

**Step 5: Test forge-absent case**

Kill forge (Ctrl+C), start engine alone.
Expected: engine loads whatever `engine_copy.dll` exists, runs without crashing. No hot-reload occurs (no forge to signal).

**Step 6: Commit any fixes**

```bash
git add -A
git commit -m "forge: end-to-end verified"
```

---

### Task 6: Update design doc

**Files:**
- Modify: `docs/plans/2026-02-25-forge-design.md`

**Step 1: Add a "Status: Implemented" line at the top**

Add after the `# Forge` heading:

```markdown
> Status: Implemented (2026-02-25)
```

**Step 2: Commit**

```bash
git add docs/plans/2026-02-25-forge-design.md
git commit -m "docs: mark forge design as implemented"
```
