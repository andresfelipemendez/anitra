#include "draw_processor.h"

#include <string.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

void draw_upload_init(draw_upload_buffers *out) {
    if (!out) return;
    out->sprite_gpu_buf = NULL;
    out->line_gpu_buf = NULL;
    out->sprite_count = 0;
    out->line_count = 0;
    out->line_vertex_count = 0;
}

void draw_upload_build(SDL_GPUDevice *gpu_device,
                       SDL_GPUCommandBuffer *cmd_buf,
                       const draw_list *dl,
                       draw_upload_buffers *out) {
    int sprite_count;
    int sprite_vertex_count;
    Uint32 sprite_buf_size;
    int line_count;
    int line_vertex_count;
    Uint32 line_buf_size;

    if (!gpu_device || !cmd_buf || !dl || !out) return;

    draw_upload_init(out);

    sprite_count = dl->sprite_count;
    sprite_vertex_count = sprite_count * 6;
    sprite_buf_size = (Uint32)(sprite_vertex_count * (int)sizeof(sprite_vertex));

    if (sprite_count > 0 && dl->sprites && sprite_buf_size > 0) {
        SDL_GPUTransferBufferCreateInfo tbuf_info = {0};
        SDL_GPUTransferBuffer *sprite_transfer;
        sprite_vertex *verts;
        SDL_GPUBufferCreateInfo buf_info;
        SDL_GPUCopyPass *copy_pass;
        SDL_GPUTransferBufferLocation src_loc;
        SDL_GPUBufferRegion dst_region;
        int i;

        tbuf_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbuf_info.size = sprite_buf_size;
        tbuf_info.props = 0;

        sprite_transfer = SDL_CreateGPUTransferBuffer(gpu_device, &tbuf_info);
        verts = (sprite_vertex *)SDL_MapGPUTransferBuffer(gpu_device, sprite_transfer, false);

        for (i = 0; i < sprite_count; i++) {
            const draw_command *cmd = &dl->sprites[i];
            float half_w = cmd->width * 0.5f;
            float half_h = cmd->height * 0.5f;
            float x0 = cmd->x - half_w;
            float y0 = cmd->y - half_h;
            float x1 = cmd->x + half_w;
            float y1 = cmd->y + half_h;
            float u0 = cmd->uv_x;
            float v0 = cmd->uv_y;
            float u1 = cmd->uv_x + cmd->uv_w;
            float v1 = cmd->uv_y + cmd->uv_h;
            float r = cmd->tint_r;
            float g = cmd->tint_g;
            float b = cmd->tint_b;
            float a = cmd->tint_a;
            sprite_vertex *v = &verts[i * 6];

            v[0] = (sprite_vertex){x0, y1, u0, v0, r, g, b, a};
            v[1] = (sprite_vertex){x1, y1, u1, v0, r, g, b, a};
            v[2] = (sprite_vertex){x1, y0, u1, v1, r, g, b, a};
            v[3] = (sprite_vertex){x0, y1, u0, v0, r, g, b, a};
            v[4] = (sprite_vertex){x1, y0, u1, v1, r, g, b, a};
            v[5] = (sprite_vertex){x0, y0, u0, v1, r, g, b, a};
        }

        SDL_UnmapGPUTransferBuffer(gpu_device, sprite_transfer);

        memset(&buf_info, 0, sizeof(buf_info));
        buf_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        buf_info.size = sprite_buf_size;
        buf_info.props = 0;
        out->sprite_gpu_buf = SDL_CreateGPUBuffer(gpu_device, &buf_info);

        copy_pass = SDL_BeginGPUCopyPass(cmd_buf);
        memset(&src_loc, 0, sizeof(src_loc));
        src_loc.transfer_buffer = sprite_transfer;
        memset(&dst_region, 0, sizeof(dst_region));
        dst_region.buffer = out->sprite_gpu_buf;
        dst_region.size = sprite_buf_size;
        SDL_UploadToGPUBuffer(copy_pass, &src_loc, &dst_region, false);
        SDL_EndGPUCopyPass(copy_pass);
        SDL_ReleaseGPUTransferBuffer(gpu_device, sprite_transfer);

        out->sprite_count = sprite_count;
    }

    line_count = dl->line_count;
    line_vertex_count = line_count * LINE_VERTS_PER_LINE;
    line_buf_size = (Uint32)(line_vertex_count * (int)sizeof(line_vert));

    if (line_count > 0 && dl->line_verts && line_buf_size > 0) {
        SDL_GPUTransferBufferCreateInfo tbuf_info = {0};
        SDL_GPUTransferBuffer *line_transfer;
        void *mapped;
        SDL_GPUBufferCreateInfo buf_info;
        SDL_GPUCopyPass *copy_pass;
        SDL_GPUTransferBufferLocation src_loc;
        SDL_GPUBufferRegion dst_region;

        tbuf_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbuf_info.size = line_buf_size;
        tbuf_info.props = 0;

        line_transfer = SDL_CreateGPUTransferBuffer(gpu_device, &tbuf_info);
        mapped = SDL_MapGPUTransferBuffer(gpu_device, line_transfer, false);
        memcpy(mapped, dl->line_verts, line_buf_size);
        SDL_UnmapGPUTransferBuffer(gpu_device, line_transfer);

        memset(&buf_info, 0, sizeof(buf_info));
        buf_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        buf_info.size = line_buf_size;
        buf_info.props = 0;
        out->line_gpu_buf = SDL_CreateGPUBuffer(gpu_device, &buf_info);

        copy_pass = SDL_BeginGPUCopyPass(cmd_buf);
        memset(&src_loc, 0, sizeof(src_loc));
        src_loc.transfer_buffer = line_transfer;
        memset(&dst_region, 0, sizeof(dst_region));
        dst_region.buffer = out->line_gpu_buf;
        dst_region.size = line_buf_size;
        SDL_UploadToGPUBuffer(copy_pass, &src_loc, &dst_region, false);
        SDL_EndGPUCopyPass(copy_pass);
        SDL_ReleaseGPUTransferBuffer(gpu_device, line_transfer);

        out->line_count = line_count;
        out->line_vertex_count = line_vertex_count;
    }
}

void draw_upload_release(SDL_GPUDevice *gpu_device, draw_upload_buffers *buffers) {
    if (!gpu_device || !buffers) return;
    if (buffers->sprite_gpu_buf) {
        SDL_ReleaseGPUBuffer(gpu_device, buffers->sprite_gpu_buf);
        buffers->sprite_gpu_buf = NULL;
    }
    if (buffers->line_gpu_buf) {
        SDL_ReleaseGPUBuffer(gpu_device, buffers->line_gpu_buf);
        buffers->line_gpu_buf = NULL;
    }
    buffers->sprite_count = 0;
    buffers->line_count = 0;
    buffers->line_vertex_count = 0;
}
