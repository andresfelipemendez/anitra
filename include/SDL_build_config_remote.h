/*
 * SDL_build_config_remote.h - fixups for cross-compiling SDL3 with zig cc
 *
 * Included via -include before any SDL3 source. Pulls in the real Linux
 * config then undefs things that don't work with zig's bundled sysroot.
 */
#include "SDL_build_config_linux.h"

/* zig's bundled glibc is older than the Pi's — strlcpy/strlcat missing */
#undef HAVE_STRLCPY
#undef HAVE_STRLCAT

/* zig's glibc declares getresuid/getresgid — tell SDL not to provide fallbacks */
#ifndef HAVE_GETRESUID
#define HAVE_GETRESUID 1
#endif
#ifndef HAVE_GETRESGID
#define HAVE_GETRESGID 1
#endif
