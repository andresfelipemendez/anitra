#ifndef EXPORT_H
#define EXPORT_H

#ifdef _WIN32
#ifdef __cplusplus
#define EXPORT extern "C" __declspec(dllexport)
#else
#define EXPORT __declspec(dllexport)
#endif
#else
#ifdef __cplusplus
#define EXPORT extern "C"
#else
#define EXPORT
#endif
#endif

/* Forward-declare so all prototypes share the file-scope tag.
   Without this, TCC creates a separate prototype-scope 'struct game'
   for each function declaration parameter list.                      */
struct game;

typedef void (*init)(struct game *g);
typedef void (*destroy)(struct game *g);
typedef void (*update)(struct game *g);
typedef void (*handle_event)(struct game *g, void *event);

#define HOTRELOAD_EVENT_NAME        "Global\\ReloadEvent"
#define HOTRELOAD_EDITOR_EVENT_NAME "Global\\ReloadEditorEvent"
#define HOTRELOAD_CORE_EVENT_NAME   "Global\\ReloadCoreEvent"

#define DECLARE_FUNC_VOID(func)                                                \
  EXPORT void func();                                                          \
  typedef void (*func##_func)();

#define DECLARE_FUNC_VOID_pGAME(func)                                          \
  EXPORT void func(struct game *g);                                            \
  typedef void (*func##_func)(struct game * g);

#define DECLARE_FUNC_VOID_pCHAR(func)                                          \
  EXPORT void func(const char *str);                                           \
  typedef void (*func##_func)(const char *str);

#define DECLARE_FUNC_INT_pGAME(func)                                           \
  EXPORT int func(struct game *g);                                             \
  typedef int (*func##_func)(struct game * g);

#define DECLARE_FUNC_VOID_pINIT(func)                       \
  EXPORT void func(init g);                                 \
  typedef void (*func##_func)(init g);

#define DECLARE_FUNC_VOID_pDESTROY(func)                       \
  EXPORT void func(destroy g);                                 \
  typedef void (*func##_func)(destroy g);

#define DECLARE_FUNC_VOID_pUPDATE(func)                       \
  EXPORT void func(update g);                                 \
  typedef void (*func##_func)(update g);

#define DECLARE_FUNC_VOID_pHANDLE_EVENT(func)                 \
  EXPORT void func(handle_event g);                           \
  typedef void (*func##_func)(handle_event g);

#endif /* EXPORT_H */
