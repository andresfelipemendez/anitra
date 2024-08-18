#include "loadlibrary.h"
#include <stdio.h>
#include <windows.h>

void *loadlibrary(const char *libname) {
  char libpath[256];
  snprintf(libpath, sizeof(libpath), "%s.dll", libname);
  HMODULE hLib = LoadLibrary(libpath);
  if (hLib == NULL) {
    fprintf(stderr, "Failed to load library: %s\n", libpath);
    return NULL;
  }
  return hLib;
}

void unloadlibrary(void *hLib) {
  if (hLib) {
    FreeLibrary((HMODULE)hLib);
  }
}

void *getfunction(void *lib, const char *funcname) {
  void *func = GetProcAddress((HMODULE)lib, funcname);
  if (func == NULL) {
    fprintf(stderr, "Failed to get function address: %s\n", funcname);
  }
  return func;
}
