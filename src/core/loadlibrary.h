#ifndef LOADLIBRARY_H
#define LOADLIBRARY_H

#ifdef __cplusplus
extern "C" {
#endif

void *loadlibrary(const char *libname);
void unloadlibrary(void *lib);
void *getfunction(void *lib, const char *funcname);
int copylibrary(const char *srcname, const char *dstname);

#ifdef __cplusplus
}
#endif

#endif // LOADLIBRARY_H
