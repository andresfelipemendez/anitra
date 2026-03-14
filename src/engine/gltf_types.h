#ifndef GLTF_TYPES_H
#define GLTF_TYPES_H

#include "math3d.h"
#include "arena.h"
#include <stdint.h>

#ifdef __cplusplus
#include <SDL3/SDL_gpu.h>
#endif

/* ── Vertex format for skinned meshes ─────────────────────────── */

typedef struct {
    float position[3];
    float normal[3];
    float uv[2];
    uint8_t bone_ids[4];
    float bone_weights[4];
} SkinnedVertex;

/* ── GPU mesh primitive ───────────────────────────────────────── */

typedef struct {
    uint32_t vertex_count;
    uint32_t index_count;
#ifdef __cplusplus
    SDL_GPUBuffer *vertex_buffer;
    SDL_GPUBuffer *index_buffer;
    SDL_GPUTexture *texture;
#else
    void *vertex_buffer;
    void *index_buffer;
    void *texture;
#endif
} GltfPrimitive;

typedef struct {
    GltfPrimitive *primitives;
    uint32_t primitive_count;
    float bounds_center[3];
    float bounds_radius;
    float bounds_half_extents[3];
} GltfMesh;

/* ── Skeleton ─────────────────────────────────────────────────── */

typedef struct {
    int16_t  *parent_indices;
    Mat4     *inverse_bind;
    Vec3     *rest_translations;
    Quat     *rest_rotations;
    Vec3     *rest_scales;
    const char **joint_names;   /* arena-allocated name strings for cross-file mapping */
    uint32_t  joint_count;
} Skeleton;

/* ── Animation ────────────────────────────────────────────────── */

typedef struct {
    uint16_t joint_index;
    uint8_t  property;       /* 0=translation, 1=rotation, 2=scale */
    uint8_t  interpolation;  /* 0=step, 1=linear, 2=cubicspline */
} ChannelHeader;

typedef struct {
    float    *timestamps;
    uint32_t  keyframe_count;
} ChannelTimes;

typedef union {
    Vec3 *vec3s;
    Quat *quats;
} ChannelData;

typedef struct {
    ChannelHeader *headers;
    ChannelTimes  *times;
    ChannelData   *data;
    uint32_t       channel_count;
    float          duration;
    char           name[32];
} AnimClip;

/* ── Loaded model ─────────────────────────────────────────────── */

typedef struct {
    GltfMesh   mesh;
    Skeleton   skeleton;
    AnimClip  *clips;
    uint32_t   clip_count;
    Mat4       armature_transform;  /* skeleton root's world transform (for model matrix) */
} GltfModel;

#endif /* GLTF_TYPES_H */
