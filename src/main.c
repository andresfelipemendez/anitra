#include "core/core.h"
#include "core/loadlibrary.h"
#include <stdio.h>
#include <string.h>

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

  while (1) {
    void *lib;
    init_core_func init;
    int result;

    if (copylibrary("externals", "externals_copy") != 0) {
      fprintf(stderr, "Failed to copy externals.dll\n");
      return 1;
    }

    if (copylibrary("core", "core_copy") != 0) {
      fprintf(stderr, "Failed to copy core.dll\n");
      return 1;
    }

    lib = loadlibrary("core_copy");
    if (lib == NULL) {
      fprintf(stderr, "Failed to load core_copy.dll\n");
      return 1;
    }

    init = (init_core_func)getfunction(lib, "init_core");
    if (init == NULL) {
      fprintf(stderr, "Failed to get init_core\n");
      unloadlibrary(lib);
      return 1;
    }

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
