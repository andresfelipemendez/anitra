#ifndef PHYSICS_H
#define PHYSICS_H

#include <game.h>

struct memory;
struct collider;
void collision(struct memory* g);
void apply_movement(struct memory* g);
bool bbox_collide(const rect* a, const rect* b);
#endif