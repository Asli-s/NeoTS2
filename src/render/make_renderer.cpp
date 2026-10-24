#include "render/make_renderer.hpp"

#include "render/renderer.hpp"

#include <SDL3/SDL.h>

#include <stdexcept>

#ifdef NEOTS2_GL2
#include "render/gl/gl_renderer.hpp"
#endif

namespace ts2::render {

bool available(Backend backend) {
#ifdef NEOTS2_GL2
    return backend == Backend::SdlGpu || backend == Backend::OpenGL2;
#else
    return backend == Backend::SdlGpu;
#endif
}

std::optional<Backend> backendNamed(std::string_view name) {
    if (name == "sdlgpu") {
        return Backend::SdlGpu;
    }
    if (name == "gl2") {
        return Backend::OpenGL2;
    }
    return std::nullopt;
}

std::uint64_t windowFlagsFor(Backend backend) {
#ifdef NEOTS2_GL2
    if (backend == Backend::OpenGL2) {
        // OpenGL 2.1 with the old fixed-function entry points kept, as the oldest drivers have it.
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        return SDL_WINDOW_OPENGL;
    }
#endif
    (void)backend;
    return 0;
}

std::unique_ptr<IRenderer> makeRenderer(Backend backend, SDL_Window* window, const std::filesystem::path& shaderFolder) {
#ifdef NEOTS2_GL2
    if (backend == Backend::OpenGL2) {
        return std::make_unique<GlRenderer>(window, shaderFolder);
    }
#endif
    if (backend != Backend::SdlGpu) {
        throw std::runtime_error("this build doesn't have that renderer");
    }
    return std::make_unique<SdlGpuRenderer>(window, shaderFolder);
}

} // namespace ts2::render
