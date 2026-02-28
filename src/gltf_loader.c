#include "game.h"
#include <stdio.h>
#include <string.h>

/* Forward declarations from externals */
extern void gltf_set_gpu_device(void *dev);
static SDL_GPUDevice *g_gltf_gpu_device = NULL;

void gltf_set_gpu_device(void *dev) {
    g_gltf_gpu_device = (SDL_GPUDevice *)dev;
}

/* Simple glTF loader stub - would use cgltf library in full implementation */
GltfModel load_glb(const char *path, arena *a) {
    GltfModel model = {0};
    
    /* Stub implementation - in real code, this would:
       1. Read file into buffer
       2. Parse using cgltf
       3. Extract meshes, skeletons, animations
       4. Upload to GPU buffers
       5. Populate GltfModel structure */
    
    fprintf(stderr, "Warning: gltf_loader stub - %s not loaded\n", path);
    return model;
}

void load_animations_glb(const char *path, GltfModel *model, arena *a) {
    if (!model || !a) return;
    
    /* Stub implementation */
    fprintf(stderr, "Warning: load_animations_glb stub - %s not loaded\n", path);
}
