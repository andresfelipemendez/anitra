#ifndef PHYSICS_H
#define PHYSICS_H

#include "game.h"
#include <math.h>

/* AABB collision detection */
int check_aabb_collision(rect a, rect b);

/* Physics update functions */
void collision(memory *g);
void apply_movement(memory *g);

#endif /* PHYSICS_H */
