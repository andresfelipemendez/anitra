#ifndef CORE_H
#define CORE_H

#include <export.h>

DECLARE_FUNC_VOID(init_core)

void begin_game_loop(struct game *g);
void begin_watch_src_directory(struct game *g);

#endif // CORE_H
