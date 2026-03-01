#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#include "gltf_types.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_SIMD
#include "stb_image.h"
#include <SDL3/SDL_gpu.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>

/* GPU device — set via gltf_set_gpu_device() before loading */
static SDL_GPUDevice *s_gpu_device;

void gltf_set_gpu_device(void *dev) {
    s_gpu_device = (SDL_GPUDevice *)dev;
}

/* ── helpers ──────────────────────────────────────────────────── */

static SDL_GPUBuffer *upload_gpu_buffer(const void *data, uint32_t size,
                                        SDL_GPUBufferUsageFlags usage) {
    SDL_GPUBufferCreateInfo buf_info = {0};
    SDL_GPUBuffer *buf;
    SDL_GPUTransferBufferCreateInfo tbuf_info = {0};
    SDL_GPUTransferBuffer *transfer;
    void *mapped;
    SDL_GPUCommandBuffer *cmd;
    SDL_GPUCopyPass *copy;
    SDL_GPUTransferBufferLocation src = {0};
    SDL_GPUBufferRegion dst = {0};

    buf_info.usage = usage;
    buf_info.size = size;
    buf = SDL_CreateGPUBuffer(s_gpu_device, &buf_info);
    if (!buf) return NULL;

    tbuf_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbuf_info.size = size;
    transfer = SDL_CreateGPUTransferBuffer(s_gpu_device, &tbuf_info);
    if (!transfer) { SDL_ReleaseGPUBuffer(s_gpu_device, buf); return NULL; }

    mapped = SDL_MapGPUTransferBuffer(s_gpu_device, transfer, 0);
    memcpy(mapped, data, size);
    SDL_UnmapGPUTransferBuffer(s_gpu_device, transfer);

    cmd = SDL_AcquireGPUCommandBuffer(s_gpu_device);
    copy = SDL_BeginGPUCopyPass(cmd);

    src.transfer_buffer = transfer;
    src.offset = 0;

    dst.buffer = buf;
    dst.offset = 0;
    dst.size = size;

    SDL_UploadToGPUBuffer(copy, &src, &dst, 0);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);

    SDL_ReleaseGPUTransferBuffer(s_gpu_device, transfer);
    return buf;
}

/* ── texture extraction ───────────────────────────────────────── */

static int path_is_absolute(const char *p) {
    if (!p || !p[0]) return 0;
#ifdef _WIN32
    if ((p[0] && p[1] == ':') || p[0] == '\\' || p[0] == '/') return 1;
#else
    if (p[0] == '/') return 1;
#endif
    return 0;
}

static void path_get_dirname(const char *path, char *out, size_t out_size) {
    const char *slash;
    size_t len;
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!path) return;

    slash = strrchr(path, '/');
#ifdef _WIN32
    {
        const char *back = strrchr(path, '\\');
        if (!slash || (back && back > slash)) slash = back;
    }
#endif
    if (!slash) {
        snprintf(out, out_size, ".");
        return;
    }
    len = (size_t)(slash - path);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, path, len);
    out[len] = '\0';
}

