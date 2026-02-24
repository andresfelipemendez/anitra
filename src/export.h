#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif

struct game;

typedef void (*init)(struct game *g);
typedef void (*destroy)(struct game *g);
typedef void (*update)(struct game *g);

#define HOTRELOAD_EVENT_NAME "Global\\ReloadEvent"

#define DECLARE_FUNC_VOID(func)                                                \
  void func();                                                                 \
  typedef void (*func##_func)();

#define DECLARE_FUNC_VOID_pGAME(func)                                          \
  void func(struct game *g);                                                   \
  typedef void (*func##_func)(struct game * g);

#define DECLARE_FUNC_VOID_pCHAR(func)                                          \
  void func(const char *str);                                                  \
  typedef void (*func##_func)(const char *str);

#define DECLARE_FUNC_INT_pGAME(func)                                           \
  int func(struct game *g);                                                    \
  typedef int (*func##_func)(struct game * g);

#define DECLARE_FUNC_VOID_pINIT(func)                       \
  void func(init g);                                        \
  typedef void (*func##_func)(init g);

#define DECLARE_FUNC_VOID_pDESTROY(func)                       \
  void func(destroy g);                                        \
  typedef void (*func##_func)(destroy g);

#define DECLARE_FUNC_VOID_pUPDATE(func)                       \
  void func(update g);                                        \
  typedef void (*func##_func)(update g);
