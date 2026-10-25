#pragma once

#include "platform/platform.hpp"
#include "render/make_renderer.hpp"

#include <filesystem>
#include <memory>

namespace app {

// Makes the window and the renderer for `backend` in it. When that isn't the default renderer and can't
// start (OpenGL 2 on a driver below 2.1, say), the player is told (unless `quiet`) and the default one is
// made instead, in a new window. `window` is the one drawn in. The renderer must be destroyed before the
// window. Throws when nothing starts.
[[nodiscard]] std::unique_ptr<ts2::render::IRenderer> openRenderer(std::unique_ptr<ts2::platform::Window>& window, ts2::render::Backend backend,
                                                                   ts2::platform::WindowOptions options, const std::filesystem::path& shaders,
                                                                   bool quiet);

} // namespace app
