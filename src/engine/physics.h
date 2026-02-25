#ifndef PHYSICS_H
#define PHYSICS_H

#include <game.h>

struct game;
struct collider;
void collision(struct game* g);
void apply_movement(struct game* g);
bool bbox_collide(const rect* a, const rect* b);
#endif