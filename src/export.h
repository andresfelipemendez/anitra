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
   Without this, TCC creates a separate prototype-scope 'struct memory'
   for each function declaration parameter list.                      */
struct memory;

typedef void (*init)(struct memory *g);
typedef void (*destroy)(struct memory *g);
typedef void (*update)(struct memory *g);
typedef int (*handle_event)(struct memory *g, void *event);

/* Type aliases for hot-reload safe function pointers */
typedef init init_func;
typedef destroy destroy_func;
typedef update update_func;
typedef handle_event handle_event_func;

#define HOTRELOAD_EVENT_NAME        "Global\\ReloadEvent"
  EXPORT void func();                                                          \
  typedef void (*func##_func)();

#define DECLARE_FUNC_VOID_pGAME(func)                                          \
  EXPORT void func(struct memory *g);                                            \
  typedef void (*func##_func)(struct memory * g);

#define DECLARE_FUNC_VOID_pCHAR(func)                                          \
  EXPORT void func(const char *str);                                           \
  typedef void (*func##_func)(const char *str);

#define DECLARE_FUNC_INT_pGAME(func)                                           \
  EXPORT int func(struct memory *g);                                             \
  typedef int (*func##_func)(struct memory * g);

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
