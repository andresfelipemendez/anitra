#ifdef _WIN32
	#define EXPORT extern "C" __declspec(dllexport)
#else
	#define EXPORT
#endif
struct ImGuiContext;
typedef void (*ImGuiUpdateFunc)(ImGuiContext*);
typedef void (*ImGuiRenderFunc)(void);

#define DECLARE_FUNC_VOID(func) \
    EXPORT void func(); \
    typedef void (*func##_func)();

#define DECLARE_FUNC_VOID_pGAME(func) \
    EXPORT void func(struct game* g); \
    typedef void (*func##_func)(struct game* g);

#define DECLARE_FUNC_VOID_pCHAR(func) \
    EXPORT void func(const char* str); \
    typedef void (*func##_func)(const char* str);

#define DECLARE_FUNC_INT_pGAME(func) \
    EXPORT int func(struct game* g); \
    typedef int (*func##_func)(struct game* g);

#define DECLARE_FUNC_VOID_IMGUIUPDATEFUNC_IMGUIRENDERFUNC(func) \
    EXPORT void func(ImGuiUpdateFunc updateFunc); \
    typedef void (*func##_func)(ImGuiUpdateFunc updateFunc);

#define DECLARE_FUNC_pIMGUICONTEXT(func) \
    EXPORT struct ImGuiContext* func(); \
    typedef struct ImGuiContext* (*func##_func)();

#define DECLARE_FUNC_VOID_pIMGUICONTEXT(func) \
     \
    EXPORT void func(ImGuiContext* ctx); \
    typedef void (*func##_func)(ImGuiContext* ctx);


