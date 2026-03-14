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
   Without this, TCC creates a separate prototype-scope 'struct game_state'
   for each function declaration parameter list.                      */
struct game_state;
struct editor_state;

/* Engine function pointers — game_state only */
typedef void (*engine_init_fn)(struct game_state *gs);
typedef void (*engine_destroy_fn)(struct game_state *gs);
typedef void (*engine_update_fn)(struct game_state *gs);

/* Editor function pointers — game_state + editor_state */
typedef void (*editor_init_fn)(struct game_state *gs, struct editor_state *es);
typedef void (*editor_destroy_fn)(struct game_state *gs, struct editor_state *es);
typedef void (*editor_update_fn)(struct game_state *gs, struct editor_state *es);
typedef int  (*editor_handle_event_fn)(struct game_state *gs, struct editor_state *es, void *event);

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

/* Assign macros for engine/editor function pointer types */
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

/* Profiler zone function pointers — called by engine.dll, implemented in externals.dll */
typedef void (*cache_zone_begin_fn)(const char *name);
typedef void (*cache_zone_end_fn)(void);
typedef void (*cpu_zone_begin_fn)(const char *name);
typedef void (*cpu_zone_end_fn)(void);

#endif /* EXPORT_H */
