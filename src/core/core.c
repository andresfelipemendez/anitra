#include "core.h"
#include "loadlibrary.h"
#include <stdio.h>

#ifdef _WIN32
#include <Windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif


static volatile int reloadFlag = 0;
static volatile int editorReloadFlag = 0;
static volatile int shutdownRequested = 0;
static void *engine_lib = NULL;
static void *editor_lib = NULL;
static void *externals_lib = NULL;

/* Function pointer table for externals.dll — resolved at runtime via
   copylibrary + loadlibrary + getfunction (PID-based naming, like engine/editor). */
static struct {
    int  (*init)(const char *project_path);
    int  (*update)(void);
    void (*end)(void);
    void (*init_engine)(void);
    void (*destroy_engine)(void);
    void (*init_editor)(void);
    void (*destroy_editor)(void);
    void (*reload_project)(void);
    void (*assign_init)(engine_init_fn);
    void (*assign_destroy)(engine_destroy_fn);
    void (*assign_update)(engine_update_fn);
    void (*assign_editor_init)(editor_init_fn);
    void (*assign_editor_destroy)(editor_destroy_fn);
    void (*assign_editor_update)(editor_update_fn);
    void (*assign_editor_handle_event)(editor_handle_event_fn);
    void (*cache_zone_begin)(const char *name);
    void (*cache_zone_end)(void);
    void (*cpu_zone_begin)(const char *name);
    void (*cpu_zone_end)(void);
} ext = {0};

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

/* PID-based copy names — built once, used for initial load + hot-reload */
static char engine_copy_name[64];
static char editor_copy_name[64];
static char ext_copy_name[64];

/* Load externals DLL: copy to PID-based name, loadlibrary, resolve all function pointers */
static int load_externals(void) {
    copylibrary("externals", ext_copy_name);
    externals_lib = loadlibrary(ext_copy_name);
    if (!externals_lib) {
        fprintf(stderr, "Failed to load %s\n", ext_copy_name);
        return -1;
    }

    ext.init           = (int(*)(const char*))getfunction(externals_lib, "init_externals");
    ext.update         = (int(*)(void))getfunction(externals_lib, "update_externals");
    ext.end            = (void(*)(void))getfunction(externals_lib, "end_externals");
    ext.init_engine    = (void(*)(void))getfunction(externals_lib, "init_engine");
    ext.destroy_engine = (void(*)(void))getfunction(externals_lib, "destroy_engine");
    ext.init_editor    = (void(*)(void))getfunction(externals_lib, "init_editor");
    ext.destroy_editor = (void(*)(void))getfunction(externals_lib, "destroy_editor");
    ext.reload_project = (void(*)(void))getfunction(externals_lib, "reload_project");

    ext.assign_init               = (void(*)(engine_init_fn))getfunction(externals_lib, "assign_init");
    ext.assign_destroy            = (void(*)(engine_destroy_fn))getfunction(externals_lib, "assign_destroy");
    ext.assign_update             = (void(*)(engine_update_fn))getfunction(externals_lib, "assign_update");
    ext.assign_editor_init        = (void(*)(editor_init_fn))getfunction(externals_lib, "assign_editor_init");
    ext.assign_editor_destroy     = (void(*)(editor_destroy_fn))getfunction(externals_lib, "assign_editor_destroy");
    ext.assign_editor_update      = (void(*)(editor_update_fn))getfunction(externals_lib, "assign_editor_update");
    ext.assign_editor_handle_event = (void(*)(editor_handle_event_fn))getfunction(externals_lib, "assign_editor_handle_event");

    ext.cache_zone_begin = (void(*)(const char*))getfunction(externals_lib, "ext_cache_zone_begin");
    ext.cache_zone_end   = (void(*)(void))getfunction(externals_lib, "ext_cache_zone_end");
    ext.cpu_zone_begin   = (void(*)(const char*))getfunction(externals_lib, "ext_cpu_zone_begin");
    ext.cpu_zone_end     = (void(*)(void))getfunction(externals_lib, "ext_cpu_zone_end");

    if (!ext.init || !ext.update || !ext.end || !ext.init_engine || !ext.destroy_engine) {
        fprintf(stderr, "Failed to resolve required externals functions\n");
        unloadlibrary(externals_lib);
        externals_lib = NULL;
        return -1;
    }

    return 0;
}

