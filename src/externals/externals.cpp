#include "externals/externals.h"
#include <stdio.h>
#include <GLFW/glfw3.h>

EXPORT int init_externals() {
    if(!glfwInit()){
        return -1;
    }

    // Decide GL+GLSL versions
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);

    return 1;
}
