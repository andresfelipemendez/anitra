#include "core.h"
#include "loadlibrary.h"
#include <externals.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <Windows.h>
#endif


static volatile int reloadFlag = 0;
static volatile int editorReloadFlag = 0;
static volatile int shutdownRequested = 0;
static void *engine_lib = NULL;
static void *editor_lib = NULL;

#ifdef _WIN32
static HANDLE hEngineThread = NULL;
static HANDLE hEditorThread = NULL;

static DWORD WINAPI waitforreloadsignal(LPVOID param) {
  HANDLE hEvent = (HANDLE)param;
  printf("Waiting for engine reload signal...\n");
  while (!shutdownRequested) {
    DWORD result = WaitForSingleObject(hEvent, 100);
    if (result == WAIT_OBJECT_0 && !shutdownRequested) {
      printf("Hot reload signal received (engine)\n");
      reloadFlag = 1;
      ResetEvent(hEvent);
    }
  }
  return 0;
}

static DWORD WINAPI waitforeditorreloadsignal(LPVOID param) {
  HANDLE hEvent = (HANDLE)param;
  printf("Waiting for editor reload signal...\n");
  while (!shutdownRequested) {
    DWORD result = WaitForSingleObject(hEvent, 100);
    if (result == WAIT_OBJECT_0 && !shutdownRequested) {
      printf("Hot reload signal received (editor)\n");
      editorReloadFlag = 1;
      ResetEvent(hEvent);
    }
  }
  return 0;
}
#endif

EXPORT int init_core() {
  printf("Core initialized\n");

#ifdef _WIN32
  HANDLE hEngineEvent = CreateEvent(NULL, TRUE, FALSE, HOTRELOAD_EVENT_NAME);
  HANDLE hEditorEvent;

  if (hEngineEvent == NULL) {
    printf("CreateEvent (engine) failed (%lu)\n", GetLastError());
  }

  hEditorEvent = CreateEvent(NULL, TRUE, FALSE, HOTRELOAD_EDITOR_EVENT_NAME);
  if (hEditorEvent == NULL) {
    printf("CreateEvent (editor) failed (%lu)\n", GetLastError());
  }
#endif

  {
    game g = {0};
    int reload;

    /* Load engine DLL (copy first so engine.dll stays unlocked) */
    copylibrary("engine", "engine_copy");
    engine_lib = loadlibrary("engine_copy");
    assign_init((init)getfunction(engine_lib, "init_engine"));
    assign_destroy((destroy)getfunction(engine_lib, "destroy_engine"));
    assign_update((update)getfunction(engine_lib, "update_engine"));

    /* Load editor DLL (copy first so editor.dll stays unlocked) */
    copylibrary("editor", "editor_copy");
    editor_lib = loadlibrary("editor_copy");
    if (editor_lib) {
      assign_editor_init((init)getfunction(editor_lib, "init_editor"));
      assign_editor_destroy((destroy)getfunction(editor_lib, "destroy_editor"));
      assign_editor_update((update)getfunction(editor_lib, "update_editor"));
      assign_editor_handle_event((handle_event)getfunction(editor_lib, "editor_handle_event"));
    }

    init_externals(&g);
    init_engine(&g);
    init_editor(&g);

#ifdef _WIN32
    /* Reset state for fresh load */
    shutdownRequested = 0;
    reloadFlag = 0;
    editorReloadFlag = 0;

    hEngineThread = CreateThread(NULL, 0, waitforreloadsignal, hEngineEvent, 0, NULL);
    hEditorThread = CreateThread(NULL, 0, waitforeditorreloadsignal, hEditorEvent, 0, NULL);
#endif

    reload = begin_game_loop(&g);

#ifdef _WIN32
    /* Stop reload threads FIRST — they run code from this DLL,
       so they must exit before we unload anything. */
    shutdownRequested = 1;
    if (hEngineThread) {
      WaitForSingleObject(hEngineThread, 500);
      CloseHandle(hEngineThread);
      hEngineThread = NULL;
    }
    if (hEditorThread) {
      WaitForSingleObject(hEditorThread, 500);
      CloseHandle(hEditorThread);
      hEditorThread = NULL;
    }

    /* Close named events */
    if (hEngineEvent) CloseHandle(hEngineEvent);
    if (hEditorEvent) CloseHandle(hEditorEvent);
#endif

    /* Teardown */
    destroy_editor(&g);
    destroy_engine(&g);
    end_externals(&g);

    unloadlibrary(engine_lib);
    unloadlibrary(editor_lib);
    engine_lib = NULL;
    editor_lib = NULL;

    return reload;
  }
}

int begin_game_loop(game *g) {
#ifdef _WIN32
  HANDLE hCoreEvent = OpenEvent(EVENT_MODIFY_STATE | SYNCHRONIZE,
                                FALSE, HOTRELOAD_CORE_EVENT_NAME);
#endif

  while (g->play) {
#ifdef _WIN32
    if (reloadFlag) {
      reloadFlag = 0;
      destroy_engine(g);
      printf("Reloading engine...\n");

      unloadlibrary(engine_lib);
      copylibrary("engine", "engine_copy");
      engine_lib = loadlibrary("engine_copy");
      assign_init((init)getfunction(engine_lib, "init_engine"));
      assign_destroy((destroy)getfunction(engine_lib, "destroy_engine"));
      assign_update((update)getfunction(engine_lib, "update_engine"));
      init_engine(g);
    }

    if (editorReloadFlag) {
      editorReloadFlag = 0;
      destroy_editor(g);
      printf("Reloading editor...\n");

      unloadlibrary(editor_lib);
      copylibrary("editor", "editor_copy");
      editor_lib = loadlibrary("editor_copy");
      assign_editor_init((init)getfunction(editor_lib, "init_editor"));
      assign_editor_destroy((destroy)getfunction(editor_lib, "destroy_editor"));
      assign_editor_update((update)getfunction(editor_lib, "update_editor"));
      assign_editor_handle_event((handle_event)getfunction(editor_lib, "editor_handle_event"));
      init_editor(g);
    }

    /* Core reload: poll with 0ms timeout (non-blocking) */
    if (hCoreEvent && WaitForSingleObject(hCoreEvent, 0) == WAIT_OBJECT_0) {
      printf("Core reload signal received — tearing down...\n");
      ResetEvent(hCoreEvent);
      CloseHandle(hCoreEvent);
      return 1; /* signal reload to init_core */
    }
#endif
    update_externals(g);
  }

#ifdef _WIN32
  if (hCoreEvent) CloseHandle(hCoreEvent);
#endif
  return 0; /* normal exit */
}