EXPORT int init_core(const char *project_path) {
  int reload;
  printf("Core initialized\n");

  /* Build PID-based copy names so multiple instances don't collide */
#ifdef _WIN32
  {
    unsigned long pid = (unsigned long)GetCurrentProcessId();
    snprintf(engine_copy_name, sizeof(engine_copy_name), "engine_%lu", pid);
    snprintf(editor_copy_name, sizeof(editor_copy_name), "editor_%lu", pid);
    snprintf(ext_copy_name, sizeof(ext_copy_name), "externals_%lu", pid);
  }

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
  {
    int pid = (int)getpid();
    snprintf(engine_copy_name, sizeof(engine_copy_name), "engine_%d", pid);
    snprintf(editor_copy_name, sizeof(editor_copy_name), "editor_%d", pid);
    snprintf(ext_copy_name, sizeof(ext_copy_name), "externals_%d", pid);
  }
  /* Avoid stale file signals causing immediate reload after launch */
  clear_reload_signal_files();
#endif

  /* Load externals DLL (copy first so externals.dll stays unlocked) */
  if (load_externals() != 0) return 1;

  /* Load engine DLL (copy first so engine.dll stays unlocked) */
  copylibrary("engine", engine_copy_name);
  engine_lib = loadlibrary(engine_copy_name);
  if (!engine_lib) {
    fprintf(stderr, "Failed to load %s.dll\n", engine_copy_name);
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

    ext.assign_init(init_e);
    ext.assign_destroy(destroy_e);
    ext.assign_update(update_e);
  }

  /* Wire profiler zone functions: externals -> engine */
  {
      typedef void (*assign_profiler_fns_t)(void(*)(const char*), void(*)(void), void(*)(const char*), void(*)(void));
      assign_profiler_fns_t assign_pfns = (assign_profiler_fns_t)getfunction(engine_lib, "assign_profiler_fns");
      if (assign_pfns) assign_pfns(ext.cache_zone_begin, ext.cache_zone_end, ext.cpu_zone_begin, ext.cpu_zone_end);
  }

  /* Load editor DLL (copy first so editor.dll stays unlocked) */
  copylibrary("editor", editor_copy_name);
  editor_lib = loadlibrary(editor_copy_name);
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
      ext.assign_editor_init(init_ed);
      ext.assign_editor_destroy(destroy_ed);
      ext.assign_editor_update(update_ed);
      ext.assign_editor_handle_event(handle_ev);

      /* Wire CPU profiler functions: externals -> editor */
      {
          typedef void (*assign_cpu_prof_fns_t)(
              void(*)(const char*), void(*)(void), int(*)(void),
              void*, void*, int(*)(void));
          assign_cpu_prof_fns_t assign_cpu = (assign_cpu_prof_fns_t)getfunction(editor_lib, "assign_cpu_prof_fns");
          if (assign_cpu) assign_cpu(
              ext.cpu_zone_begin, ext.cpu_zone_end,
              (int(*)(void))getfunction(externals_lib, "cpu_prof_get_capture_enabled"),
              getfunction(externals_lib, "cpu_prof_get_frame_at_offset"),
              getfunction(externals_lib, "cpu_prof_get_frame_id_at_offset"),
              (int(*)(void))getfunction(externals_lib, "cpu_prof_get_history_count"));
      }
    }
  }

  ext.init(project_path);
  ext.init_engine();
  ext.init_editor();

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
  ext.destroy_editor();
  ext.destroy_engine();
  ext.end();

  unloadlibrary(engine_lib);
  unloadlibrary(editor_lib);
  engine_lib = NULL;
  editor_lib = NULL;

  /* Unload externals LAST — engine/editor may reference its memory during teardown.
     Do NOT unload on normal exit (reload==0); the OS reclaims everything, and
     DllMain destructors can deadlock on the loader lock. */
  if (reload && externals_lib) {
    unloadlibrary(externals_lib);
    externals_lib = NULL;
  }

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

      ext.destroy_engine();
      printf("Reloading engine...\n");

      unloadlibrary(engine_lib);
      copylibrary("engine", engine_copy_name);
      engine_lib = loadlibrary(engine_copy_name);
      if (!engine_lib) {
        fprintf(stderr, "Failed to reload %s.dll\n", engine_copy_name);
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

        ext.assign_init(new_init);
        ext.assign_destroy(new_destroy);
        ext.assign_update(new_update);
      }

      /* Re-wire profiler zone functions after engine reload */
      {
          typedef void (*assign_profiler_fns_t)(void(*)(const char*), void(*)(void), void(*)(const char*), void(*)(void));
          assign_profiler_fns_t assign_pfns = (assign_profiler_fns_t)getfunction(engine_lib, "assign_profiler_fns");
          if (assign_pfns) assign_pfns(ext.cache_zone_begin, ext.cache_zone_end, ext.cpu_zone_begin, ext.cpu_zone_end);
      }

      /* Reload project.toml from disk so TOML edits take effect on hot-reload */
      ext.reload_project();

      ext.init_engine();
    }

    if (editorReloadFlag) {
      editorReloadFlag = 0;

      /* Verify new DLL was loaded */
      if (!editor_lib) {
        fprintf(stderr, "Editor library is NULL - cannot reload\n");
        continue;
      }

      ext.destroy_editor();
      printf("Reloading editor...\n");

      unloadlibrary(editor_lib);
      copylibrary("editor", editor_copy_name);
      editor_lib = loadlibrary(editor_copy_name);
      if (!editor_lib) {
        fprintf(stderr, "Failed to reload %s.dll\n", editor_copy_name);
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

        ext.assign_editor_init(new_init_ed);
        ext.assign_editor_destroy(new_destroy_ed);
        ext.assign_editor_update(new_update_ed);
        ext.assign_editor_handle_event(new_handle_ev);

        /* Re-wire CPU profiler functions after editor reload */
        {
            typedef void (*assign_cpu_prof_fns_t)(
                void(*)(const char*), void(*)(void), int(*)(void),
                void*, void*, int(*)(void));
            assign_cpu_prof_fns_t assign_cpu = (assign_cpu_prof_fns_t)getfunction(editor_lib, "assign_cpu_prof_fns");
            if (assign_cpu) assign_cpu(
                ext.cpu_zone_begin, ext.cpu_zone_end,
                (int(*)(void))getfunction(externals_lib, "cpu_prof_get_capture_enabled"),
                getfunction(externals_lib, "cpu_prof_get_frame_at_offset"),
                getfunction(externals_lib, "cpu_prof_get_frame_id_at_offset"),
                (int(*)(void))getfunction(externals_lib, "cpu_prof_get_history_count"));
        }
      }
      ext.init_editor();
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

    if (!ext.update()) break;
  }

#ifdef _WIN32
  if (hCoreEvent) CloseHandle(hCoreEvent);
#endif
  return 0; /* normal exit */
}
