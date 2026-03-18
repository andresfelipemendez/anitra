#include "core.h"
#include "externals.h"
#include "cpu_profiler.h"
#include "loadlibrary.h"
#include <stdio.h>

#ifdef _WIN32
#include <Windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

static void *engine_lib = NULL;
static void *editor_lib = NULL;

/* PID-based copy names — built once, used for initial load + hot-reload */
static char engine_copy_name[64];
static char editor_copy_name[64];

/* --- DLL timestamp tracking --- */

#ifdef _WIN32
typedef FILETIME dll_time_t;

static char dll_basedir[512];
static int  dll_basedir_ready = 0;

static void ensure_dll_basedir(void) {
  if (!dll_basedir_ready) {
    DWORD len = GetModuleFileNameA(NULL, dll_basedir, sizeof(dll_basedir));
    if (len > 0) {
      char *sep = strrchr(dll_basedir, '\\');
      if (!sep) sep = strrchr(dll_basedir, '/');
      if (sep) *(sep + 1) = '\0';
    }
    dll_basedir_ready = 1;
  }
}

static dll_time_t get_dll_mtime(const char *name) {
  char path[512];
  WIN32_FILE_ATTRIBUTE_DATA data;
  dll_time_t zero = {0};
  ensure_dll_basedir();
  snprintf(path, sizeof(path), "%s%s.dll", dll_basedir, name);
  if (GetFileAttributesExA(path, GetFileExInfoStandard, &data))
    return data.ftLastWriteTime;
  return zero;
}

static int dll_time_changed(dll_time_t a, dll_time_t b) {
  return a.dwLowDateTime != b.dwLowDateTime ||
         a.dwHighDateTime != b.dwHighDateTime;
}
#else
typedef time_t dll_time_t;

static dll_time_t get_dll_mtime(const char *name) {
  char path[512];
  struct stat st;
  snprintf(path, sizeof(path), "build/Debug/lib%s.so", name);
  if (stat(path, &st) == 0) return st.st_mtime;
  return 0;
}

static int dll_time_changed(dll_time_t a, dll_time_t b) {
  return a != b;
}
#endif

static dll_time_t last_engine_time;
static dll_time_t last_editor_time;
static dll_time_t last_core_time;

/* --- profiler wiring --- */

static void wire_editor_profiler(void) {
    typedef void (*assign_cpu_prof_fns_t)(
        void(*)(const char*), void(*)(void), int(*)(void),
        void*, void*, int(*)(void));
    assign_cpu_prof_fns_t assign_cpu = (assign_cpu_prof_fns_t)getfunction(editor_lib, "assign_cpu_prof_fns");
    if (assign_cpu) assign_cpu(
        ext_cpu_zone_begin, ext_cpu_zone_end,
        cpu_prof_get_capture_enabled,
        (void*)cpu_prof_get_frame_at_offset,
        (void*)cpu_prof_get_frame_id_at_offset,
        cpu_prof_get_history_count);
}

static void wire_engine_profiler(void) {
    typedef void (*assign_profiler_fns_t)(void(*)(const char*), void(*)(void), void(*)(const char*), void(*)(void));
    assign_profiler_fns_t assign_pfns = (assign_profiler_fns_t)getfunction(engine_lib, "assign_profiler_fns");
    if (assign_pfns) assign_pfns(ext_cache_zone_begin, ext_cache_zone_end, ext_cpu_zone_begin, ext_cpu_zone_end);
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
  }
#else
  {
    int pid = (int)getpid();
    snprintf(engine_copy_name, sizeof(engine_copy_name), "engine_%d", pid);
    snprintf(editor_copy_name, sizeof(editor_copy_name), "editor_%d", pid);
  }
