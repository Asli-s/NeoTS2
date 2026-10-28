// The invisible parts of a window's frame on Windows.
#pragma once

struct SDL_Window;

namespace ts2::platform {

// Windows draws a window's visible frame inside a wider invisible one, which keeps its shadow. These
// are the invisible widths on each side, in pixels. All 0 elsewhere, or when Windows can't say.
struct HiddenFrame {
    int left = 0, top = 0, right = 0, bottom = 0;
};

#ifdef _WIN32
HiddenFrame hiddenFrame(SDL_Window* window);
#else
inline HiddenFrame hiddenFrame(SDL_Window*) {
    return {};
}
#endif

} // namespace ts2::platform
