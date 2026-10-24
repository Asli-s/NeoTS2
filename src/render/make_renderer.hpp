// Picks the renderer the game draws with.
#pragma once

#include "platform/platform.hpp"
#include "render/irenderer.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

namespace ts2::render {

enum class Backend {
    SdlGpu,   // SDL's GPU API: Direct3D 12 on Windows, Vulkan elsewhere (built with NEOTS2_SDLGPU, which needs SDL 3)
    OpenGL2,  // OpenGL 2.1, for systems with no more (built with NEOTS2_GL2)
    OpenGL1,  // OpenGL 1.5 without shaders, for older systems still (built with NEOTS2_GL1)
};

// Whether this build has the renderer for `backend`.
[[nodiscard]] bool available(Backend backend);

// What draws when none is asked for: the SDL GPU renderer where this build has it, else OpenGL 2, else OpenGL 1.
[[nodiscard]] Backend defaultBackend();

// "sdlgpu", "gl2" or "gl1" as `Backend`, none for another name.
[[nodiscard]] std::optional<Backend> backendNamed(std::string_view name);

// What the window needs to be drawn in with `backend`, set in `options` before the window is made.
void prepareWindow(Backend backend, platform::WindowOptions& options);

// Makes the renderer for `backend` on `window`. Throws a runtime_error saying why when it can't.
[[nodiscard]] std::unique_ptr<IRenderer> makeRenderer(Backend backend, platform::Window& window, const std::filesystem::path& shaderFolder);

} // namespace ts2::render
