#pragma once

#include "render/make_renderer.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>

struct SDL_Window;

namespace app {

// Makes the window and the renderer for `backend` in it. When the OpenGL 2 one can't start (a driver below
// 2.1, say), the player is told (unless `quiet`) and the default renderer is made instead, in a new window.
// `window` is the one drawn in. The renderer must be destroyed before the window. Throws when nothing starts.
[[nodiscard]] std::unique_ptr<ts2::render::IRenderer> openRenderer(SDL_Window*& window, ts2::render::Backend backend, int width, int height,
                                                                   std::uint64_t windowFlags, const std::filesystem::path& shaders, bool quiet);

} // namespace app
