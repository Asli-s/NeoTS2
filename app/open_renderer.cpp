#include "open_renderer.hpp"

#include "diag/log.hpp"
#include "message_box.hpp"

#include <stdexcept>
#include <string>

namespace app {

std::unique_ptr<ts2::render::IRenderer> openRenderer(std::unique_ptr<ts2::platform::Window>& window, ts2::render::Backend backend,
                                                     ts2::platform::WindowOptions options, const std::filesystem::path& shaders, bool quiet) {
    using ts2::render::Backend;
    for (;;) {
        ts2::platform::WindowOptions made = options;
        ts2::render::prepareWindow(backend, made);
        std::string problem;
        try {
            window = ts2::platform::Window::create(made);
            return ts2::render::makeRenderer(backend, *window, shaders);
        } catch (const std::exception& ex) {
            problem = ex.what();
        }
        window.reset();
        if (backend == ts2::render::defaultBackend()) {
            throw std::runtime_error(problem);
        }
        ts2::diag::error("The OpenGL 2 renderer couldn't start: " + problem);
        if (!quiet) {
            showMessage(MessageKind::Warning, "The OpenGL 2 renderer couldn't start:\n\n" + problem +
                                                  "\n\nNeoTS2 will draw with its default renderer instead. Choose Automatic under Graphics in the launcher to stop seeing this.");
        }
        backend = ts2::render::defaultBackend();
    }
}

} // namespace app
