#ifndef EXTERNALS_H
#define EXTERNALS_H

#include <export.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

DECLARE_FUNC_INT_pGAME(init_externals)
DECLARE_FUNC_VOID_pGAME(update_externals)
DECLARE_FUNC_VOID_pGAME(end_externals)

DECLARE_FUNC_VOID_pCHAR(CallFromOutsideBegin)

#endif