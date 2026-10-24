// Picks the renderer the game draws with.
#pragma once

#include "render/irenderer.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>

struct SDL_Window;

namespace ts2::render {

enum class Backend {
    SdlGpu,  // SDL's GPU API: Direct3D 12 on Windows, Vulkan elsewhere
};

// The SDL window flags the window needs to be drawn with `backend`, made before the renderer is.
[[nodiscard]] std::uint64_t windowFlagsFor(Backend backend);

// Makes the renderer for `backend` on `window`. Throws a runtime_error saying why when it can't.
[[nodiscard]] std::unique_ptr<IRenderer> makeRenderer(Backend backend, SDL_Window* window, const std::filesystem::path& shaderFolder);

} // namespace ts2::render
