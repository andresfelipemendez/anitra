#include "gltf_types.h"
#include <string.h>

/* ── keyframe search ──────────────────────────────────────────── */

static uint32_t find_keyframe(float *timestamps, uint32_t count, float time) {
    uint32_t lo = 0, hi = count - 1;
    while (lo < hi - 1) {
        uint32_t mid = (lo + hi) / 2;
        if (timestamps[mid] <= time) lo = mid;
        else hi = mid;
    }
    return lo;
}

/* ── init pose from bind (rest) pose ──────────────────────────── */

void init_pose_from_bind(Skeleton *skel, Vec3 *trans, Quat *rot, Vec3 *scale) {
    memcpy(trans, skel->rest_translations, skel->joint_count * sizeof(Vec3));
    memcpy(rot,   skel->rest_rotations,    skel->joint_count * sizeof(Quat));
    memcpy(scale, skel->rest_scales,       skel->joint_count * sizeof(Vec3));
}

/* ── sample animation clip ────────────────────────────────────── */

void sample_clip(AnimClip *clip, float time,
                 Vec3 *out_trans, Quat *out_rot, Vec3 *out_scale,
                 uint32_t joint_count) {
    for (uint32_t c = 0; c < clip->channel_count; c++) {
        ChannelHeader *h = &clip->headers[c];
        ChannelTimes *t = &clip->times[c];

        if (t->keyframe_count < 2) {
            switch (h->property) {
            case 0: out_trans[h->joint_index] = clip->data[c].vec3s[0]; break;
            case 1: out_rot[h->joint_index]   = clip->data[c].quats[0]; break;
            case 2: out_scale[h->joint_index] = clip->data[c].vec3s[0]; break;
            }
            continue;
        }

        uint32_t k = find_keyframe(t->timestamps, t->keyframe_count, time);

        float t0 = t->timestamps[k];
        float t1 = t->timestamps[k + 1];
        float alpha = (t1 > t0) ? (time - t0) / (t1 - t0) : 0.0f;

        if (h->interpolation == 0) /* step */
            alpha = 0.0f;

        switch (h->property) {
        case 0: /* translation */
            out_trans[h->joint_index] = vec3_lerp(
                clip->data[c].vec3s[k], clip->data[c].vec3s[k + 1], alpha);
            break;
        case 1: /* rotation */
            out_rot[h->joint_index] = quat_slerp(
                clip->data[c].quats[k], clip->data[c].quats[k + 1], alpha);
            break;
        case 2: /* scale */
            out_scale[h->joint_index] = vec3_lerp(
                clip->data[c].vec3s[k], clip->data[c].vec3s[k + 1], alpha);
            break;
        }
    }
}

/* ── compute world transforms (forward hierarchy pass) ────────── */

void compute_world_transforms(Skeleton *skel,
                               Vec3 *trans, Quat *rot, Vec3 *scales,
                               Mat4 *out_world) {
    for (uint32_t i = 0; i < skel->joint_count; i++) {
        Mat4 local = mat4_from_trs(trans[i], rot[i], scales[i]);
        int16_t parent = skel->parent_indices[i];
        out_world[i] = (parent >= 0) ? mat4_mul(out_world[parent], local) : local;
    }
}

/* ── compute skinning matrices ────────────────────────────────── */

void compute_skinning_matrices(Skeleton *skel, Mat4 *world, Mat4 *out_skinning) {
    for (uint32_t i = 0; i < skel->joint_count; i++) {
        out_skinning[i] = mat4_mul(world[i], skel->inverse_bind[i]);
    }
}
