#ifndef DRAW_PROCESSOR_H
#define DRAW_PROCESSOR_H

#include "draw_list.h"

typedef struct SDL_GPUDevice SDL_GPUDevice;
typedef struct SDL_GPUCommandBuffer SDL_GPUCommandBuffer;
typedef struct SDL_GPUBuffer SDL_GPUBuffer;

typedef struct sprite_vertex {
    float x, y;
    float u, v;
    float r, g, b, a;
} sprite_vertex;

typedef struct draw_upload_buffers {
    SDL_GPUBuffer *sprite_gpu_buf;
    SDL_GPUBuffer *line_gpu_buf;
    int sprite_count;
    int line_count;
    int line_vertex_count;
} draw_upload_buffers;

void draw_upload_init(draw_upload_buffers *out);
void draw_upload_build(SDL_GPUDevice *gpu_device,
                       SDL_GPUCommandBuffer *cmd_buf,
                       const draw_list *dl,
                       draw_upload_buffers *out);
void draw_upload_release(SDL_GPUDevice *gpu_device, draw_upload_buffers *buffers);

#endif /* DRAW_PROCESSOR_H */
