#include "core/core.h"
#include "core/loadlibrary.h"
#include <stdio.h>

int main() {
  while (1) {
    copylibrary("core", "core_copy");
    void *lib = loadlibrary("core_copy");
    if (lib == NULL) {
      fprintf(stderr, "Failed to load core_copy.dll\n");
      return 1;
    }

    init_core_func init = (init_core_func)getfunction(lib, "init_core");
    if (init == NULL) {
      fprintf(stderr, "Failed to get init_core\n");
      unloadlibrary(lib);
      return 1;
    }

    int result = init();

    if (result == 0) {
      /* Normal exit — do NOT FreeLibrary(core_copy.dll).
         All subsystems are already torn down inside init_core().
         FreeLibrary would cascade through the import chain
         (core → externals → tracy) and tracy.dll's C++ static
         destructors deadlock in DllMain (loader lock vs thread exit).
         The OS will reclaim everything when the process terminates. */
      return 0;
    }

    /* Core hot-reload requested — must unload so we can load the new copy */
    unloadlibrary(lib);
    printf("Core reload — restarting...\n");
  }

  return 0;
}
