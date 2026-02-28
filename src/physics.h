#ifndef PHYSICS_H
#define PHYSICS_H

#include "game.h"
#include <math.h>

/* AABB collision detection */
int check_aabb_collision(rect a, rect b);

/* Physics update functions */
void collision(game_state *gs);
void apply_movement(game_state *gs);

#endif /* PHYSICS_H */
