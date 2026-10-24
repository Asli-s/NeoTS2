#include "render/make_renderer.hpp"

#include "render/renderer.hpp"

namespace ts2::render {

std::uint64_t windowFlagsFor(Backend) {
    return 0;
}

std::unique_ptr<IRenderer> makeRenderer(Backend, SDL_Window* window, const std::filesystem::path& shaderFolder) {
    return std::make_unique<SdlGpuRenderer>(window, shaderFolder);
}

} // namespace ts2::render
