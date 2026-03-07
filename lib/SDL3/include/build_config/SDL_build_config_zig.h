/*
 * SDL_build_config_zig.h - fixups for building SDL3 with zig cc (MinGW)
 *
 * Zig's bundled MinGW headers define _WIN32_MAXVER=0x0A00 but lack the
 * WinRT header windows.gaming.input.h.  Include the real build config
 * first, then disable the feature guards that depend on missing headers.
 */
#include "SDL_build_config_windows.h"

#undef HAVE_WINDOWS_GAMING_INPUT_H
#undef SDL_JOYSTICK_WGI
