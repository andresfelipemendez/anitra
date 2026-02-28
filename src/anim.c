#include "game.h"
#include <string.h>

/* Initialize skeleton pose from bind pose */
void init_pose_from_bind(Skeleton *skel, Vec3 *trans, Quat *rot, Vec3 *scale) {
    if (!skel || !trans || !rot || !scale) return;
    
    for (uint32_t i = 0; i < skel->joint_count; i++) {
        trans[i] = skel->rest_translations[i];
        rot[i] = skel->rest_rotations[i];
        scale[i] = VEC3(1.0f, 1.0f, 1.0f);
    }
}

/* Sample animation clip at given time */
void sample_clip(AnimClip *clip, float time,
                 Vec3 *out_trans, Quat *out_rot, Vec3 *out_scale,
                 uint32_t joint_count) {
    if (!clip || !out_trans || !out_rot || !out_scale) return;
    
    /* Clamp time to clip duration */
    float t = fmodf(time, clip->duration);
    if (t < 0.0f) t += clip->duration;
    
    /* For each channel, find the keyframes and interpolate */
    for (uint32_t c = 0; c < clip->channel_count; c++) {
        ChannelHeader *hdr = &clip->headers[c];
        
        if (hdr->joint_index >= joint_count) continue;
        
        uint32_t ji = hdr->joint_index;
        float *times = clip->times[c].timestamps;
        ChannelData data = clip->data[c];
        
        /* Find keyframe indices */
        int prev_idx = -1;
        for (uint32_t k = 0; k < clip->times[c].keyframe_count; k++) {
            if (times[k] <= t) prev_idx = k;
        }
        
        if (prev_idx < 0) continue;
        
        int next_idx = prev_idx + 1;
        if (next_idx >= (int)clip->times[c].keyframe_count) {
            /* Loop to first keyframe */
            next_idx = 0;
        }
        
        float t_prev = times[prev_idx];
        float t_next = times[next_idx];
        float dt = t_next - t_prev;
        float alpha = dt > 1e-6f ? (t - t_prev) / dt : 0.0f;
        
        /* Interpolate based on property type */
        switch (hdr->property) {
            case 0: { /* Translation */
                Vec3 p = data.vec3s[prev_idx];
                Vec3 n = data.vec3s[next_idx];
                out_trans[ji] = vec3_lerp(p, n, alpha);
                break;
            }
            case 1: { /* Rotation */
                Quat p = data.quats[prev_idx];
                Quat n = data.quats[next_idx];
                out_rot[ji] = quat_nlerp(p, n, alpha);
                break;
            }
            case 2: { /* Scale */
                Vec3 p = data.vec3s[prev_idx];
                Vec3 n = data.vec3s[next_idx];
                out_scale[ji] = vec3_lerp(p, n, alpha);
                break;
            }
        }
    }
}

/* Compute world transforms from local transforms */
void compute_world_transforms(Skeleton *skel,
                              Vec3 *trans, Quat *rot, Vec3 *scale,
                              Mat4 *out_world) {
    if (!skel || !trans || !rot || !scale || !out_world) return;
    
    for (uint32_t i = 0; i < skel->joint_count; i++) {
        int parent = skel->parent_indices[i];
        
        Mat4 local = mat4_from_trs(trans[i], rot[i], scale[i]);
        
        if (parent >= 0) {
            out_world[i] = mat4_mul(out_world[parent], local);
        } else {
            /* Root joint */
            out_world[i] = local;
        }
    }
}

/* Compute final skinning matrices (world * inverse_bind) */
void compute_skinning_matrices(Skeleton *skel, Mat4 *world, Mat4 *out_skinning) {
    if (!skel || !world || !out_skinning) return;
    
    for (uint32_t i = 0; i < skel->joint_count; i++) {
        out_skinning[i] = mat4_mul(world[i], skel->inverse_bind[i]);
    }
}
