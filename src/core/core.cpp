#include <stdio.h>
#include "core.h"
#include <game.h>
#include <externals.h>
#include <engine.h>
#include "loadlibrary.h"

#include <Windows.h>

#include <iostream>
#include <thread>

std::atomic<bool> reloadFlag(false);

void waitforreloadsignal(HANDLE hEvent) {
	while (true) {
		DWORD dwWaitResult = WaitForSingleObject(hEvent, INFINITE);
		if (dwWaitResult == WAIT_OBJECT_0) {
			printf("Hot reload signal received\n");
            reloadFlag.store(true);
			break;
		}
	}
}

EXPORT void init() {
    printf("Core initialized\n");

    HANDLE hEvent = CreateEvent(NULL, TRUE, FALSE, HOTRELOAD_EVENT_NAME);
    if (hEvent == NULL) {
        std::cerr << "CreateEvent failed (" << GetLastError() << ")" << std::endl;
    }
    std::thread signalThread(waitforreloadsignal, hEvent);
    game g;
    g.engine_lib = loadlibrary("engine");
    
    assign_hotreloadable(
        (hotreloadable_imgui_draw_func)getfunction(g.engine_lib, "hotreloadable_imgui_draw")
    );

    init_externals(&g);
    
    begin_game_loop(g);
}

void begin_game_loop(game& g) {
    while (g.play) {
        if (reloadFlag.load()) {
			reloadFlag.store(false);
			printf("Reloading...\n");
            unloadlibrary(g.engine_lib);
            g.engine_lib = loadlibrary("engine");
			assign_hotreloadable(
				(hotreloadable_imgui_draw_func)getfunction(g.engine_lib, "hotreloadable_imgui_draw")
			);
		}
        update_externals(&g);
    }
}
