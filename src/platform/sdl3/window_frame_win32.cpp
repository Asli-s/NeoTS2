#include "platform/sdl3/window_frame.hpp"

#include <SDL3/SDL.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>

namespace ts2::platform {

HiddenFrame hiddenFrame(SDL_Window* window) {
    // A few dozen pixels at most, even at high display scales. Anything wider is a wrong answer.
    constexpr int kMaxWidth = 64;
    const auto hwnd = static_cast<HWND>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    RECT outer{}, visible{};
    if (hwnd == nullptr || !GetWindowRect(hwnd, &outer) ||
        DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &visible, sizeof visible) != S_OK) {
        return {};
    }
    const HiddenFrame frame{visible.left - outer.left, visible.top - outer.top, outer.right - visible.right,
                            outer.bottom - visible.bottom};
    const auto usable = [](int width) { return width >= 0 && width <= kMaxWidth; };
    if (!usable(frame.left) || !usable(frame.top) || !usable(frame.right) || !usable(frame.bottom)) {
        return {};
    }
    return frame;
}

} // namespace ts2::platform
