#include <stdio.h>
#include "core/core.h"
#include "externals/externals.h"
#include "core/loadlibrary.h"

EXPORT void init() {
    printf("Core initialized\n");
    void* lib = loadlibrary("externals");
    if (lib == NULL) {
        fprintf(stderr, "Library externals loading failed\n");
    }
    printf("Library externals loaded successfully\n");
    init_externals_func init = (init_externals_func)getfunction(lib, "init_externals");

    init();
}