#endif

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

    assign_init(init_e);
    assign_destroy(destroy_e);
    assign_update(update_e);
  }

  wire_engine_profiler();

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
      assign_editor_init(init_ed);
      assign_editor_destroy(destroy_ed);
      assign_editor_update(update_ed);
      assign_editor_handle_event(handle_ev);
      wire_editor_profiler();
    }
  }

  /* Record initial DLL timestamps */
  last_engine_time = get_dll_mtime("engine");
  last_editor_time = get_dll_mtime("editor");
  last_core_time   = get_dll_mtime("core");

  init_externals(project_path);
  init_engine();
  init_editor();

  reload = begin_game_loop();

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
  while (1) {
    /* Check engine.dll timestamp */
    {
      dll_time_t cur = get_dll_mtime("engine");
      if (dll_time_changed(cur, last_engine_time)) {

        if (!engine_lib) {
          /* Engine was NULL from a previous failed reload — retry */
          copylibrary("engine", engine_copy_name);
          engine_lib = loadlibrary(engine_copy_name);
          if (engine_lib) {
            engine_init_fn new_init = (engine_init_fn)getfunction(engine_lib, "init_engine");
            engine_destroy_fn new_destroy = (engine_destroy_fn)getfunction(engine_lib, "destroy_engine");
            engine_update_fn new_update = (engine_update_fn)getfunction(engine_lib, "update_engine");
            if (!new_init || !new_destroy || !new_update) {
              fprintf(stderr, "Failed to get engine functions after reload\n");
              unloadlibrary(engine_lib);
              engine_lib = NULL;
            } else {
              assign_init(new_init);
              assign_destroy(new_destroy);
              assign_update(new_update);
              wire_engine_profiler();
              reload_project();
              init_engine();
              last_engine_time = cur;
            }
          } else {
            fprintf(stderr, "Engine library reload failed - will retry on next change\n");
          }
        } else {
          destroy_engine();
          printf("Reloading engine...\n");

          unloadlibrary(engine_lib);
          engine_lib = NULL;
          assign_init(NULL);
          assign_destroy(NULL);
          assign_update(NULL);

          copylibrary("engine", engine_copy_name);
          engine_lib = loadlibrary(engine_copy_name);
          if (!engine_lib) {
            fprintf(stderr, "Failed to reload %s.dll — function pointers cleared\n", engine_copy_name);
          } else {
            engine_init_fn new_init = (engine_init_fn)getfunction(engine_lib, "init_engine");
            engine_destroy_fn new_destroy = (engine_destroy_fn)getfunction(engine_lib, "destroy_engine");
            engine_update_fn new_update = (engine_update_fn)getfunction(engine_lib, "update_engine");

            if (!new_init || !new_destroy || !new_update) {
              fprintf(stderr, "Failed to get engine functions after reload\n");
              unloadlibrary(engine_lib);
              engine_lib = NULL;
            } else {
              assign_init(new_init);
              assign_destroy(new_destroy);
              assign_update(new_update);
              wire_engine_profiler();
              reload_project();
              init_engine();
              last_engine_time = cur;
            }
          }
        }
      }
    }

    /* Check editor.dll timestamp */
    {
      dll_time_t cur = get_dll_mtime("editor");
      if (dll_time_changed(cur, last_editor_time)) {

        if (!editor_lib) {
          /* Editor was NULL from a previous failed reload — retry */
          copylibrary("editor", editor_copy_name);
          editor_lib = loadlibrary(editor_copy_name);
          if (editor_lib) {
            editor_init_fn new_init_ed = (editor_init_fn)getfunction(editor_lib, "init_editor");
            editor_destroy_fn new_destroy_ed = (editor_destroy_fn)getfunction(editor_lib, "destroy_editor");
            editor_update_fn new_update_ed = (editor_update_fn)getfunction(editor_lib, "update_editor");
            editor_handle_event_fn new_handle_ev = (editor_handle_event_fn)getfunction(editor_lib, "editor_handle_event");
            if (!new_init_ed || !new_destroy_ed || !new_update_ed) {
              fprintf(stderr, "Failed to get editor functions after reload\n");
              unloadlibrary(editor_lib);
              editor_lib = NULL;
            } else {
              assign_editor_init(new_init_ed);
              assign_editor_destroy(new_destroy_ed);
              assign_editor_update(new_update_ed);
              assign_editor_handle_event(new_handle_ev);
              wire_editor_profiler();
              init_editor();
              last_editor_time = cur;
            }
          } else {
            fprintf(stderr, "Editor library reload failed - will retry on next change\n");
          }
        } else {
          destroy_editor();
          printf("Reloading editor...\n");

          unloadlibrary(editor_lib);
          editor_lib = NULL;
          assign_editor_init(NULL);
          assign_editor_destroy(NULL);
          assign_editor_update(NULL);
          assign_editor_handle_event(NULL);

          copylibrary("editor", editor_copy_name);
          editor_lib = loadlibrary(editor_copy_name);
          if (!editor_lib) {
            fprintf(stderr, "Failed to reload %s.dll — editor function pointers cleared\n", editor_copy_name);
          } else {
            editor_init_fn new_init_ed = (editor_init_fn)getfunction(editor_lib, "init_editor");
            editor_destroy_fn new_destroy_ed = (editor_destroy_fn)getfunction(editor_lib, "destroy_editor");
            editor_update_fn new_update_ed = (editor_update_fn)getfunction(editor_lib, "update_editor");
            editor_handle_event_fn new_handle_ev = (editor_handle_event_fn)getfunction(editor_lib, "editor_handle_event");

            if (!new_init_ed || !new_destroy_ed || !new_update_ed) {
              fprintf(stderr, "Failed to get editor functions after reload\n");
              unloadlibrary(editor_lib);
              editor_lib = NULL;
            } else {
              assign_editor_init(new_init_ed);
              assign_editor_destroy(new_destroy_ed);
              assign_editor_update(new_update_ed);
              assign_editor_handle_event(new_handle_ev);
              wire_editor_profiler();
              init_editor();
              last_editor_time = cur;
            }
          }
        }
      }
    }

    /* Check core.dll timestamp — signal main.exe to reload */
    {
      dll_time_t cur = get_dll_mtime("core");
      if (dll_time_changed(cur, last_core_time)) {
        printf("Core change detected — tearing down...\n");
        return 1;
      }
    }

    if (!update_externals()) break;
  }

  return 0; /* normal exit */
}
