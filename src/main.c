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
#endif

typedef int  (*init_builder_func)(void);
typedef int  (*start_forge_func)(void);
typedef void (*stop_forge_func)(void);

int main(int argc, char **argv) {
  const char *project_path = (argc > 1) ? argv[1] : NULL;
  unsigned long pid;
  char build_copy[64];
  char core_copy[64];
  void *blib = NULL;
  stop_forge_func forge_stop = NULL;

#ifdef _WIN32
  LARGE_INTEGER t0;
  pid = (unsigned long)GetCurrentProcessId();
  QueryPerformanceCounter(&t0);
  printf("[boot] start\n");
#else
  pid = (unsigned long)getpid();
#endif

  snprintf(build_copy, sizeof(build_copy), "build_%lu", pid);
  snprintf(core_copy, sizeof(core_copy), "core_%lu", pid);

  /* Load builder DLL, build everything, start forge watcher */
  {
    init_builder_func build;
    start_forge_func forge_start;

    if (copylibrary("build", build_copy) != 0) {
      fprintf(stderr, "[boot] failed to copy build.dll\n");
      return 1;
    }
    blib = loadlibrary(build_copy);
    if (!blib) {
      fprintf(stderr, "[boot] failed to load %s.dll\n", build_copy);
      return 1;
    }
    build = (init_builder_func)getfunction(blib, "init_builder");
    if (!build) {
      fprintf(stderr, "[boot] failed to get init_builder\n");
      unloadlibrary(blib);
      return 1;
    }
    printf("[boot] building...\n");
    if (build() != 0) {
      fprintf(stderr, "[boot] build failed\n");
      unloadlibrary(blib);
      return 1;
    }
#ifdef _WIN32
    printf("[boot] build complete (%.0f ms)\n", _boot_ms(t0));
#endif

    /* Start forge file-watcher thread (build.dll stays loaded) */
    forge_start = (start_forge_func)getfunction(blib, "start_forge");
    forge_stop  = (stop_forge_func)getfunction(blib, "stop_forge");
    if (forge_start && forge_stop) {
      if (forge_start() == 0) {
        printf("[boot] forge watcher started\n");
      } else {
        fprintf(stderr, "[boot] warning: forge watcher failed to start\n");
        forge_stop = NULL;
      }
    } else {
      fprintf(stderr, "[boot] warning: forge functions not found in build.dll\n");
      forge_stop = NULL;
    }
  }

  /* Load core DLL, run engine */
  while (1) {
    void *lib;
    init_core_func init;
    int result;

    if (copylibrary("core", core_copy) != 0) {
      fprintf(stderr, "Failed to copy core.dll\n");
      break;
    }
    lib = loadlibrary(core_copy);
    if (!lib) {
      fprintf(stderr, "Failed to load %s.dll\n", core_copy);
      break;
    }
    init = (init_core_func)getfunction(lib, "init_core");
    if (!init) {
      fprintf(stderr, "Failed to get init_core\n");
      unloadlibrary(lib);
      break;
    }
    result = init(project_path);
    unloadlibrary(lib);
    if (result == 0)
      break;
    printf("Core reload — restarting...\n");
  }

  /* Stop forge and unload build.dll */
  if (forge_stop) forge_stop();
  if (blib) unloadlibrary(blib);

  return 0;
}
