#include "core.h"
#include "loadlibrary.h"
#include <engine.h>
#include <externals.h>
#include <game.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <Windows.h>
#endif


static volatile int reloadFlag = 0;
static void *engine_lib = NULL;

#ifdef _WIN32
static DWORD WINAPI waitforreloadsignal(LPVOID param) {
  HANDLE hEvent = (HANDLE)param;
  printf("Waiting for reload signal...\n");
  while (1) {
    DWORD result = WaitForSingleObject(hEvent, INFINITE);
    if (result == WAIT_OBJECT_0) {
      printf("Hot reload signal received\n");
      reloadFlag = 1;
      ResetEvent(hEvent);
    }
  }
  return 0;
}
#endif

EXPORT void init_core() {
  printf("Core initialized\n");

#ifdef _WIN32
  HANDLE hEvent = CreateEvent(NULL, TRUE, FALSE, HOTRELOAD_EVENT_NAME);
  if (hEvent == NULL) {
    printf("CreateEvent failed (%lu)\n", GetLastError());
  }
#endif

  game g = {};

  engine_lib = loadlibrary("engine");
  assign_init((init)getfunction(engine_lib, "init_engine"));
  assign_destroy((destroy)getfunction(engine_lib, "destroy_engine"));
  assign_update((update)getfunction(engine_lib, "update_engine"));

  init_externals(&g);
  init_engine(&g);

#ifdef _WIN32
  HANDLE hThread = CreateThread(NULL, 0, waitforreloadsignal, hEvent, 0, NULL);
  if (hThread) CloseHandle(hThread);
#endif

  begin_game_loop(g);
  destroy_engine(&g);
  end_externals(&g);
  exit(0);
}

void begin_game_loop(game &g) {
  while (g.play) {
#ifdef _WIN32
    if (reloadFlag) {
      reloadFlag = 0;
      destroy_engine(&g);
      printf("Reloading engine_copy.dll...\n");

      unloadlibrary(engine_lib);
      engine_lib = loadlibrary("engine_copy");
      assign_init((init)getfunction(engine_lib, "init_engine"));
      assign_destroy((destroy)getfunction(engine_lib, "destroy_engine"));
      assign_update((update)getfunction(engine_lib, "update_engine"));
      init_engine(&g);
    }
#endif
    update_externals(&g);
  }
}
