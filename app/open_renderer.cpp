#include "open_renderer.hpp"

#include "diag/log.hpp"
#include "message_box.hpp"
#include "render/renderer.hpp"

#include <SDL3/SDL.h>

#include <stdexcept>
#include <string>

namespace app {

std::unique_ptr<ts2::render::IRenderer> openRenderer(SDL_Window*& window, ts2::render::Backend backend, int width, int height,
                                                     std::uint64_t windowFlags, const std::filesystem::path& shaders, bool quiet) {
    using ts2::render::Backend;
    for (;;) {
        window = SDL_CreateWindow("NeoTS2", width, height, windowFlags | ts2::render::windowFlagsFor(backend));
        std::string problem = window == nullptr ? SDL_GetError() : "";
        if (window != nullptr) {
            try {
                return ts2::render::makeRenderer(backend, window, shaders);
            } catch (const std::exception& ex) {
                problem = ex.what();
            }
            SDL_DestroyWindow(window);
            window = nullptr;
        }
        if (backend == Backend::SdlGpu) {
            throw std::runtime_error(problem);
        }
        ts2::diag::error("The OpenGL 2 renderer couldn't start: " + problem);
        if (!quiet) {
            showMessage(MessageKind::Warning, "The OpenGL 2 renderer couldn't start:\n\n" + problem +
                                                  "\n\nNeoTS2 will draw with its default renderer instead. Choose Automatic under Graphics in the launcher to stop seeing this.");
        }
        backend = Backend::SdlGpu;
    }
}

} // namespace app
