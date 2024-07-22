#ifndef LOADLIBRARY_H
#define LOADLIBRARY_H

void* loadlibrary(const char* libname);
void* getfunction(void* lib, const char* funcname);

#endif // LOADLIBRARY_H