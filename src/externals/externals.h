#ifndef EXTERNALS_H
#define EXTERNALS_H

#include <export.h>

/* Externals dispatch wrappers — take full memory* */
DECLARE_FUNC_INT_pGAME(init_externals)
DECLARE_FUNC_VOID_pGAME(update_externals)
DECLARE_FUNC_VOID_pGAME(end_externals)
DECLARE_FUNC_VOID_pGAME(init_engine)
DECLARE_FUNC_VOID_pGAME(destroy_engine)

/* Engine DLL callback assignment — new typed function pointers */
DECLARE_FUNC_VOID_pENGINE_INIT(assign_init)
DECLARE_FUNC_VOID_pENGINE_DESTROY(assign_destroy)
DECLARE_FUNC_VOID_pENGINE_UPDATE(assign_update)

/* Editor DLL callbacks (loaded by core.c, dispatched through externals) */
DECLARE_FUNC_VOID_pGAME(init_editor)
DECLARE_FUNC_VOID_pGAME(destroy_editor)
DECLARE_FUNC_VOID_pEDITOR_INIT(assign_editor_init)
DECLARE_FUNC_VOID_pEDITOR_DESTROY(assign_editor_destroy)
DECLARE_FUNC_VOID_pEDITOR_UPDATE(assign_editor_update)
DECLARE_FUNC_VOID_pEDITOR_HANDLE_EVENT(assign_editor_handle_event)

#endif
