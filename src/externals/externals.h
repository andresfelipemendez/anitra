#ifndef EXTERNALS_H
#define EXTERNALS_H

#include <export.h>



DECLARE_FUNC_INT_pGAME(init_externals)
DECLARE_FUNC_VOID_pGAME(update_externals)
DECLARE_FUNC_VOID_pGAME(end_externals)

DECLARE_FUNC_VOID_pHOTRELOADABLE_IMGUI_DRAW(assign_hotreloadable)

#endif