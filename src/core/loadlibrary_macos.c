#include "core/loadlibrary.h"
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

/* Resolve exe directory once — loadlibrary/copylibrary use it as base path */
extern uint32_t _NSGetExecutablePath(char *buf, uint32_t *bufsize);

static const char *get_exe_dir(void) {
  static char dir[512];
  static int done = 0;
  if (!done) {
    uint32_t sz = sizeof(dir);
    if (_NSGetExecutablePath(dir, &sz) == 0) {
      char *last = strrchr(dir, '/');
      if (last) *(last + 1) = '\0';
    } else {
      dir[0] = '\0';
    }
    done = 1;
  }
  return dir;
}

void *loadlibrary(const char *libname) {
  char libpath[512];
  snprintf(libpath, sizeof(libpath), "%slib%s.dylib", get_exe_dir(), libname);
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

int copylibrary(const char *srcname, const char *dstname) {
  const char *base = get_exe_dir();
  char srcpath[512], dstpath[512];
  snprintf(srcpath, sizeof(srcpath), "%slib%s.dylib", base, srcname);
  snprintf(dstpath, sizeof(dstpath), "%slib%s.dylib", base, dstname);

  int src_fd = open(srcpath, O_RDONLY);
  if (src_fd < 0) {
    fprintf(stderr, "copylibrary: failed to open %s\n", srcpath);
    return -1;
  }

  int dst_fd = open(dstpath, O_WRONLY | O_CREAT | O_TRUNC, 0755);
  if (dst_fd < 0) {
    fprintf(stderr, "copylibrary: failed to create %s\n", dstpath);
    close(src_fd);
    return -1;
  }

  char buf[8192];
  ssize_t n;
  while ((n = read(src_fd, buf, sizeof(buf))) > 0) {
    write(dst_fd, buf, n);
  }

  close(src_fd);
  close(dst_fd);
  return 0;
}
