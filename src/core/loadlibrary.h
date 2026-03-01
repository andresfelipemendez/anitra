#ifndef LOADLIBRARY_H
#define LOADLIBRARY_H

#include "error_value.h"

#ifdef __cplusplus
extern "C" {
#endif

void *loadlibrary(const char *libname);
void unloadlibrary(void *lib);
void *getfunction(void *lib, const char *funcname);
int copylibrary(const char *srcname, const char *dstname);
int copylibrary_with_error(const char *srcname, const char *dstname, error_value *out_error);

#ifdef __cplusplus
}
#endif

#endif // LOADLIBRARY_H
