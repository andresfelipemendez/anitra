#ifndef PHYSICS_H
#define PHYSICS_H
struct game;
struct collider;
void collision(game* g);
void apply_movement(game* g);
bool bbox_collide(struct collider a, struct collider b);
#endif