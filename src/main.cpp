#include "core/core.h"
#include "core/loadlibrary.h"
#include <stdio.h>

int main() {
  const char *libname = "core";
  void *lib = loadlibrary(libname);
  if (lib == NULL) {
    fprintf(stderr, "Library loading failed\n");
    return 1;
  }
  printf("Library loaded successfully\n");

  init_core_func init = (init_core_func)getfunction(lib, "init_core");
  if (init == NULL) {
    fprintf(stderr, "Failed to get init function address\n");
    return 1;
  }

  init();

  return 0;
}
