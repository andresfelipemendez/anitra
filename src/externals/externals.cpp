#include "externals.h"
#include "../game.h"
#include <stdio.h>
#include <GLFW/glfw3.h>

EXPORT int init_externals(game* g) {
    if (!glfwInit())
        return -1;

    g->window = glfwCreateWindow(640, 480, "Hello World", NULL, NULL);
    if (!g->window)
    {
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(g->window);

    g->play = true;

    return 1;
}

EXPORT void update_externals(game* g){

    glClear(GL_COLOR_BUFFER_BIT);

    glfwSwapBuffers(g->window);
    glfwPollEvents();
    g->play = !glfwWindowShouldClose(g->window);

}

EXPORT void end_externals(game* g){
}
