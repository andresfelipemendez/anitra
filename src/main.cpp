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

  init_func init = (init_func)getfunction(lib, "init");
  if (init == NULL) {
    fprintf(stderr, "Failed to get init function address\n");
    return 1;
  }

  // Call the init function from core.c
  init();

  return 0;
}