static SDL_GPUTexture *extract_texture(cgltf_image *image, const char *asset_dir) {
    int w, h, channels;
    unsigned char *pixels = NULL;
    uint32_t img_size;
    SDL_GPUTextureCreateInfo tex_info = {0};
    SDL_GPUTexture *texture;
    SDL_GPUTransferBufferCreateInfo tbuf_info = {0};
    SDL_GPUTransferBuffer *transfer;
    void *mapped;
    SDL_GPUCommandBuffer *cmd;
    SDL_GPUCopyPass *copy;
    SDL_GPUTextureTransferInfo src = {0};
    SDL_GPUTextureRegion dst_region = {0};
    if (image->buffer_view) {
        /* embedded image in GLB */
        const uint8_t *buf = (const uint8_t *)image->buffer_view->buffer->data;
        const uint8_t *ptr = buf + image->buffer_view->offset;
        pixels = stbi_load_from_memory(ptr, (int)image->buffer_view->size,
                                       &w, &h, &channels, 4);
    } else if (image->uri) {
        if (asset_dir && asset_dir[0] && !path_is_absolute(image->uri)) {
            char resolved[PATH_MAX];
            snprintf(resolved, sizeof(resolved), "%s/%s", asset_dir, image->uri);
            pixels = stbi_load(resolved, &w, &h, &channels, 4);
        } else {
            pixels = stbi_load(image->uri, &w, &h, &channels, 4);
        }
    }

    if (!pixels) {
        fprintf(stderr, "Failed to decode glTF image\n");
        return NULL;
    }

    img_size = (uint32_t)(w * h * 4);

    tex_info.type = SDL_GPU_TEXTURETYPE_2D;
    tex_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    tex_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tex_info.width = (uint32_t)w;
    tex_info.height = (uint32_t)h;
    tex_info.layer_count_or_depth = 1;
    tex_info.num_levels = 1;
    tex_info.sample_count = SDL_GPU_SAMPLECOUNT_1;

    texture = SDL_CreateGPUTexture(s_gpu_device, &tex_info);
    if (!texture) { stbi_image_free(pixels); return NULL; }

    /* upload via transfer buffer */
    tbuf_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbuf_info.size = img_size;
    transfer = SDL_CreateGPUTransferBuffer(s_gpu_device, &tbuf_info);

    mapped = SDL_MapGPUTransferBuffer(s_gpu_device, transfer, 0);
    memcpy(mapped, pixels, img_size);
    SDL_UnmapGPUTransferBuffer(s_gpu_device, transfer);
    stbi_image_free(pixels);

    cmd = SDL_AcquireGPUCommandBuffer(s_gpu_device);
    copy = SDL_BeginGPUCopyPass(cmd);

    src.transfer_buffer = transfer;
    dst_region.texture = texture;
    dst_region.w = (uint32_t)w;
    dst_region.h = (uint32_t)h;
    dst_region.d = 1;

    SDL_UploadToGPUTexture(copy, &src, &dst_region, 0);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(s_gpu_device, transfer);

    return texture;
}

/* ── matrix helpers ───────────────────────────────────────────── */

/* Transform a position (x,y,z) by a column-major 4x4 matrix (w=1) */
static void transform_point(const float *m, float *xyz) {
    float x = xyz[0], y = xyz[1], z = xyz[2];
    xyz[0] = m[0]*x + m[4]*y + m[ 8]*z + m[12];
    xyz[1] = m[1]*x + m[5]*y + m[ 9]*z + m[13];
    xyz[2] = m[2]*x + m[6]*y + m[10]*z + m[14];
}

/* Transform a direction (normal) by upper-3x3 of a column-major 4x4 matrix */
static void transform_normal(const float *m, float *xyz) {
    float x = xyz[0], y = xyz[1], z = xyz[2];
    float len;
    xyz[0] = m[0]*x + m[4]*y + m[ 8]*z;
    xyz[1] = m[1]*x + m[5]*y + m[ 9]*z;
    xyz[2] = m[2]*x + m[6]*y + m[10]*z;
    /* renormalize */
    len = sqrtf(xyz[0]*xyz[0] + xyz[1]*xyz[1] + xyz[2]*xyz[2]);
    if (len > 1e-6f) { xyz[0] /= len; xyz[1] /= len; xyz[2] /= len; }
}

