#include "core/loadlibrary.h"
#include <dlfcn.h>
#include <stdio.h>

void *loadlibrary(const char *libname) {
  char libpath[256];
  snprintf(libpath, sizeof(libpath), "lib%s.dylib", libname);
  void *hLib = dlopen(libpath, RTLD_LAZY);
  if (hLib == NULL) {
    fprintf(stderr, "Failed to load library: %s\n", dlerror());
    return NULL;
  }
  return hLib;
}

void unloadlibrary(void *lib) {
  if (lib != NULL) {
    dlclose(lib);
  }
}

void *getfunction(void *lib, const char *funcname) {
  void *func = dlsym(lib, funcname);
  if (func == NULL) {
    fprintf(stderr, "Failed to get function address: %s\n", funcname);
  }
  return func;
}
