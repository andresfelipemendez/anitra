#include "core/core.h"
#include "core/loadlibrary.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static double _boot_ms(LARGE_INTEGER start) {
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  return (double)(now.QuadPart - start.QuadPart) * 1000.0 / (double)freq.QuadPart;
}
#else
#include <unistd.h>
#endif

int main(int argc, char **argv) {
  const char *project_path = (argc > 1) ? argv[1] : NULL;

  /* Build PID-based copy name so multiple instances don't collide */
  char core_copy[64];
#ifdef _WIN32
  LARGE_INTEGER t0;
  {
    unsigned long pid = (unsigned long)GetCurrentProcessId();
    snprintf(core_copy, sizeof(core_copy), "core_%lu", pid);
  }
  QueryPerformanceCounter(&t0);
  printf("[boot] start\n");
#else
  {
    int pid = (int)getpid();
    snprintf(core_copy, sizeof(core_copy), "core_%d", pid);
  }
#endif

  /* Spawn builder in watch-only mode (detached — we don't own it) */
#ifdef _WIN32
  {
    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi = {0};
    char exe_dir[MAX_PATH];
    char cmd[MAX_PATH + 64];
    si.cb = sizeof(si);
    GetModuleFileNameA(NULL, exe_dir, sizeof(exe_dir));
    {
      char *last_sep = exe_dir + strlen(exe_dir);
      while (last_sep > exe_dir && *last_sep != '\\' && *last_sep != '/') last_sep--;
      *last_sep = '\0';
    }
    snprintf(cmd, sizeof(cmd),
             "\"%s\\build.exe\" watch --pid %lu", exe_dir, (unsigned long)GetCurrentProcessId());
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                       CREATE_NEW_PROCESS_GROUP, NULL, NULL, &si, &pi)) {
      printf("[boot] builder launched (PID: %lu)\n",
             (unsigned long)pi.dwProcessId);
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
    }
  }
#endif

  while (1) {
    void *lib;
    init_core_func init;
    int result;

    if (copylibrary("core", core_copy) != 0) {
      fprintf(stderr, "Failed to copy core.dll\n");
      return 1;
    }
    lib = loadlibrary(core_copy);
    if (lib == NULL) {
      fprintf(stderr, "Failed to load %s.dll\n", core_copy);
      return 1;
    }
    init = (init_core_func)getfunction(lib, "init_core");
    if (init == NULL) {
      fprintf(stderr, "Failed to get init_core\n");
      unloadlibrary(lib);
      return 1;
    }
    result = init(project_path);
    if (result == 0) {
     return 0;
    }
    unloadlibrary(lib);
    printf("Core reload — restarting...\n");
  }

  return 0;
}
