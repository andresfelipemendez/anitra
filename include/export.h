#ifdef _WIN32
	#define EXPORT __declspec(dllexport)
#else
	#define EXPORT
#endif


#define DECLARE_FUNC(func) \
    EXPORT void func(); \
    typedef void (*func##_func)()


