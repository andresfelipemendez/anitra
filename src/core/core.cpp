#include <stdio.h>
#include "core.h"
#include <game.h>
#include <externals.h>
#include <engine.h>
#include "loadlibrary.h"

#include <Windows.h>

#include <iostream>
#include <thread>

#include <filesystem>

std::atomic<bool> reloadFlag(false);

void waitforreloadsignal(HANDLE hEvent) {

    std::cout << "Current wating for reload signal" << std::endl;
	while (true) {
		DWORD dwWaitResult = WaitForSingleObject(hEvent, INFINITE);
		if (dwWaitResult == WAIT_OBJECT_0) {
			printf("Hot reload signal received\n");
            reloadFlag.store(true);
			break;
		}
	}
}


std::string getCurrentWorkingDirectory() {
    char buffer[MAX_PATH];
    GetCurrentDirectory(MAX_PATH, buffer);
    return std::string(buffer);
}


EXPORT void init() {
    printf("Core initialized\n");

    std::string cwd = getCurrentWorkingDirectory();
    std::cout << "Current working directory: " << cwd << std::endl;

    HANDLE hEvent = CreateEvent(NULL, TRUE, FALSE, HOTRELOAD_EVENT_NAME);
    if (hEvent == NULL) {
        std::cerr << "CreateEvent failed (" << GetLastError() << ")" << std::endl;
    }
    std::thread signalThread(waitforreloadsignal, hEvent);
	signalThread.detach();

    game g;

    std::string src = cwd + "\\build\\debug\\engine.dll";
    std::string dest = cwd + "\\build\\debug\\engine_copy.dll";
    std::filesystem::copy_file(src, dest, std::filesystem::copy_options::overwrite_existing);

    g.engine_lib = loadlibrary("engine_copy");
    
    assign_hotreloadable(
        (hotreloadable_imgui_draw_func)getfunction(g.engine_lib, "hotreloadable_imgui_draw")
    );

    init_externals(&g);
    
    begin_watch_src_directory(g);
    begin_game_loop(g);
}

void compile_dll() {
    std::string cwd = getCurrentWorkingDirectory();
    std::string command = "cd /d " + cwd + " && build_engine.bat";  // Use /d to change the drive as well
    std::cout << "Compiling DLL with command: " << command << std::endl;
    system(command.c_str());
}

void copy_dll(const std::string& src, const std::string& dest) {
    std::filesystem::copy_file(src, dest, std::filesystem::copy_options::overwrite_existing);
}

void watch_src_directory() {

    std::cout << "inside watch_src_directory" << std::endl;
    HANDLE hDir = CreateFile(
        "src",
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        NULL
    );

    if (hDir == INVALID_HANDLE_VALUE) {
        std::cerr << "CreateFile failed (" << GetLastError() << ")" << std::endl;
        return;
    }

    char buffer[1024];
    DWORD bytesReturned;
    OVERLAPPED overlapped = {};
    overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    if (overlapped.hEvent == NULL) {
        std::cerr << "CreateEvent failed (" << GetLastError() << ")" << std::endl;
        CloseHandle(hDir);
        return;
    }

    std::cout << "handle is valid being watch loop" << std::endl;
    while (true) {
        if (ReadDirectoryChangesW(
            hDir,
            buffer,
            sizeof(buffer),
            TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE,
            &bytesReturned,
            &overlapped,
            NULL
        )) {
            WaitForSingleObject(overlapped.hEvent, INFINITE);
            FILE_NOTIFY_INFORMATION* pNotify;
            int offset = 0;
            do {
                pNotify = (FILE_NOTIFY_INFORMATION*)((char*)buffer + offset);
                std::wstring fileName(pNotify->FileName, pNotify->FileNameLength / sizeof(WCHAR));
                std::wcout << L"Change detected in: " << fileName << std::endl;
                offset += pNotify->NextEntryOffset;
            } while (pNotify->NextEntryOffset);

            compile_dll();

            ResetEvent(overlapped.hEvent);
            reloadFlag.store(true);
        }
    }

}

void begin_watch_src_directory(game& g) {

    std::cout << "calling watch_src_directory" << std::endl;
    std::thread watchThread(watch_src_directory);
	watchThread.detach();
}

void begin_game_loop(game& g) {
    while (g.play) {
        if (reloadFlag.load()) {
			reloadFlag.store(false);
			printf("Reloading...\n");
            unloadlibrary(g.engine_lib);


            std::string cwd = getCurrentWorkingDirectory();
            std::string src = cwd + "\\build\\Debug\\engine.dll";
            std::string dest = cwd + "\\build\\Debug\\engine_copy.dll";
            std::filesystem::copy_file(src, dest, std::filesystem::copy_options::overwrite_existing);

            g.engine_lib = loadlibrary("engine_copy");
			assign_hotreloadable(
				(hotreloadable_imgui_draw_func)getfunction(g.engine_lib, "hotreloadable_imgui_draw")
			);
		}
        update_externals(&g);
    }
}
