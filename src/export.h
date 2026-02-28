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
struct game_state;
struct editor_state;

/* Legacy function pointers — externals dispatch wrappers take memory* */
typedef void (*init)(struct memory *g);
typedef void (*destroy)(struct memory *g);
typedef void (*update)(struct memory *g);
typedef int (*handle_event)(struct memory *g, void *event);

/* Engine function pointers — game_state only */
typedef void (*engine_init_fn)(struct game_state *gs);
typedef void (*engine_destroy_fn)(struct game_state *gs);
typedef void (*engine_update_fn)(struct game_state *gs);

/* Editor function pointers — game_state + editor_state */
typedef void (*editor_init_fn)(struct game_state *gs, struct editor_state *es);
typedef void (*editor_destroy_fn)(struct game_state *gs, struct editor_state *es);
typedef void (*editor_update_fn)(struct game_state *gs, struct editor_state *es);
typedef int  (*editor_handle_event_fn)(struct game_state *gs, struct editor_state *es, void *event);

#ifdef _WIN32
#define HOTRELOAD_EVENT_NAME "Global\\ReloadEvent"
#endif

/* Type aliases for hot-reload safe function pointers */
typedef init init_func;
typedef destroy destroy_func;
typedef update update_func;
typedef handle_event handle_event_func;

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

/* Engine: game_state only */
#define DECLARE_ENGINE_FUNC_VOID(func)                                         \
  EXPORT void func(struct game_state *gs);                                     \
  typedef void (*func##_func)(struct game_state *gs);

/* Editor: game_state + editor_state */
#define DECLARE_EDITOR_FUNC_VOID(func)                                         \
  EXPORT void func(struct game_state *gs, struct editor_state *es);            \
  typedef void (*func##_func)(struct game_state *gs, struct editor_state *es);

#define DECLARE_EDITOR_FUNC_INT(func)                                          \
  EXPORT int func(struct game_state *gs, struct editor_state *es, void *event); \
  typedef int (*func##_func)(struct game_state *gs, struct editor_state *es, void *event);

/* Assign macros for new engine/editor function pointer types */
#define DECLARE_FUNC_VOID_pENGINE_INIT(func)                 \
  EXPORT void func(engine_init_fn f);                        \
  typedef void (*func##_func)(engine_init_fn f);

#define DECLARE_FUNC_VOID_pENGINE_DESTROY(func)              \
  EXPORT void func(engine_destroy_fn f);                     \
  typedef void (*func##_func)(engine_destroy_fn f);

#define DECLARE_FUNC_VOID_pENGINE_UPDATE(func)               \
  EXPORT void func(engine_update_fn f);                      \
  typedef void (*func##_func)(engine_update_fn f);

#define DECLARE_FUNC_VOID_pEDITOR_INIT(func)                 \
  EXPORT void func(editor_init_fn f);                        \
  typedef void (*func##_func)(editor_init_fn f);

#define DECLARE_FUNC_VOID_pEDITOR_DESTROY(func)              \
  EXPORT void func(editor_destroy_fn f);                     \
  typedef void (*func##_func)(editor_destroy_fn f);

#define DECLARE_FUNC_VOID_pEDITOR_UPDATE(func)               \
  EXPORT void func(editor_update_fn f);                      \
  typedef void (*func##_func)(editor_update_fn f);

#define DECLARE_FUNC_VOID_pEDITOR_HANDLE_EVENT(func)         \
  EXPORT void func(editor_handle_event_fn f);                \
  typedef void (*func##_func)(editor_handle_event_fn f);

#endif /* EXPORT_H */
