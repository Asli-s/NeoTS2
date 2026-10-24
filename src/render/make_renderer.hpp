// Picks the renderer the game draws with.
#pragma once

#include "render/irenderer.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

struct SDL_Window;

namespace ts2::render {

enum class Backend {
    SdlGpu,   // SDL's GPU API: Direct3D 12 on Windows, Vulkan elsewhere
    OpenGL2,  // OpenGL 2.1, for systems with no more (built with NEOTS2_GL2)
};

// Whether this build has the renderer for `backend`.
[[nodiscard]] bool available(Backend backend);

// "sdlgpu" or "gl2" as `Backend`, none for another name.
[[nodiscard]] std::optional<Backend> backendNamed(std::string_view name);

// The SDL window flags the window needs to be drawn with `backend`, made before the renderer is.
[[nodiscard]] std::uint64_t windowFlagsFor(Backend backend);

// Makes the renderer for `backend` on `window`. Throws a runtime_error saying why when it can't.
[[nodiscard]] std::unique_ptr<IRenderer> makeRenderer(Backend backend, SDL_Window* window, const std::filesystem::path& shaderFolder);

} // namespace ts2::render
