#include "core.h"
#include "loadlibrary.h"
#include <externals.h>
#include <stdio.h>

#ifdef _WIN32
#include <Windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif


/* Profiler zone wrappers exported from externals.dll */
extern void ext_cache_zone_begin(const char *name);
extern void ext_cache_zone_end(void);
extern void ext_cpu_zone_begin(const char *name);
extern void ext_cpu_zone_end(void);

static volatile int reloadFlag = 0;
static volatile int editorReloadFlag = 0;
static volatile int shutdownRequested = 0;
static void *engine_lib = NULL;
static void *editor_lib = NULL;

#ifndef _WIN32
#define ENGINE_RELOAD_SIGNAL_FILE "build/Debug/.reload-signal"
#define EDITOR_RELOAD_SIGNAL_FILE "build/Debug/.editor-reload-signal"
#define CORE_RELOAD_SIGNAL_FILE   "build/Debug/.core-reload-signal"

static int consume_reload_signal_file(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) return 0;
  remove(path);
  return 1;
}

static void clear_reload_signal_files(void) {
  remove(ENGINE_RELOAD_SIGNAL_FILE);
  remove(EDITOR_RELOAD_SIGNAL_FILE);
  remove(CORE_RELOAD_SIGNAL_FILE);
}
#endif

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

