#include "core/core.h"
#include "core/loadlibrary.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static double _boot_ms(LARGE_INTEGER start) {
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  return (double)(now.QuadPart - start.QuadPart) * 1000.0 / (double)freq.QuadPart;
}
#else
#include <unistd.h>
#endif

static const char *resolve_project_path(int argc, char **argv) {
  int i;
  const char *project_path = NULL;

  for (i = 1; i < argc; i++) {
    if ((strcmp(argv[i], "--include") == 0 || strcmp(argv[i], "-i") == 0) &&
        i + 1 < argc) {
      project_path = argv[i + 1];
      i++;
      continue;
    }

    if (argv[i][0] != '-' && project_path == NULL) {
      project_path = argv[i];
    }
  }

  return project_path;
}

int main(int argc, char **argv) {
  const char *project_path = resolve_project_path(argc, argv);

  /* Build PID-based copy name so multiple instances don't collide */
  char core_copy[64];
#ifdef _WIN32
  LARGE_INTEGER t0;
  {
    unsigned long pid = (unsigned long)GetCurrentProcessId();
    snprintf(core_copy, sizeof(core_copy), "core_%lu", pid);
  }
  QueryPerformanceCounter(&t0);
  printf("[boot] start\n");
#else
  {
    int pid = (int)getpid();
    snprintf(core_copy, sizeof(core_copy), "core_%d", pid);
  }
#endif

  while (1) {
    void *lib;
    init_core_func init;
    int result;

    if (copylibrary("core", core_copy) != 0) {
      fprintf(stderr, "Failed to copy core.dll\n");
      return 1;
    }
#ifdef _WIN32
    printf("[boot] copy core: %.2f ms\n", _boot_ms(t0));
#endif

    lib = loadlibrary(core_copy);
    if (lib == NULL) {
      fprintf(stderr, "Failed to load %s.dll\n", core_copy);
      return 1;
    }
#ifdef _WIN32
    printf("[boot] load %s: %.2f ms\n", core_copy, _boot_ms(t0));
#endif

    init = (init_core_func)getfunction(lib, "init_core");
    if (init == NULL) {
      fprintf(stderr, "Failed to get init_core\n");
      unloadlibrary(lib);
      return 1;
    }

#ifdef _WIN32
    printf("[boot] calling init_core...\n");
#endif
    result = init(project_path);
    if (result == 0) {
      /* Normal exit — do NOT FreeLibrary(core_copy.dll).
         All subsystems are already torn down inside init_core().
         FreeLibrary would cascade through the DLL import chain and
         DllMain destructors can deadlock on the loader lock.
         The OS will reclaim everything when the process terminates. */
      return 0;
    }

    /* Core hot-reload requested — must unload so we can load the new copy */
    unloadlibrary(lib);
    printf("Core reload — restarting...\n");
  }

  return 0;
}
