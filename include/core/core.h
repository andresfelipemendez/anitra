#ifndef CORE_H
#define CORE_H


#ifdef _WIN32
    #define EXPORT __declspec(dllexport)
#else
    #define EXPORT
#endif

#define DECLARE_FUNC(func) \
    EXPORT void func(); \
    typedef void (*func##_func)()

DECLARE_FUNC(init);

#endif // CORE_H
