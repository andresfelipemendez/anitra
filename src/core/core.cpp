#include <stdio.h>
#include "core.h"
#include <game.h>
#include <externals.h>
#include <engine.h>
#include "loadlibrary.h"

void* load_lib(const char* libname){
    void* lib = loadlibrary(libname);
    if (lib == NULL) {
        fprintf(stderr, "Library %s loading failed\n", libname);
    }
    printf("Library %s loaded successfully\n", libname);
    return lib;
}

EXPORT void init() {
    printf("Core initialized\n");

    void* engine_lib = load_lib("engine");
    hotreloadable_imgui_draw_func hotreloadable_imgui_draw = (hotreloadable_imgui_draw_func)getfunction(engine_lib, "hotreloadable_imgui_draw");
    game g;
    init_externals(&g);
    assign_hotreloadable(hotreloadable_imgui_draw);
    bool hotReload = false;
    if(hotReload) {
        
    }

    while(g.play) {
        update_externals(&g);
        
        end_externals(&g);
    }
}
