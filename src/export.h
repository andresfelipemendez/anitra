#ifdef _WIN32
	#define EXPORT extern "C" __declspec(dllexport)
#else
	#define EXPORT
#endif

typedef void (*ImGuiUpdateFunc)(void);
typedef void (*ImGuiRenderFunc)(void);

#define DECLARE_FUNC_VOID(func) \
    EXPORT void func(); \
    typedef void (*func##_func)();

#define DECLARE_FUNC_VOID_pGAME(func) \
    EXPORT void func(struct game* g); \
    typedef void (*func##_func)(struct game* g);

#define DECLARE_FUNC_INT_pGAME(func) \
    EXPORT int func(struct game* g); \
    typedef int (*func##_func)(struct game* g);


