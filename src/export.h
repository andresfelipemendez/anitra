#ifdef _WIN32
	#define EXPORT extern "C" __declspec(dllexport)
#else
	#define EXPORT
#endif

typedef void (*hotreloadable_imgui_draw_func)(struct game* g);


#define HOTRELOAD_EVENT_NAME "Global\\ReloadEvent"

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

#define DECLARE_FUNC_VOID_pIMGUICONTEXT(func) \
    EXPORT void func(struct ImGuiContext* g); \
    typedef void (*func##_func)(struct ImGuiContext* g);

#define DECLARE_FUNC_VOID_pHOTRELOADABLE_IMGUI_DRAW(func) \
    EXPORT void func(hotreloadable_imgui_draw_func g); \
    typedef void (*func##_func)(hotreloadable_imgui_draw_func g);