/* Check if a column-major 4x4 matrix is identity (within epsilon) */
static int mat4_is_identity_check(const float *m) {
    const float id[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    int i;
    for (i = 0; i < 16; i++)
        if (fabsf(m[i] - id[i]) > 1e-5f) return 0;
    return 1;
}

/* Invert an affine 4x4 (column-major, bottom row = [0,0,0,1]) via 3x3 cofactors */
static void invert_affine(const float *m, float *out) {
    /* 3x3 part: column-major indices [0-2], [4-6], [8-10] */
    float a=m[0], b=m[4], c=m[8];
    float d=m[1], e=m[5], f=m[9];
    float g=m[2], h=m[6], k=m[10];
    float tx=m[12], ty=m[13], tz=m[14];
    float det, inv_det;
    float r00, r10, r20, r01, r11, r21, r02, r12, r22;

    det = a*(e*k - f*h) - b*(d*k - f*g) + c*(d*h - e*g);
    if (fabsf(det) < 1e-12f) {
        /* degenerate — return identity */
        float id[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        memcpy(out, id, sizeof(float)*16);
        return;
    }
    inv_det = 1.0f / det;

    /* inverse of upper-left 3x3 (stored column-major) */
    r00 = (e*k - f*h) * inv_det;
    r10 = -(d*k - f*g) * inv_det;
    r20 = (d*h - e*g) * inv_det;
    r01 = -(b*k - c*h) * inv_det;
    r11 = (a*k - c*g) * inv_det;
    r21 = -(a*h - b*g) * inv_det;
    r02 = (b*f - c*e) * inv_det;
    r12 = -(a*f - c*d) * inv_det;
    r22 = (a*e - b*d) * inv_det;

    out[0] = r00; out[1] = r10; out[2]  = r20; out[3]  = 0;
    out[4] = r01; out[5] = r11; out[6]  = r21; out[7]  = 0;
    out[8] = r02; out[9] = r12; out[10] = r22; out[11] = 0;

    /* translation: -R^-1 * t */
    out[12] = -(r00*tx + r01*ty + r02*tz);
    out[13] = -(r10*tx + r11*ty + r12*tz);
    out[14] = -(r20*tx + r21*ty + r22*tz);
    out[15] = 1;
}

/* Multiply two column-major 4x4 matrices: out = A * B */
static void mul_mat4(const float *a, const float *b, float *out) {
    int col, row, k;
    for (col = 0; col < 4; col++) {
        for (row = 0; row < 4; row++) {
            float sum = 0;
            for (k = 0; k < 4; k++)
                sum += a[k*4 + row] * b[col*4 + k];
            out[col*4 + row] = sum;
        }
    }
}

/* ── mesh extraction ──────────────────────────────────────────── */

static GltfMesh extract_mesh(cgltf_mesh *mesh, const float *node_world, const char *asset_dir, arena *ar) {
    GltfMesh result = {0};
    int apply_transform;
    uint32_t pi;

    result.primitive_count = (uint32_t)mesh->primitives_count;
    result.primitives = (GltfPrimitive *)arena_alloc(ar,
        (uint32_t)(result.primitive_count * sizeof(GltfPrimitive)), 16, "mesh_primitives");

    /* bounding-box accumulators for bounding sphere computation */
    float bb_min[3] = { 1e30f, 1e30f, 1e30f };
    float bb_max[3] = {-1e30f,-1e30f,-1e30f};
    int total_verts = 0;

    /* check if we actually need to apply the transform */
    apply_transform = node_world && !mat4_is_identity_check(node_world);

    for (pi = 0; pi < result.primitive_count; pi++) {
        cgltf_primitive *prim = &mesh->primitives[pi];
        GltfPrimitive *out = &result.primitives[pi];
        cgltf_accessor *pos_acc = NULL, *norm_acc = NULL, *uv_acc = NULL;
        cgltf_accessor *joints_acc = NULL, *weights_acc = NULL;
        cgltf_size ai;
        uint32_t vert_count;
        SkinnedVertex *verts;
        uint32_t vi;

        memset(out, 0, sizeof(*out));

        /* find accessors */
        for (ai = 0; ai < prim->attributes_count; ai++) {
            cgltf_attribute *attr = &prim->attributes[ai];
            switch (attr->type) {
            case cgltf_attribute_type_position: pos_acc = attr->data; break;
            case cgltf_attribute_type_normal:   norm_acc = attr->data; break;
            case cgltf_attribute_type_texcoord: uv_acc = attr->data; break;
            case cgltf_attribute_type_joints:   joints_acc = attr->data; break;
            case cgltf_attribute_type_weights:  weights_acc = attr->data; break;
            default: break;
            }
        }

        if (!pos_acc) continue;
        vert_count = (uint32_t)pos_acc->count;
        out->vertex_count = vert_count;

        /* build interleaved vertex array */
        verts = (SkinnedVertex *)malloc(vert_count * sizeof(SkinnedVertex));
        memset(verts, 0, vert_count * sizeof(SkinnedVertex));

        for (vi = 0; vi < vert_count; vi++) {
            if (pos_acc)
                cgltf_accessor_read_float(pos_acc, vi, verts[vi].position, 3);
            if (norm_acc)
                cgltf_accessor_read_float(norm_acc, vi, verts[vi].normal, 3);
            if (uv_acc)
                cgltf_accessor_read_float(uv_acc, vi, verts[vi].uv, 2);

            /* apply node world transform to bring vertices into bind-pose world space */
            if (apply_transform) {
                transform_point(node_world, verts[vi].position);
                if (norm_acc)
                    transform_normal(node_world, verts[vi].normal);
            }

            /* accumulate bounding box */
            {
                float *p = verts[vi].position;
                if (p[0] < bb_min[0]) bb_min[0] = p[0];
                if (p[1] < bb_min[1]) bb_min[1] = p[1];
                if (p[2] < bb_min[2]) bb_min[2] = p[2];
                if (p[0] > bb_max[0]) bb_max[0] = p[0];
                if (p[1] > bb_max[1]) bb_max[1] = p[1];
                if (p[2] > bb_max[2]) bb_max[2] = p[2];
                total_verts++;
            }

            if (joints_acc) {
                cgltf_uint jids[4] = {0};
                cgltf_accessor_read_uint(joints_acc, vi, jids, 4);
                verts[vi].bone_ids[0] = (uint8_t)jids[0];
                verts[vi].bone_ids[1] = (uint8_t)jids[1];
                verts[vi].bone_ids[2] = (uint8_t)jids[2];
                verts[vi].bone_ids[3] = (uint8_t)jids[3];
            }
            if (weights_acc)
                cgltf_accessor_read_float(weights_acc, vi, verts[vi].bone_weights, 4);
            else {
                /* no weights: default to bone 0 fully weighted */
                verts[vi].bone_weights[0] = 1.0f;
            }
        }

        out->vertex_buffer = upload_gpu_buffer(verts,
            vert_count * sizeof(SkinnedVertex), SDL_GPU_BUFFERUSAGE_VERTEX);
        free(verts);

        /* indices */
        if (prim->indices) {
            uint32_t idx_count = (uint32_t)prim->indices->count;
            uint16_t *indices;
            uint32_t ii;

            out->index_count = idx_count;
            indices = (uint16_t *)malloc(idx_count * sizeof(uint16_t));
            for (ii = 0; ii < idx_count; ii++) {
                indices[ii] = (uint16_t)cgltf_accessor_read_index(prim->indices, ii);
            }
            out->index_buffer = upload_gpu_buffer(indices,
                idx_count * sizeof(uint16_t), SDL_GPU_BUFFERUSAGE_INDEX);
            free(indices);
        }

        /* texture from material */
        if (prim->material &&
            prim->material->has_pbr_metallic_roughness &&
            prim->material->pbr_metallic_roughness.base_color_texture.texture) {
            cgltf_image *img = prim->material->pbr_metallic_roughness
                                   .base_color_texture.texture->image;
            out->texture = extract_texture(img, asset_dir);
        }
    }

    /* compute bounding sphere from accumulated AABB */
    if (total_verts > 0) {
        result.bounds_center[0] = (bb_min[0] + bb_max[0]) * 0.5f;
        result.bounds_center[1] = (bb_min[1] + bb_max[1]) * 0.5f;
        result.bounds_center[2] = (bb_min[2] + bb_max[2]) * 0.5f;
        {
            float dx = bb_max[0] - bb_min[0];
            float dy = bb_max[1] - bb_min[1];
            float dz = bb_max[2] - bb_min[2];
            result.bounds_radius = sqrtf(dx*dx + dy*dy + dz*dz) * 0.5f;
        }
    }

    return result;
}

/* ── skeleton extraction ──────────────────────────────────────── */

static Skeleton extract_skeleton(cgltf_skin *skin, arena *ar) {
    Skeleton skel = {0};
    uint32_t jc = (uint32_t)skin->joints_count;
    uint32_t i;

    skel.joint_count = jc;

    skel.parent_indices    = (int16_t *)arena_alloc(ar, jc * sizeof(int16_t), 4, "skel_parents");
    skel.inverse_bind      = (Mat4 *)arena_alloc(ar, jc * sizeof(Mat4), 16, "skel_ibm");
    skel.rest_translations = (Vec3 *)arena_alloc(ar, jc * sizeof(Vec3), 16, "skel_rest_t");
    skel.rest_rotations    = (Quat *)arena_alloc(ar, jc * sizeof(Quat), 16, "skel_rest_r");
    skel.rest_scales       = (Vec3 *)arena_alloc(ar, jc * sizeof(Vec3), 16, "skel_rest_s");
    skel.joint_names       = (const char **)arena_alloc(ar, jc * sizeof(const char *), 8, "skel_names");

    /* read inverse bind matrices */
    if (skin->inverse_bind_matrices) {
        for (i = 0; i < jc; i++) {
            float ibm[16];
            cgltf_accessor_read_float(skin->inverse_bind_matrices, i, ibm, 16);
            skel.inverse_bind[i] = mat4_from_floats(ibm);
        }
    }

    /* per-joint: parent index + rest pose + name */
    for (i = 0; i < jc; i++) {
        cgltf_node *joint = skin->joints[i];
        const char *src_name = joint->name ? joint->name : "";
        uint32_t name_len = (uint32_t)strlen(src_name) + 1;
        char *dst_name = (char *)arena_alloc(ar, name_len, 1, "jname");
        uint32_t p;

        memcpy(dst_name, src_name, name_len);
        skel.joint_names[i] = dst_name;

        /* find parent index */
        skel.parent_indices[i] = -1;
        for (p = 0; p < jc; p++) {
            if (skin->joints[p] == joint->parent) {
                skel.parent_indices[i] = (int16_t)p;
                break;
            }
        }

        /* rest pose */
        if (joint->has_translation) {
            skel.rest_translations[i] = VEC3(
                joint->translation[0], joint->translation[1], joint->translation[2]);
        } else {
            skel.rest_translations[i] = VEC3(0, 0, 0);
        }

        if (joint->has_rotation) {
            skel.rest_rotations[i] = QUAT(
                joint->rotation[0], joint->rotation[1],
                joint->rotation[2], joint->rotation[3]);
        } else {
            skel.rest_rotations[i] = quat_identity();
        }

        if (joint->has_scale) {
            skel.rest_scales[i] = VEC3(
                joint->scale[0], joint->scale[1], joint->scale[2]);
        } else {
            skel.rest_scales[i] = VEC3(1, 1, 1);
        }
    }

    return skel;
}

/* ── animation clip extraction ────────────────────────────────── */

static AnimClip extract_anim_clip(cgltf_animation *anim, cgltf_skin *skin, arena *ar) {
    AnimClip clip = {0};
    uint32_t c;

    if (anim->name) {
        strncpy(clip.name, anim->name, sizeof(clip.name) - 1);
    }
    clip.channel_count = (uint32_t)anim->channels_count;
    clip.duration = 0.0f;

    clip.headers = (ChannelHeader *)arena_alloc(ar,
        clip.channel_count * sizeof(ChannelHeader), 4, "anim_headers");
    clip.times = (ChannelTimes *)arena_alloc(ar,
        clip.channel_count * sizeof(ChannelTimes), 8, "anim_times");
    clip.data = (ChannelData *)arena_alloc(ar,
        clip.channel_count * sizeof(ChannelData), 8, "anim_data");

    for (c = 0; c < clip.channel_count; c++) {
        cgltf_animation_channel *ch = &anim->channels[c];
        cgltf_animation_sampler *sampler = ch->sampler;
        uint16_t joint_index = 0;
        cgltf_size j;
        uint8_t prop = 0;
        uint8_t interp = 1;
        uint32_t kf_count;
        uint32_t k;
        float last;

        /* find joint index */
        for (j = 0; j < skin->joints_count; j++) {
            if (skin->joints[j] == ch->target_node) {
                joint_index = (uint16_t)j;
                break;
            }
        }

        /* property */
        switch (ch->target_path) {
        case cgltf_animation_path_type_translation: prop = 0; break;
        case cgltf_animation_path_type_rotation:    prop = 1; break;
        case cgltf_animation_path_type_scale:       prop = 2; break;
        default: continue;
        }

        /* interpolation */
        switch (sampler->interpolation) {
        case cgltf_interpolation_type_step:         interp = 0; break;
        case cgltf_interpolation_type_linear:       interp = 1; break;
        case cgltf_interpolation_type_cubic_spline: interp = 2; break;
        case cgltf_interpolation_type_max_enum:
        default:
            interp = 1;
            break;
        }

        clip.headers[c].joint_index = joint_index;
        clip.headers[c].property = prop;
        clip.headers[c].interpolation = interp;

        /* timestamps */
        kf_count = (uint32_t)sampler->input->count;
        clip.times[c].keyframe_count = kf_count;
        clip.times[c].timestamps = (float *)arena_alloc(ar,
            kf_count * sizeof(float), 4, "anim_ts");
        for (k = 0; k < kf_count; k++)
            cgltf_accessor_read_float(sampler->input, k, &clip.times[c].timestamps[k], 1);

        last = clip.times[c].timestamps[kf_count - 1];
        if (last > clip.duration) clip.duration = last;

        /* values */
        if (prop == 1) { /* rotation -> Quat */
            clip.data[c].quats = (Quat *)arena_alloc(ar,
                kf_count * sizeof(Quat), 16, "anim_rot");
            for (k = 0; k < kf_count; k++)
                cgltf_accessor_read_float(sampler->output, k,
                    &clip.data[c].quats[k].x, 4);
        } else { /* translation or scale -> Vec3 */
            clip.data[c].vec3s = (Vec3 *)arena_alloc(ar,
                kf_count * sizeof(Vec3), 16, "anim_vec3");
            for (k = 0; k < kf_count; k++)
                cgltf_accessor_read_float(sampler->output, k,
                    &clip.data[c].vec3s[k].x, 3);
        }
    }

    printf("  Animation '%s': %u channels, %.2fs\n",
           anim->name ? anim->name : "(unnamed)", clip.channel_count, clip.duration);
    return clip;
}

/* ── public API ───────────────────────────────────────────────── */

GltfModel load_glb(const char *path, arena *ar) {
    GltfModel model = {0};
    cgltf_options options = {0};
    cgltf_data *data = NULL;
    cgltf_result result;
    cgltf_skin *skin = NULL;
    cgltf_size ni;
    uint32_t total_prims = 0;
    char asset_dir[PATH_MAX];

    path_get_dirname(path, asset_dir, sizeof(asset_dir));

    result = cgltf_parse_file(&options, path, &data);
    if (result != cgltf_result_success) {
        fprintf(stderr, "Failed to parse GLB: %s\n", path);
        return model;
    }

    result = cgltf_load_buffers(&options, data, path);
    if (result != cgltf_result_success) {
        fprintf(stderr, "Failed to load GLB buffers: %s\n", path);
        cgltf_free(data);
        return model;
    }

    printf("Loaded GLB: %s (%zu meshes, %zu skins, %zu anims)\n",
           path, data->meshes_count, data->skins_count, data->animations_count);

    /* find the skin */
    for (ni = 0; ni < data->nodes_count; ni++) {
        if (data->nodes[ni].skin) { skin = data->nodes[ni].skin; break; }
    }
    if (!skin && data->skins_count > 0) skin = &data->skins[0];

    /* collect ALL mesh nodes (KayKit stores body parts as separate meshes) */
    for (ni = 0; ni < data->nodes_count; ni++) {
        if (data->nodes[ni].mesh)
            total_prims += (uint32_t)data->nodes[ni].mesh->primitives_count;
    }

    if (total_prims > 0) {
        float inv_skel_world[16];
        float bb_min[3] = { 1e30f, 1e30f, 1e30f };
        float bb_max[3] = {-1e30f,-1e30f,-1e30f};
        int has_bounds = 0;
        uint32_t prim_idx = 0;

        model.mesh.primitive_count = total_prims;
        model.mesh.primitives = (GltfPrimitive *)arena_alloc(ar,
            total_prims * sizeof(GltfPrimitive), 16, "mesh_primitives");

        /*
         * Our skinning pipeline (compute_world_transforms + compute_skinning_matrices)
         * operates in skeleton-local space: root joints get their local TRS directly,
         * not multiplied by the armature's world transform.  So mesh vertices must
         * also be in skeleton-local space.
         *
         * Transform each mesh node's vertices by:
         *   inv(skeleton_root_world) * mesh_node_world
         * This strips the armature's scene transform (e.g. Blender Z-up rotation)
         * and keeps only the relative offset from the skeleton root.
         */
        {
            /* find skeleton root node */
            cgltf_node *skel_root = NULL;
            if (skin && skin->skeleton) {
                skel_root = skin->skeleton;
            } else if (skin && skin->joints_count > 0) {
                /* walk up from first joint to find root */
                skel_root = skin->joints[0];
                while (skel_root->parent) skel_root = skel_root->parent;
            }

            if (skel_root) {
                float skel_world[16];
                cgltf_node_transform_world(skel_root, skel_world);
                invert_affine(skel_world, inv_skel_world);
                /* store armature transform so it can be used as model matrix */
                memcpy(model.armature_transform.m, skel_world, sizeof(float)*16);
            } else {
                /* no skeleton — identity */
                float id[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
                memcpy(inv_skel_world, id, sizeof(float)*16);
                model.armature_transform = mat4_identity();
            }
        }

        for (ni = 0; ni < data->nodes_count; ni++) {
            cgltf_node *node = &data->nodes[ni];
            float mesh_world[16], mesh_to_skel[16];
            GltfMesh sub;
            uint32_t p;

            if (!node->mesh) continue;

            /* mesh_to_skeleton = inv(skel_world) * mesh_world */
            cgltf_node_transform_world(node, mesh_world);
            mul_mat4(inv_skel_world, mesh_world, mesh_to_skel);

            sub = extract_mesh(node->mesh, mesh_to_skel, asset_dir, ar);
            if (sub.primitive_count > 0) {
                float r = sub.bounds_radius > 0.0f ? sub.bounds_radius : 0.0f;
                float sx0 = sub.bounds_center[0] - r;
                float sy0 = sub.bounds_center[1] - r;
                float sz0 = sub.bounds_center[2] - r;
                float sx1 = sub.bounds_center[0] + r;
                float sy1 = sub.bounds_center[1] + r;
                float sz1 = sub.bounds_center[2] + r;
                if (sx0 < bb_min[0]) bb_min[0] = sx0;
                if (sy0 < bb_min[1]) bb_min[1] = sy0;
                if (sz0 < bb_min[2]) bb_min[2] = sz0;
                if (sx1 > bb_max[0]) bb_max[0] = sx1;
                if (sy1 > bb_max[1]) bb_max[1] = sy1;
                if (sz1 > bb_max[2]) bb_max[2] = sz1;
                has_bounds = 1;
            }
            for (p = 0; p < sub.primitive_count; p++) {
                model.mesh.primitives[prim_idx++] = sub.primitives[p];
            }
        }
        model.mesh.primitive_count = prim_idx;
        if (has_bounds) {
            float dx, dy, dz;
            model.mesh.bounds_center[0] = (bb_min[0] + bb_max[0]) * 0.5f;
            model.mesh.bounds_center[1] = (bb_min[1] + bb_max[1]) * 0.5f;
            model.mesh.bounds_center[2] = (bb_min[2] + bb_max[2]) * 0.5f;
            dx = bb_max[0] - bb_min[0];
            dy = bb_max[1] - bb_min[1];
            dz = bb_max[2] - bb_min[2];
            model.mesh.bounds_radius = sqrtf(dx*dx + dy*dy + dz*dz) * 0.5f;
        }
    }

    if (skin) {
        model.skeleton = extract_skeleton(skin, ar);
    }

    if (data->animations_count > 0 && skin) {
        uint32_t i;
        model.clip_count = (uint32_t)data->animations_count;
        model.clips = (AnimClip *)arena_alloc(ar,
            model.clip_count * sizeof(AnimClip), 8, "anim_clips");
        for (i = 0; i < model.clip_count; i++) {
            model.clips[i] = extract_anim_clip(&data->animations[i], skin, ar);
            printf("  clip %u: \"%s\" (%.2fs)\n", i, model.clips[i].name, model.clips[i].duration);
        }
    }

    cgltf_free(data);
    return model;
}

/* Load animations from a separate GLB and attach to existing model */
void load_animations_glb(const char *path, GltfModel *model, arena *ar) {
    cgltf_options options = {0};
    cgltf_data *data = NULL;
    cgltf_result result;
    cgltf_skin *skin;
    uint32_t anim_jc, char_jc;
    uint16_t *remap;
    uint32_t remap_misses = 0;
    uint32_t ai;

    result = cgltf_parse_file(&options, path, &data);
    if (result != cgltf_result_success) {
        fprintf(stderr, "Failed to parse anim GLB: %s\n", path);
        return;
    }

    result = cgltf_load_buffers(&options, data, path);
    if (result != cgltf_result_success) {
        fprintf(stderr, "Failed to load anim GLB buffers: %s\n", path);
        cgltf_free(data);
        return;
    }

    printf("Loaded anim GLB: %s (%zu animations)\n", path, data->animations_count);

    /* Use first skin in the anim file for joint mapping */
    skin = data->skins_count > 0 ? &data->skins[0] : NULL;

    if (!skin) {
        fprintf(stderr, "  No skin in anim file, cannot map joints\n");
        cgltf_free(data);
        return;
    }

    /*
     * Build remap table: anim_joint_index -> character_joint_index.
     * The two files may store joints in different order within their skins.
     * We match by joint node NAME to get the correct mapping.
     */
    anim_jc = (uint32_t)skin->joints_count;
    char_jc = model->skeleton.joint_count;
    remap = (uint16_t *)malloc(anim_jc * sizeof(uint16_t));

    for (ai = 0; ai < anim_jc; ai++) {
        const char *anim_name = skin->joints[ai]->name ? skin->joints[ai]->name : "";
        int found = 0;
        uint32_t ci;

        remap[ai] = 0; /* fallback to root */
        for (ci = 0; ci < char_jc; ci++) {
            if (strcmp(anim_name, model->skeleton.joint_names[ci]) == 0) {
                remap[ai] = (uint16_t)ci;
                found = 1;
                break;
            }
        }
        if (!found) remap_misses++;
    }
    printf("  Joint remap: %u anim -> %u char (%u misses)\n", anim_jc, char_jc, remap_misses);

    if (data->animations_count > 0) {
        uint32_t i;
        uint32_t old_count = model->clip_count;
        uint32_t add_count = (uint32_t)data->animations_count;
        uint32_t new_count = old_count + add_count;
        AnimClip *new_clips = (AnimClip *)arena_alloc(ar,
            new_count * sizeof(AnimClip), 8, "anim_clips");
        if (old_count > 0 && model->clips) {
            memcpy(new_clips, model->clips, old_count * sizeof(AnimClip));
        }
        model->clips = new_clips;
        model->clip_count = new_count;
        for (i = 0; i < add_count; i++) {
            AnimClip *cl;
            uint32_t c;

            model->clips[old_count + i] = extract_anim_clip(&data->animations[i], skin, ar);

            /* Remap all channel joint indices from anim ordering -> character ordering */
            cl = &model->clips[old_count + i];
            for (c = 0; c < cl->channel_count; c++) {
                uint16_t old_idx = cl->headers[c].joint_index;
                if (old_idx < anim_jc)
                    cl->headers[c].joint_index = remap[old_idx];
            }
        }
    }

    free(remap);
    cgltf_free(data);
}
