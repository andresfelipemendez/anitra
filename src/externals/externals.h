#ifndef EXTERNALS_H
#define EXTERNALS_H

#include "../export.h"

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

DECLARE_FUNC_INT_pGAME(init_externals)
DECLARE_FUNC_VOID_pGAME(update_externals)
DECLARE_FUNC_VOID_pGAME(end_externals)

DECLARE_FUNC_VOID_IMGUIUPDATEFUNC_IMGUIRENDERFUNC(update_imgui_functions)
DECLARE_FUNC_VOID_pCHAR(CallFromOutsideBegin)
DECLARE_FUNC_pIMGUICONTEXT(GetImguiContext)
#endif

