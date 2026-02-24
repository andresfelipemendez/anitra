#include "core.h"
#include "loadlibrary.h"
#include <engine.h>
#include <externals.h>
#include <game.h>
#include <stdio.h>

#include <Windows.h>

/* Forward declarations */
void compile_dll(void);

static volatile LONG reloadFlag = 0;

static DWORD WINAPI waitforreloadsignal(LPVOID param) {
  HANDLE hEvent = (HANDLE)param;
  printf("Currently waiting for reload signal\n");
  while (1) {
    DWORD dwWaitResult = WaitForSingleObject(hEvent, INFINITE);
    if (dwWaitResult == WAIT_OBJECT_0) {
      printf("Hot reload signal received\n");
      InterlockedExchange(&reloadFlag, 1);
      break;
    }
  }
  return 0;
}

EXPORT void init_core(void) {
  char cwd[MAX_PATH];
  char src[MAX_PATH];
  char dest[MAX_PATH];

  printf("Core initialized\n");

  GetCurrentDirectoryA(MAX_PATH, cwd);
  printf("Current working directory: %s\n", cwd);

  HANDLE hEvent = CreateEventA(NULL, TRUE, FALSE, HOTRELOAD_EVENT_NAME);
  if (hEvent == NULL) {
    fprintf(stderr, "CreateEvent failed (%lu)\n", GetLastError());
  }
  CreateThread(NULL, 0, waitforreloadsignal, hEvent, 0, NULL);

  struct game g;
  memset(&g, 0, sizeof(g));
  g.camera.position.x = 0;
  g.camera.position.y = 160;
  g.camera.zoom = 1;

  /* Compile engine DLL before loading */
  compile_dll();

  snprintf(src, MAX_PATH, "%s\\build\\debug\\engine.dll", cwd);
  snprintf(dest, MAX_PATH, "%s\\build\\debug\\engine_copy.dll", cwd);
  CopyFileA(src, dest, FALSE);

  g.engine_lib = loadlibrary("engine_copy");
  assign_init((init)getfunction(g.engine_lib, "init_engine"));
  assign_destroy((destroy)getfunction(g.engine_lib, "destroy_engine"));
  assign_update((update)getfunction(g.engine_lib, "update_engine"));

  init_externals(&g);
  init_engine(&g);
  begin_watch_src_directory(&g);
  begin_game_loop(&g);
}

void compile_dll(void) {
  char cwd[MAX_PATH];
  char command[MAX_PATH * 2];
  GetCurrentDirectoryA(MAX_PATH, cwd);
  snprintf(command, sizeof(command), "cd /d %s && .\\build_engine.bat", cwd);
  printf("Compiling DLL with command: %s\n", command);
  system(command);
}

static DWORD WINAPI watch_src_directory_thread(LPVOID param) {
  char buffer[1024];
  DWORD bytesReturned;
  OVERLAPPED overlapped;

  (void)param;
  printf("inside watch_src_directory\n");

  HANDLE hDir = CreateFileA(
      "src", FILE_LIST_DIRECTORY,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);

  if (hDir == INVALID_HANDLE_VALUE) {
    fprintf(stderr, "CreateFile failed (%lu)\n", GetLastError());
    return 1;
  }

  memset(&overlapped, 0, sizeof(overlapped));
  overlapped.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);

  if (overlapped.hEvent == NULL) {
    fprintf(stderr, "CreateEvent failed (%lu)\n", GetLastError());
    CloseHandle(hDir);
    return 1;
  }

  printf("handle is valid, beginning watch loop\n");
  while (1) {
    if (ReadDirectoryChangesW(
            hDir, buffer, sizeof(buffer), TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
                FILE_NOTIFY_CHANGE_LAST_WRITE,
            &bytesReturned, &overlapped, NULL)) {
      WaitForSingleObject(overlapped.hEvent, INFINITE);
      FILE_NOTIFY_INFORMATION *pNotify;
      int offset = 0;
      do {
        pNotify = (FILE_NOTIFY_INFORMATION *)((char *)buffer + offset);
        wprintf(L"Change detected in: %.*s\n",
                (int)(pNotify->FileNameLength / sizeof(WCHAR)),
                pNotify->FileName);
        offset += pNotify->NextEntryOffset;
      } while (pNotify->NextEntryOffset);

      compile_dll();

      ResetEvent(overlapped.hEvent);
      InterlockedExchange(&reloadFlag, 1);
    }
  }
  return 0;
}

void begin_watch_src_directory(struct game *g) {
  (void)g;
  printf("calling watch_src_directory\n");
  CreateThread(NULL, 0, watch_src_directory_thread, NULL, 0, NULL);
}

void begin_game_loop(struct game *g) {
  char cwd[MAX_PATH];
  char src[MAX_PATH];
  char dest[MAX_PATH];

  while (g->play) {
    if (InterlockedCompareExchange(&reloadFlag, 0, 1) == 1) {
      destroy_engine(g);
      printf("Reloading...\n");

      unloadlibrary(g->engine_lib);

      GetCurrentDirectoryA(MAX_PATH, cwd);
      snprintf(src, MAX_PATH, "%s\\build\\Debug\\engine.dll", cwd);
      snprintf(dest, MAX_PATH, "%s\\build\\Debug\\engine_copy.dll", cwd);
      CopyFileA(src, dest, FALSE);

      g->engine_lib = loadlibrary("engine_copy");
      assign_init((init)getfunction(g->engine_lib, "init_engine"));
      assign_destroy((destroy)getfunction(g->engine_lib, "destroy_engine"));
      assign_update((update)getfunction(g->engine_lib, "update_engine"));
      init_engine(g);
    }
    update_externals(g);
  }
}
