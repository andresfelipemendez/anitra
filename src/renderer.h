#ifndef RENDERER_H
#define RENDERER_H

#include "game.h"
#include "draw_list.h"

/* Camera matrix utilities */
void update_camera_matrix(camera *cam, float *out_matrix);

/* Render functions (implemented in renderer.c) */
void render_tiles(memory *g);
void render_entities(memory *g);

/* Animation functions (implemented in anim.c) */
void update_animation(memory *g);
void init_pose_from_bind(Skeleton *skel, Vec3 *trans, Quat *rot, Vec3 *scale);
void sample_clip(AnimClip *clip, float time,
                 Vec3 *out_trans, Quat *out_rot, Vec3 *out_scale,
                 uint32_t joint_count);
void compute_world_transforms(Skeleton *skel,
                              Vec3 *trans, Quat *rot, Vec3 *scale,
                              Mat4 *out_world);
void compute_skinning_matrices(Skeleton *skel, Mat4 *world, Mat4 *out_skinning);

/* Physics functions (implemented in physics.c) */
void collision(memory *g);
void apply_movement(memory *g);

#endif /* RENDERER_H */
