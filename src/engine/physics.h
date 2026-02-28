#ifndef PHYSICS_H
#define PHYSICS_H

#include <game.h>

struct game_state;
struct collider;
void collision(struct game_state* gs);
void apply_movement(struct game_state* gs);
bool bbox_collide(const rect* a, const rect* b);
#endif
