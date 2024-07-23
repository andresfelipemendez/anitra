#include <stdio.h>
#include "core.h"
#include "../game.h"
#include "../externals/externals.h"
#include "loadlibrary.h"

EXPORT void init() {
    printf("Core initialized\n");
    void* lib = loadlibrary("externals");
    if (lib == NULL) {
        fprintf(stderr, "Library externals loading failed\n");
    }
    printf("Library externals loaded successfully\n");
    init_externals_func init = (init_externals_func)getfunction(lib, "init_externals");
    update_externals_func update = (update_externals_func)getfunction(lib, "update_externals");
    
    game g;
    init(&g);

    while(g.play) {
        update(&g);
    }
}
