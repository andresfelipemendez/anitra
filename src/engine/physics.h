#ifndef PHYSICS_H
#define PHYSICS_H
#include <stdbool.h>
#include <game.h>
void collision(struct game* g);
void apply_movement(struct game* g);
bool bbox_collide(rect a, rect b);
#endif
