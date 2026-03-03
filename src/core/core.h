#ifndef CORE_H
#define CORE_H

#include <export.h>

EXPORT int init_core(const char *project_path);
typedef int (*init_core_func)(const char *project_path);

int begin_game_loop(void);

#endif // CORE_H
