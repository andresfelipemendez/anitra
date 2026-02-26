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
    unloadlibrary(lib);

    if (result == 0) break; /* normal exit */

    printf("Core reload — restarting...\n");
  }

  return 0;
}
