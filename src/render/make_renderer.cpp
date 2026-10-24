#include "render/make_renderer.hpp"

#include <stdexcept>

#ifdef NEOTS2_SDLGPU
#include "platform/sdl3/sdl3_window.hpp"
#include "render/renderer.hpp"
#endif

#if defined(NEOTS2_GL1) || defined(NEOTS2_GL2)
#include "render/gl/gl_renderer.hpp"
#endif

namespace ts2::render {

bool available(Backend backend) {
#ifdef NEOTS2_SDLGPU
    if (backend == Backend::SdlGpu) {
        return true;
    }
#endif
#ifdef NEOTS2_GL2
    if (backend == Backend::OpenGL2) {
        return true;
    }
#endif
#ifdef NEOTS2_GL1
    if (backend == Backend::OpenGL1) {
        return true;
    }
#endif
    (void)backend;
    return false;
}

Backend defaultBackend() {
    return available(Backend::SdlGpu)     ? Backend::SdlGpu
           : available(Backend::OpenGL2) ? Backend::OpenGL2
                                         : Backend::OpenGL1;
}

std::optional<Backend> backendNamed(std::string_view name) {
    if (name == "sdlgpu") {
        return Backend::SdlGpu;
    }
    if (name == "gl2") {
        return Backend::OpenGL2;
    }
    if (name == "gl1") {
        return Backend::OpenGL1;
    }
    return std::nullopt;
}

void prepareWindow(Backend backend, platform::WindowOptions& options) {
#ifdef NEOTS2_GL2
    if (backend == Backend::OpenGL2) {
        // OpenGL 2.1 with the old fixed-function entry points kept, as the oldest drivers have it.
        platform::GlRequest request;
        request.major = 2;
        request.minor = 1;
        request.compatibility = true;
        request.doubleBuffer = true;
        request.depthBits = 24;
        options.gl = request;
    }
#endif
#ifdef NEOTS2_GL1
    if (backend == Backend::OpenGL1) {
        // OpenGL 1.5, the first with buffers, with the fixed pipeline it draws with.
        platform::GlRequest request;
        request.major = 1;
        request.minor = 5;
        request.compatibility = true;
        request.doubleBuffer = true;
        request.depthBits = 24;
        options.gl = request;
    }
#endif
    (void)backend;
    (void)options;
}

std::unique_ptr<IRenderer> makeRenderer(Backend backend, platform::Window& window, const std::filesystem::path& shaderFolder) {
#ifdef NEOTS2_GL2
    if (backend == Backend::OpenGL2) {
        return std::make_unique<GlRenderer>(window, shaderFolder);
    }
#endif
#ifdef NEOTS2_GL1
    if (backend == Backend::OpenGL1) {
        return std::make_unique<GlRenderer>(window, shaderFolder, true);
    }
#endif
#ifdef NEOTS2_SDLGPU
    if (backend == Backend::SdlGpu) {
        return std::make_unique<SdlGpuRenderer>(platform::sdlWindow(window), shaderFolder);
    }
#endif
    (void)window;
    (void)shaderFolder;
    throw std::runtime_error("this build doesn't have that renderer");
}

} // namespace ts2::render
