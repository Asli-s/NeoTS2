// The SDL 3 window behind a platform window, for what draws with SDL itself (the GPU renderer).
#pragma once

#include "platform/platform.hpp"

struct SDL_Window;

namespace ts2::platform {

[[nodiscard]] SDL_Window* sdlWindow(Window& window);

} // namespace ts2::platform
