#ifndef CORE_H
#define CORE_H

#include <export.h>
#include <game.h>

EXPORT int init_core();
typedef int (*init_core_func)();

int begin_game_loop(memory *g);

#endif // CORE_H
