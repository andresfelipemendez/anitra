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
    memory g = {0};
    int reload;

/* Load engine DLL (copy first so engine.dll stays unlocked) */
    copylibrary("engine", "engine_copy");
    engine_lib = loadlibrary("engine_copy");
    if (!engine_lib) {
      fprintf(stderr, "Failed to load engine_copy.dll\n");
      return 1;
    }
    
    init_func init_e = (init_func)getfunction(engine_lib, "init_engine");
    destroy_func destroy_e = (destroy_func)getfunction(engine_lib, "destroy_engine");
    update_func update_e = (update_func)getfunction(engine_lib, "update_engine");
    
    if (!init_e || !destroy_e || !update_e) {
      fprintf(stderr, "Failed to get engine functions\n");
      unloadlibrary(engine_lib);
      return 1;
    }
    
    assign_init(init_e);
    assign_destroy(destroy_e);
    assign_update(update_e);

    /* Load editor DLL (copy first so editor.dll stays unlocked) */
    copylibrary("editor", "editor_copy");
    editor_lib = loadlibrary("editor_copy");
    if (editor_lib) {
      init_func init_ed = (init_func)getfunction(editor_lib, "init_editor");
      destroy_func destroy_ed = (destroy_func)getfunction(editor_lib, "destroy_editor");
      update_func update_ed = (update_func)getfunction(editor_lib, "update_editor");
      handle_event_func handle_ev = (handle_event_func)getfunction(editor_lib, "editor_handle_event");
      
      if (!init_ed || !destroy_ed || !update_ed) {
        fprintf(stderr, "Warning: Editor functions not found - editor disabled\n");
        unloadlibrary(editor_lib);
        editor_lib = NULL;
      } else {
        assign_editor_init(init_ed);
        assign_editor_destroy(destroy_ed);
        assign_editor_update(update_ed);
        assign_editor_handle_event(handle_ev);
      }
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

int begin_game_loop(memory *g) {
#ifdef _WIN32
  HANDLE hCoreEvent = OpenEvent(EVENT_MODIFY_STATE | SYNCHRONIZE,
                                FALSE, HOTRELOAD_CORE_EVENT_NAME);
#endif

  while (g->play) {
#ifdef _WIN32
if (reloadFlag) {
      reloadFlag = 0;
      
      /* Verify new DLL was loaded */
      if (!engine_lib) {
        fprintf(stderr, "Engine library is NULL - cannot reload\n");
        continue;
      }
      
      destroy_engine(g);
      printf("Reloading engine...\n");

      unloadlibrary(engine_lib);
      copylibrary("engine", "engine_copy");
      engine_lib = loadlibrary("engine_copy");
      if (!engine_lib) {
        fprintf(stderr, "Failed to reload engine_copy.dll\n");
        continue;
      }
      
init_func new_init = (init_func)getfunction(engine_lib, "init_engine");
      destroy_func new_destroy = (destroy_func)getfunction(engine_lib, "destroy_engine");
      update_func new_update = (update_func)getfunction(engine_lib, "update_engine");

      if (!new_init || !new_destroy || !new_update) {
        fprintf(stderr, "Failed to get engine functions after reload\n");
        unloadlibrary(engine_lib);
        continue;
      }

      assign_init(new_init);
      assign_destroy(new_destroy);
      assign_update(new_update);
      init_engine(g);
    }

    if (editorReloadFlag) {
      editorReloadFlag = 0;
      
      /* Verify new DLL was loaded */
      if (!editor_lib) {
        fprintf(stderr, "Editor library is NULL - cannot reload\n");
        continue;
      }
      
      destroy_editor(g);
      printf("Reloading editor...\n");

      unloadlibrary(editor_lib);
      copylibrary("editor", "editor_copy");
      editor_lib = loadlibrary("editor_copy");
      if (!editor_lib) {
        fprintf(stderr, "Failed to reload editor_copy.dll\n");
        continue;
      }

      init_func new_init_ed = (init_func)getfunction(editor_lib, "init_editor");
      destroy_func new_destroy_ed = (destroy_func)getfunction(editor_lib, "destroy_editor");
      update_func new_update_ed = (update_func)getfunction(editor_lib, "update_editor");
      handle_event_func new_handle_ev = (handle_event_func)getfunction(editor_lib, "editor_handle_event");

      if (!new_init_ed || !new_destroy_ed || !new_update_ed) {
        fprintf(stderr, "Failed to get editor functions after reload\n");
        unloadlibrary(editor_lib);
        editor_lib = NULL;
        continue;
      }

      assign_editor_init(new_init_ed);
      assign_editor_destroy(new_destroy_ed);
      assign_editor_update(new_update_ed);
      assign_editor_handle_event(new_handle_ev);
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
