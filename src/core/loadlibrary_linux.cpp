#include "core/loadlibrary.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

void *loadlibrary(const char *libname) {
  char libpath[256];
  snprintf(libpath, sizeof(libpath), "lib%s.so", libname);
  void *hLib = dlopen(libpath, RTLD_LAZY);
  if (hLib == NULL) {
    fprintf(stderr, "Failed to load library: %s\n", libpath);
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

int copylibrary_with_error(const char *srcname, const char *dstname, error_value *out_error) {
  char srcpath[256], dstpath[256];
  int src_fd = -1;
  int dst_fd = -1;
  char buf[8192];
  ssize_t n;

  if (!srcname || !srcname[0] || !dstname || !dstname[0]) {
    ERRV_RETURN_CODE_ERR(-1, out_error, 1, "copylibrary: invalid source/destination name");
  }

  snprintf(srcpath, sizeof(srcpath), "lib%s.so", srcname);
  snprintf(dstpath, sizeof(dstpath), "lib%s.so", dstname);

  src_fd = open(srcpath, O_RDONLY);
  if (src_fd < 0) {
    ERRV_RETURN_CODE_ERR(-1, out_error, 2, "copylibrary: failed to open source library");
  }

  dst_fd = open(dstpath, O_WRONLY | O_CREAT | O_TRUNC, 0755);
  if (dst_fd < 0) {
    close(src_fd);
    ERRV_RETURN_CODE_ERR(-1, out_error, 3, "copylibrary: failed to create destination library");
  }

  while ((n = read(src_fd, buf, sizeof(buf))) > 0) {
    ssize_t written = 0;
    while (written < n) {
      ssize_t w = write(dst_fd, buf + written, (size_t)(n - written));
      if (w <= 0) {
        close(src_fd);
        close(dst_fd);
        ERRV_RETURN_CODE_ERR(-1, out_error, 4, "copylibrary: failed while writing destination library");
      }
      written += w;
    }
  }

  if (n < 0) {
    close(src_fd);
    close(dst_fd);
    ERRV_RETURN_CODE_ERR(-1, out_error, 5, "copylibrary: failed while reading source library");
  }

  close(src_fd);
  close(dst_fd);
  ERRV_RETURN_CODE_OK(0, out_error);
}

int copylibrary(const char *srcname, const char *dstname) {
  error_value err = ERRV_OK;
  int rc = copylibrary_with_error(srcname, dstname, &err);
  if (rc != 0 && !ERRV_IS_OK(err)) {
    fprintf(stderr, "%s:%d: %s (code=%d, src=%s, dst=%s, errno=%d)\n",
            err.file ? err.file : "copylibrary",
            err.line,
            err.message ? err.message : "copylibrary: unknown error",
            err.code,
            srcname ? srcname : "(null)",
            dstname ? dstname : "(null)",
            errno);
  }
  return rc;
}
