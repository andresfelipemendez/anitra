#include "loadlibrary.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>

void *loadlibrary(const char *libname) {
  char libpath[256];
  snprintf(libpath, sizeof(libpath), "%s.dll", libname);
  HMODULE hLib = LoadLibrary(libpath);
  if (hLib == NULL) {
    DWORD err = GetLastError();
    fprintf(stderr, "Failed to load library: %s (error %lu)\n", libpath, err);
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

int copylibrary_with_error(const char *srcname, const char *dstname, error_value *out_error) {
  char exedir[512];
  char srcpath[512], dstpath[512];
  char *last_sep;
  DWORD len;

  if (!srcname || !srcname[0] || !dstname || !dstname[0]) {
    ERRV_RETURN_CODE_ERR(-1, out_error, 1, "copylibrary: invalid source/destination name");
  }

  /* Get directory containing the running exe */
  len = GetModuleFileNameA(NULL, exedir, sizeof(exedir));
  if (len == 0) {
    ERRV_RETURN_CODE_ERR(-1, out_error, 2, "copylibrary: failed to get executable path");
  }

  last_sep = strrchr(exedir, '\\');
  if (!last_sep) last_sep = strrchr(exedir, '/');
  if (last_sep) *(last_sep + 1) = '\0';
  else exedir[0] = '\0';

  snprintf(srcpath, sizeof(srcpath), "%s%s.dll", exedir, srcname);
  snprintf(dstpath, sizeof(dstpath), "%s%s.dll", exedir, dstname);

  if (!CopyFileA(srcpath, dstpath, FALSE)) {
    ERRV_RETURN_CODE_ERR(-1, out_error, 3, "copylibrary: failed to copy source to destination");
  }

  ERRV_RETURN_CODE_OK(0, out_error);
}

int copylibrary(const char *srcname, const char *dstname) {
  error_value err = ERRV_OK;
  int rc = copylibrary_with_error(srcname, dstname, &err);
  if (rc != 0 && !ERRV_IS_OK(err)) {
    fprintf(stderr, "%s:%d: %s (code=%d, src=%s, dst=%s, winerr=%lu)\n",
            err.file ? err.file : "copylibrary",
            err.line,
            err.message ? err.message : "copylibrary: unknown error",
            err.code,
            srcname ? srcname : "(null)",
            dstname ? dstname : "(null)",
            GetLastError());
  }
  return rc;
}
