#ifndef EXTERNALS_H
#define EXTERNALS_H

#include <export.h>

DECLARE_FUNC_INT_pGAME(init_externals) 
DECLARE_FUNC_VOID_pGAME(update_externals)
DECLARE_FUNC_VOID_pGAME(end_externals)
DECLARE_FUNC_VOID_pGAME(init_engine)
DECLARE_FUNC_VOID_pGAME(destroy_engine)

DECLARE_FUNC_VOID_pINIT(assign_init)
DECLARE_FUNC_VOID_pDESTROY(assign_destroy)
DECLARE_FUNC_VOID_pUPDATE(assign_update)

#endif
