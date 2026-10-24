#include "message_box.hpp"

#include <SDL3/SDL.h>

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// clang-format off
#include <commctrl.h>
// clang-format on

namespace app {

namespace {

// The dialog's width in dialog units. The launcher uses the same.
constexpr UINT kWidth = 240;

std::wstring wide(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

} // namespace

void showMessage(MessageKind kind, const std::string& text, SDL_Window* parent) {
    const std::wstring content = wide(text);
    HWND owner = parent ? static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(parent), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr))
                        : nullptr;
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = owner;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    config.dwCommonButtons = TDCBF_OK_BUTTON;
    config.pszWindowTitle = L"NeoTS2";
    config.pszMainIcon = kind == MessageKind::Error ? TD_ERROR_ICON : kind == MessageKind::Warning ? TD_WARNING_ICON : TD_INFORMATION_ICON;
    config.pszContent = content.c_str();
    config.cxWidth = kWidth;
    if (FAILED(TaskDialogIndirect(&config, nullptr, nullptr, nullptr))) {
        const Uint32 flags = kind == MessageKind::Error     ? SDL_MESSAGEBOX_ERROR
                             : kind == MessageKind::Warning ? SDL_MESSAGEBOX_WARNING
                                                            : SDL_MESSAGEBOX_INFORMATION;
        SDL_ShowSimpleMessageBox(flags, "NeoTS2", text.c_str(), parent);
    }
}

} // namespace app

#else

namespace app {

void showMessage(MessageKind kind, const std::string& text, SDL_Window* parent) {
    const Uint32 flags = kind == MessageKind::Error     ? SDL_MESSAGEBOX_ERROR
                         : kind == MessageKind::Warning ? SDL_MESSAGEBOX_WARNING
                                                        : SDL_MESSAGEBOX_INFORMATION;
    SDL_ShowSimpleMessageBox(flags, "NeoTS2", text.c_str(), parent);
}

} // namespace app

#endif
