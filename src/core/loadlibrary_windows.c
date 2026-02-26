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

int copylibrary(const char *srcname, const char *dstname) {
  char exedir[512];
  char srcpath[512], dstpath[512];
  char *last_sep;
  DWORD len;

  /* Get directory containing the running exe */
  len = GetModuleFileNameA(NULL, exedir, sizeof(exedir));
  if (len == 0) return -1;

  last_sep = strrchr(exedir, '\\');
  if (!last_sep) last_sep = strrchr(exedir, '/');
  if (last_sep) *(last_sep + 1) = '\0';
  else exedir[0] = '\0';

  snprintf(srcpath, sizeof(srcpath), "%s%s.dll", exedir, srcname);
  snprintf(dstpath, sizeof(dstpath), "%s%s.dll", exedir, dstname);

  if (!CopyFileA(srcpath, dstpath, FALSE)) {
    fprintf(stderr, "copylibrary: failed to copy %s -> %s (error %lu)\n",
            srcpath, dstpath, GetLastError());
    return -1;
  }
  return 0;
}
