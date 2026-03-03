#ifndef EXTERNALS_H
#define EXTERNALS_H

#include <export.h>

/* Externals owns memory — parameterless API */
EXPORT int  init_externals(const char *project_path);
typedef int (*init_externals_func)(const char *project_path);

EXPORT int  update_externals(void);
typedef int (*update_externals_func)(void);

EXPORT void end_externals(void);
typedef void (*end_externals_func)(void);

EXPORT void init_engine(void);
typedef void (*init_engine_func)(void);

EXPORT void destroy_engine(void);
typedef void (*destroy_engine_func)(void);

EXPORT void init_editor(void);
typedef void (*init_editor_func)(void);

EXPORT void destroy_editor(void);
typedef void (*destroy_editor_func)(void);

EXPORT void reload_project(void);
typedef void (*reload_project_func)(void);

/* Engine DLL callback assignment — new typed function pointers */
DECLARE_FUNC_VOID_pENGINE_INIT(assign_init)
DECLARE_FUNC_VOID_pENGINE_DESTROY(assign_destroy)
DECLARE_FUNC_VOID_pENGINE_UPDATE(assign_update)

/* Editor DLL callbacks (loaded by core.c, dispatched through externals) */
DECLARE_FUNC_VOID_pEDITOR_INIT(assign_editor_init)
DECLARE_FUNC_VOID_pEDITOR_DESTROY(assign_editor_destroy)
DECLARE_FUNC_VOID_pEDITOR_UPDATE(assign_editor_update)
DECLARE_FUNC_VOID_pEDITOR_HANDLE_EVENT(assign_editor_handle_event)

#endif
