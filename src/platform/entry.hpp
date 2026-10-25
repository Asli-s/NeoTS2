// Included where main() is. Some systems start a program through their own main: SDL 1.2 on Mac OS X
// sets up the application there (its SDLmain library calls SDL_main, which this makes of main).
#pragma once

#if defined(NEOTS2_PLATFORM_SDL12)
#include <SDL.h>
#elif defined(NEOTS2_PLATFORM_SDL3)
#include <SDL3/SDL_main.h>
#else
#error "Build with NEOTS2_PLATFORM_SDL3 or NEOTS2_PLATFORM_SDL12 (the ts2platform library sets them)."
#endif