EXPORT int init_core(const char *project_path) {
  int reload;
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
#else
  /* Avoid stale file signals causing immediate reload after launch */
  clear_reload_signal_files();
#endif

  /* Load engine DLL (copy first so engine.dll stays unlocked) */
  copylibrary("engine", "engine_copy");
  engine_lib = loadlibrary("engine_copy");
  if (!engine_lib) {
    fprintf(stderr, "Failed to load engine_copy.dll\n");
    return 1;
  }

  {
    engine_init_fn init_e = (engine_init_fn)getfunction(engine_lib, "init_engine");
    engine_destroy_fn destroy_e = (engine_destroy_fn)getfunction(engine_lib, "destroy_engine");
    engine_update_fn update_e = (engine_update_fn)getfunction(engine_lib, "update_engine");

    if (!init_e || !destroy_e || !update_e) {
      fprintf(stderr, "Failed to get engine functions\n");
      unloadlibrary(engine_lib);
      return 1;
    }

    assign_init(init_e);
    assign_destroy(destroy_e);
    assign_update(update_e);
  }

  /* Wire profiler zone functions: externals -> engine */
  {
      typedef void (*assign_profiler_fns_t)(void(*)(const char*), void(*)(void), void(*)(const char*), void(*)(void));
      assign_profiler_fns_t assign_pfns = (assign_profiler_fns_t)getfunction(engine_lib, "assign_profiler_fns");
      if (assign_pfns) assign_pfns(ext_cache_zone_begin, ext_cache_zone_end, ext_cpu_zone_begin, ext_cpu_zone_end);
  }

  /* Load editor DLL (copy first so editor.dll stays unlocked) */
  copylibrary("editor", "editor_copy");
  editor_lib = loadlibrary("editor_copy");
  if (editor_lib) {
    editor_init_fn init_ed = (editor_init_fn)getfunction(editor_lib, "init_editor");
    editor_destroy_fn destroy_ed = (editor_destroy_fn)getfunction(editor_lib, "destroy_editor");
    editor_update_fn update_ed = (editor_update_fn)getfunction(editor_lib, "update_editor");
    editor_handle_event_fn handle_ev = (editor_handle_event_fn)getfunction(editor_lib, "editor_handle_event");

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

  init_externals(project_path);
  init_engine();
  init_editor();

  /* Reset state for fresh load */
  shutdownRequested = 0;
  reloadFlag = 0;
  editorReloadFlag = 0;

#ifdef _WIN32
  hEngineThread = CreateThread(NULL, 0, waitforreloadsignal, hEngineEvent, 0, NULL);
  hEditorThread = CreateThread(NULL, 0, waitforeditorreloadsignal, hEditorEvent, 0, NULL);
#endif

  reload = begin_game_loop();

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
  destroy_editor();
  destroy_engine();
  end_externals();

  unloadlibrary(engine_lib);
  unloadlibrary(editor_lib);
  engine_lib = NULL;
  editor_lib = NULL;

  return reload;
}

int begin_game_loop(void) {
#ifdef _WIN32
  HANDLE hCoreEvent = OpenEvent(EVENT_MODIFY_STATE | SYNCHRONIZE,
                                FALSE, HOTRELOAD_CORE_EVENT_NAME);
#endif

  while (1) {
#ifndef _WIN32
    if (consume_reload_signal_file(ENGINE_RELOAD_SIGNAL_FILE))
      reloadFlag = 1;
    if (consume_reload_signal_file(EDITOR_RELOAD_SIGNAL_FILE))
      editorReloadFlag = 1;
#endif

    if (reloadFlag) {
      reloadFlag = 0;

      /* Verify new DLL was loaded */
      if (!engine_lib) {
        fprintf(stderr, "Engine library is NULL - cannot reload\n");
        continue;
      }

      destroy_engine();
      printf("Reloading engine...\n");

      unloadlibrary(engine_lib);
      copylibrary("engine", "engine_copy");
      engine_lib = loadlibrary("engine_copy");
      if (!engine_lib) {
        fprintf(stderr, "Failed to reload engine_copy.dll\n");
        continue;
      }

      {
        engine_init_fn new_init = (engine_init_fn)getfunction(engine_lib, "init_engine");
        engine_destroy_fn new_destroy = (engine_destroy_fn)getfunction(engine_lib, "destroy_engine");
        engine_update_fn new_update = (engine_update_fn)getfunction(engine_lib, "update_engine");

        if (!new_init || !new_destroy || !new_update) {
          fprintf(stderr, "Failed to get engine functions after reload\n");
          unloadlibrary(engine_lib);
          continue;
        }

        assign_init(new_init);
        assign_destroy(new_destroy);
        assign_update(new_update);
      }

      /* Re-wire profiler zone functions after engine reload */
      {
          typedef void (*assign_profiler_fns_t)(void(*)(const char*), void(*)(void), void(*)(const char*), void(*)(void));
          assign_profiler_fns_t assign_pfns = (assign_profiler_fns_t)getfunction(engine_lib, "assign_profiler_fns");
          if (assign_pfns) assign_pfns(ext_cache_zone_begin, ext_cache_zone_end, ext_cpu_zone_begin, ext_cpu_zone_end);
      }

      /* Reload project.toml from disk so TOML edits take effect on hot-reload */
      reload_project();

      init_engine();
    }

    if (editorReloadFlag) {
      editorReloadFlag = 0;

      /* Verify new DLL was loaded */
      if (!editor_lib) {
        fprintf(stderr, "Editor library is NULL - cannot reload\n");
        continue;
      }

      destroy_editor();
      printf("Reloading editor...\n");

      unloadlibrary(editor_lib);
      copylibrary("editor", "editor_copy");
      editor_lib = loadlibrary("editor_copy");
      if (!editor_lib) {
        fprintf(stderr, "Failed to reload editor_copy.dll\n");
        continue;
      }

      {
        editor_init_fn new_init_ed = (editor_init_fn)getfunction(editor_lib, "init_editor");
        editor_destroy_fn new_destroy_ed = (editor_destroy_fn)getfunction(editor_lib, "destroy_editor");
        editor_update_fn new_update_ed = (editor_update_fn)getfunction(editor_lib, "update_editor");
        editor_handle_event_fn new_handle_ev = (editor_handle_event_fn)getfunction(editor_lib, "editor_handle_event");

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
      }
      init_editor();
    }

    /* Core reload: poll with 0ms timeout (non-blocking) */
#ifdef _WIN32
    if (hCoreEvent && WaitForSingleObject(hCoreEvent, 0) == WAIT_OBJECT_0) {
      printf("Core reload signal received — tearing down...\n");
      ResetEvent(hCoreEvent);
      CloseHandle(hCoreEvent);
      return 1; /* signal reload to init_core */
    }
#else
    if (consume_reload_signal_file(CORE_RELOAD_SIGNAL_FILE)) {
      printf("Core reload signal received — tearing down...\n");
      return 1; /* signal reload to init_core */
    }
#endif

    if (!update_externals()) break;
  }

#ifdef _WIN32
  if (hCoreEvent) CloseHandle(hCoreEvent);
#endif
  return 0; /* normal exit */
}
