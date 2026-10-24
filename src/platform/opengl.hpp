// The OpenGL types, constants and prototypes, from the header of the platform's SDL.
#pragma once

#define GL_GLEXT_PROTOTYPES 1
#if defined(NEOTS2_PLATFORM_SDL12)
#include <SDL_opengl.h>
#elif defined(NEOTS2_PLATFORM_SDL3)
#include <SDL3/SDL_opengl.h>
#else
#error "Build with NEOTS2_PLATFORM_SDL3 or NEOTS2_PLATFORM_SDL12 (the ts2platform library sets them)."
#endif
