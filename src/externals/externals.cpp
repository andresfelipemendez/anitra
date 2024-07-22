#include "externals.h"
#include "../game.h"
#include <stdio.h>
#include <GLFW/glfw3.h>

EXPORT int init_externals(game* g) {

    /* Initialize the library */
    if (!glfwInit())
        return -1;

    /* Create a windowed mode window and its OpenGL context */
    g->window = glfwCreateWindow(640, 480, "Hello World", NULL, NULL);
    if (!g->window)
    {
        glfwTerminate();
        return -1;
    }

    /* Make the window's context current */
    glfwMakeContextCurrent(g->window);

    return 1;
}
