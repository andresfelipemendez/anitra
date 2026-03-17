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

typedef int (*init_builder_func)(void);

int main(int argc, char **argv) {
  const char *project_path = (argc > 1) ? argv[1] : NULL;
  unsigned long pid;
  char build_copy[64];
  char core_copy[64];

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

  /* Load builder DLL, build everything */
  {
    void *blib;
    init_builder_func build;

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
    unloadlibrary(blib);
#ifdef _WIN32
    printf("[boot] build complete (%.0f ms)\n", _boot_ms(t0));
#endif
  }

  /* Load core DLL, run engine */
  while (1) {
    void *lib;
    init_core_func init;
    int result;

    if (copylibrary("core", core_copy) != 0) {
      fprintf(stderr, "Failed to copy core.dll\n");
      return 1;
    }
    lib = loadlibrary(core_copy);
    if (!lib) {
      fprintf(stderr, "Failed to load %s.dll\n", core_copy);
      return 1;
    }
    init = (init_core_func)getfunction(lib, "init_core");
    if (!init) {
      fprintf(stderr, "Failed to get init_core\n");
      unloadlibrary(lib);
      return 1;
    }
    result = init(project_path);
    if (result == 0)
      return 0;
    unloadlibrary(lib);
    printf("Core reload — restarting...\n");
  }
}
