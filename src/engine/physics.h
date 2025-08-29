#ifndef PHYSICS_H
#define PHYSICS_H
struct game;
struct collider;
void collision(game* g);
bool bbox_collide(struct collider a, struct collider b);
#endif