#include "crash_dialog.hpp"

#include "report_upload.hpp"

#include "diag/log.hpp"
#include "diag/report.hpp"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// clang-format off
#include <commctrl.h>
#include <shellapi.h>
// clang-format on
#include <SDL3/SDL.h>
#include <objbase.h>

// Task dialogs need version 6 of the common controls.
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace app::crash {

namespace {

constexpr int kSend = 100, kShowFiles = 101, kFirstChoice = 200;

std::wstring wide(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}


// What a choice does. True when the message is done (the report went off to be sent).
bool act(int choice, const Message& m) {
    if (choice == kSend && canSend(m.folder) && startSending(m.folder)) {
        return true;  // a fresh copy of NeoTS2 sends it and says how it went
    }
    if (choice == kSend) {
        openLink(m.reportLink);  // no dump to send: a GitHub issue, and the files to drag in
    }
    openFolder(m.folder);
    return false;
}

// The plain message box, when task dialogs aren't there.
void fallback(const Message& m) {
    if (m.shown) {
        m.shown();
    }
    const std::string text = m.text + "\n\nThe report was saved in:\n" + ts2::diag::utf8(m.folder);
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, kSend, "Send a report"},
        {0, kShowFiles, "Show files"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, IDCLOSE, m.close.c_str()},
    };
    SDL_MessageBoxData box{};
    box.flags = SDL_MESSAGEBOX_INFORMATION;
    box.title = "NeoTS2";
    box.message = text.c_str();
    box.numbuttons = 3;
    box.buttons = buttons;
    for (int asked = 0; asked < 10; ++asked) {
        int choice = IDCLOSE;
        if (!SDL_ShowMessageBox(&box, &choice) || choice == IDCLOSE || act(choice, m)) {
            return;
        }
    }
}

// A message's text in lines of at most 60 characters, so it ends well before the dialog's edge.
std::wstring dialogText(const std::string& text) {
    return wide(ts2::diag::wrapped(text, 60));
}

TASKDIALOGCONFIG dialog(const std::wstring& heading, const std::wstring& text) {
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof config;
    config.hInstance = GetModuleHandleW(nullptr);
    // A set width (in dialog units, about 470 pixels at 100% scaling), with the text broken into
    // shorter lines than that (see dialogText). Windows wraps text right at the dialog's edge.
    config.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    config.cxWidth = 280;
    config.pszWindowTitle = L"NeoTS2";
    config.pszMainIcon = MAKEINTRESOURCEW(1);  // the app's icon
    config.pszMainInstruction = heading.c_str();
    config.pszContent = text.c_str();
    config.pfCallback = [](HWND window, UINT note, WPARAM button, LPARAM, LONG_PTR data) -> HRESULT {
        const auto* m = reinterpret_cast<const Message*>(data);
        if (note == TDN_CREATED) {
            // In front of everything, even a fullscreen game window that is left frozen behind it.
            SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            SetForegroundWindow(window);
            if (m && m->shown) {
                m->shown();
            }
        }
        // Showing the files, or a report with no dump to send (a GitHub issue and the files), keeps
        // the message up, so the tester can still send or close it afterwards.
        const int clicked = static_cast<int>(button);
        if (note == TDN_BUTTON_CLICKED && m && (clicked == kShowFiles || (clicked == kSend && !canSend(m->folder)))) {
            act(clicked, *m);
            return S_FALSE;
        }
        return S_OK;
    };
    return config;
}

void shellOpen(const std::wstring& what) {
    // Waits until the browser or Explorer has the request: these programs end right after.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    // Lets the browser come to the front instead of opening behind our message.
    AllowSetForegroundWindow(ASFW_ANY);
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof info;
    info.fMask = SEE_MASK_NOASYNC;
    info.lpVerb = L"open";
    info.lpFile = what.c_str();
    info.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&info);
    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
}

} // namespace

void openLink(const std::string& url) {
    shellOpen(wide(url));
}

// Windows shows its own progress dialog while a report is sent (see report_upload.cpp).
void showWhile(const std::string&, const std::string&, const std::atomic<bool>&) {}

void openFolder(const std::filesystem::path& folder) {
    shellOpen(folder.wstring());
}

void show(const Message& m) {
    const std::wstring heading = wide(m.heading), text = dialogText(m.text), details = wide(m.details), close = wide(m.close);
    const std::wstring send = canSend(m.folder)
                                  ? L"Send a report\nSends the saved report to the NeoTS2 developers."
                                  : L"Send a report\nOpens a new report on GitHub in your browser, and the saved files to add to it.";
    const TASKDIALOG_BUTTON buttons[] = {
        {kSend, send.c_str()},
        {kShowFiles, L"Show the saved files\nOpens the folder with the report."},
        {IDCLOSE, close.c_str()},
    };
    TASKDIALOGCONFIG config = dialog(heading, text);
    config.pszExpandedInformation = details.c_str();
    config.pszCollapsedControlText = L"Show details";
    config.pszExpandedControlText = L"Hide details";
    config.cButtons = 3;
    config.pButtons = buttons;
    config.nDefaultButton = kSend;
    config.lpCallbackData = reinterpret_cast<LONG_PTR>(&m);
    for (int asked = 0; asked < 10; ++asked) {
        int choice = IDCLOSE;
        if (FAILED(TaskDialogIndirect(&config, &choice, nullptr, nullptr))) {
            fallback(m);
            return;
        }
        if ((choice != kSend && choice != kShowFiles) || act(choice, m)) {
            return;
        }
    }
}

int choose(const std::string& heading, const std::string& text, const std::vector<std::string>& choices) {
    const std::wstring h = wide(heading), t = dialogText(text);
    std::vector<std::wstring> labels;
    std::vector<TASKDIALOG_BUTTON> buttons;
    for (const auto& c : choices) {
        labels.push_back(wide(c));
    }
    for (std::size_t i = 0; i < labels.size(); ++i) {
        buttons.push_back({kFirstChoice + static_cast<int>(i), labels[i].c_str()});
    }
    TASKDIALOGCONFIG config = dialog(h, t);
    config.cButtons = static_cast<UINT>(buttons.size());
    config.pButtons = buttons.data();
    int choice = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&config, &choice, nullptr, nullptr))) {
        MessageBoxW(nullptr, (h + L"\n\n" + t).c_str(), L"NeoTS2", MB_OK | MB_ICONINFORMATION);
        return -1;
    }
    const int index = choice - kFirstChoice;
    return index >= 0 && index < static_cast<int>(choices.size()) ? index : -1;
}

} // namespace app::crash

#elif defined(NEOTS2_CRASHPAD)

#include "ui/fonts.hpp"

#include <SDL3/SDL.h>
#include <spawn.h>
#include <sys/wait.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>

extern char** environ;

// Noto Sans, built into the program (see CMakeLists.txt).
extern const unsigned char notoSansRegular[];
extern const std::size_t notoSansRegularSize;
extern const unsigned char notoSansSemiBold[];
extern const std::size_t notoSansSemiBoldSize;

namespace app::crash {

namespace {

constexpr int kSend = 100, kShowFiles = 101, kClose = 102, kFirstChoice = 200;

// The video system while message boxes are up, when nothing else has it: SDL can only tell the
// system's light or dark look with it. Stopped again afterwards.
struct Video {
    bool started = !SDL_WasInit(SDL_INIT_VIDEO) && SDL_InitSubSystem(SDL_INIT_VIDEO);
    Video() = default;
    Video(const Video&) = delete;
    Video& operator=(const Video&) = delete;
    ~Video() {
        if (started) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
    }
};

// Dark colours for the box SDL draws itself (the plain one on Linux without a desktop's own dialog
// program). macOS and the desktops' dialogs follow the system's look by themselves.
const SDL_MessageBoxColorScheme* colours() {
    static const SDL_MessageBoxColorScheme dark{{
        {0x2b, 0x2b, 0x2b},  // background
        {0xf0, 0xf0, 0xf0},  // text
        {0x5a, 0x5a, 0x5a},  // button border
        {0x3c, 0x3c, 0x3c},  // button background
        {0x4a, 0x6f, 0xa5},  // button selected
    }};
    return SDL_GetSystemTheme() == SDL_SYSTEM_THEME_DARK ? &dark : nullptr;
}

// A program run with its arguments and waited for, with no shell, so nothing in them is run.
void run(const char* program, const std::string& argument) {
    std::string name = program, arg = argument;
    char* args[] = {name.data(), arg.data(), nullptr};
    pid_t child = 0;
    if (posix_spawnp(&child, program, nullptr, nullptr, args, environ) == 0) {
        int status = 0;
        waitpid(child, &status, 0);
    }
}

// Message box text broken into lines on Linux, where SDL's own box doesn't wrap and one long line
// makes it as wide as the screen. macOS and the desktops' dialogs wrap by themselves.
std::string boxText(const std::string& text) {
#ifdef __APPLE__
    return text;
#else
    return ts2::diag::wrapped(text, 64);
#endif
}

const char* opener() {
#ifdef __APPLE__
    return "open";
#else
    return "xdg-open";
#endif
}

// What a choice does. True when the message is done (the report went off to be sent).
bool act(int choice, const Message& m) {
    if (choice == kSend && canSend(m.folder) && startSending(m.folder)) {
        return true;  // a fresh copy of NeoTS2 sends it and says how it went
    }
    if (choice == kSend) {
        openLink(m.reportLink);  // no dump to send: a GitHub issue, and the files to drag in
    }
    openFolder(m.folder);
    return false;
}

void waitFor(const std::atomic<bool>& done) {
    while (!done) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

// A line of text as a texture: white glyphs, tinted when drawn. Null for empty text.
struct TextureDeleter {
    void operator()(SDL_Texture* t) const { SDL_DestroyTexture(t); }
};
using Texture = std::unique_ptr<SDL_Texture, TextureDeleter>;

Texture textTexture(SDL_Renderer* renderer, ts2::ui::Fonts& fonts, const char* style, const std::string& text) {
    const auto image = fonts.draw(fonts.style(style), text);
    const auto& p = image.pixels;
    if (p.width == 0 || p.height == 0 || p.rgba.size() < static_cast<std::size_t>(p.width) * p.height * 4) {
        return nullptr;
    }
    SDL_Surface* surface = SDL_CreateSurfaceFrom(static_cast<int>(p.width), static_cast<int>(p.height), SDL_PIXELFORMAT_RGBA32,
                                                 const_cast<std::uint8_t*>(p.rgba.data()), static_cast<int>(p.width * 4));
    Texture texture(surface ? SDL_CreateTextureFromSurface(renderer, surface) : nullptr);
    SDL_DestroySurface(surface);
    return texture;
}

void drawText(SDL_Renderer* renderer, SDL_Texture* text, float x, float y, SDL_Color colour) {
    if (!text) {
        return;
    }
    float w = 0, h = 0;
    SDL_GetTextureSize(text, &w, &h);
    SDL_SetTextureColorMod(text, colour.r, colour.g, colour.b);
    const SDL_FRect at{x, y, w, h};
    SDL_RenderTexture(renderer, text, nullptr, &at);
}

} // namespace

void showWhile(const std::string& heading, const std::string& text, const std::atomic<bool>& done) {
    const Video video;
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        waitFor(done);  // no display
        return;
    }
    constexpr int kWidth = 380, kHeight = 118;  // in the display's points
    SDL_Window* window = SDL_CreateWindow("NeoTS2", kWidth, kHeight, SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_ALWAYS_ON_TOP);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) {
        if (window) {
            SDL_DestroyWindow(window);
        }
        waitFor(done);
        return;
    }
    const float density = SDL_GetWindowPixelDensity(window);
    const float scale = density > 0 ? density : 1;
    const bool dark = SDL_GetSystemTheme() == SDL_SYSTEM_THEME_DARK;
    const SDL_Color background = dark ? SDL_Color{0x2b, 0x2b, 0x2b, 255} : SDL_Color{0xf4, 0xf4, 0xf4, 255};
    const SDL_Color ink = dark ? SDL_Color{0xf0, 0xf0, 0xf0, 255} : SDL_Color{0x1d, 0x1d, 0x1f, 255};
    const SDL_Color muted = dark ? SDL_Color{0xb4, 0xb4, 0xb4, 255} : SDL_Color{0x5f, 0x5f, 0x5f, 255};
    const SDL_Color track = dark ? SDL_Color{0x44, 0x44, 0x44, 255} : SDL_Color{0xdd, 0xdd, 0xdd, 255};
    const SDL_Color bar{0x2f, 0x6f, 0xde, 255};
    Texture headingText, bodyText;
    try {
        const auto size = [&](int points) { return std::to_string(static_cast<int>(std::lround(points * scale))); };
        ts2::ui::Fonts fonts("[Font Styles]\nHeading = \"Noto Sans SemiBold\", \"" + size(17) + "\"\nText = \"Noto Sans\", \"" + size(13) + "\"\n",
                             {std::vector<std::uint8_t>(notoSansRegular, notoSansRegular + notoSansRegularSize),
                              std::vector<std::uint8_t>(notoSansSemiBold, notoSansSemiBold + notoSansSemiBoldSize)});
        headingText = textTexture(renderer, fonts, "Heading", heading);
        bodyText = textTexture(renderer, fonts, "Text", text);
    } catch (const std::exception&) {
        // drawn without its words: the bar still shows it is working
    }
    const auto started = std::chrono::steady_clock::now();
    while (!done) {
        for (SDL_Event e; SDL_PollEvent(&e);) {
            // Closing it doesn't stop the sending, which ends by itself within its time limit.
        }
        const float pad = 20 * scale, width = kWidth * scale;
        SDL_SetRenderDrawColor(renderer, background.r, background.g, background.b, 255);
        SDL_RenderClear(renderer);
        drawText(renderer, headingText.get(), pad, 16 * scale, ink);
        drawText(renderer, bodyText.get(), pad, 46 * scale, muted);
        // A block sliding along the track, once every 1.4 seconds.
        const SDL_FRect trackRect{pad, 82 * scale, width - 2 * pad, 6 * scale};
        SDL_SetRenderDrawColor(renderer, track.r, track.g, track.b, 255);
        SDL_RenderFillRect(renderer, &trackRect);
        const float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - started).count();
        const float phase = std::fmod(seconds / 1.4f, 1.0f);
        const float block = trackRect.w * 0.3f;
        const float left = std::max(trackRect.x, trackRect.x - block + phase * (trackRect.w + block));
        const float right = std::min(trackRect.x + trackRect.w, trackRect.x - block + phase * (trackRect.w + block) + block);
        const SDL_FRect barRect{left, trackRect.y, std::max(0.0f, right - left), trackRect.h};
        SDL_SetRenderDrawColor(renderer, bar.r, bar.g, bar.b, 255);
        SDL_RenderFillRect(renderer, &barRect);
        SDL_RenderPresent(renderer);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    headingText.reset();
    bodyText.reset();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
}

void openLink(const std::string& url) {
    run(opener(), url);
}

void openFolder(const std::filesystem::path& folder) {
    run(opener(), folder.string());
}

void show(const Message& m) {
    const Video video;
    if (m.shown) {
        m.shown();
    }
    const std::string text = boxText(m.text + "\n\nThe report was saved in:\n" + ts2::diag::utf8(m.folder));
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, kSend, "Send a report"},
        {0, kShowFiles, "Show the saved files"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, kClose, m.close.c_str()},
    };
    SDL_MessageBoxData box{};
    box.flags = SDL_MESSAGEBOX_INFORMATION;
    box.title = m.heading.c_str();
    box.message = text.c_str();
    box.numbuttons = 3;
    box.buttons = buttons;
    box.colorScheme = colours();
    for (int asked = 0; asked < 10; ++asked) {
        int choice = kClose;
        if (!SDL_ShowMessageBox(&box, &choice) || choice == kClose || choice < 0 || act(choice, m)) {
            return;
        }
    }
}

int choose(const std::string& heading, const std::string& text, const std::vector<std::string>& choices) {
    const Video video;
    std::vector<std::string> labels;
    for (const auto& c : choices) {
        labels.push_back(c.substr(0, c.find('\n')));  // the note doesn't fit on a button
    }
    std::vector<SDL_MessageBoxButtonData> buttons;
    for (std::size_t i = 0; i < labels.size(); ++i) {
        const SDL_MessageBoxButtonFlags flags = i == 0 ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT
                                                : i + 1 == labels.size() ? SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT
                                                                         : 0;
        buttons.push_back({flags, kFirstChoice + static_cast<int>(i), labels[i].c_str()});
    }
    SDL_MessageBoxData box{};
    box.flags = SDL_MESSAGEBOX_INFORMATION;
    box.title = heading.c_str();
    const std::string message = boxText(text);
    box.message = message.c_str();
    box.numbuttons = static_cast<int>(buttons.size());
    box.buttons = buttons.data();
    box.colorScheme = colours();
    int choice = -1;
    if (!SDL_ShowMessageBox(&box, &choice)) {
        return -1;
    }
    const int index = choice - kFirstChoice;
    return index >= 0 && index < static_cast<int>(choices.size()) ? index : -1;
}

} // namespace app::crash

#else

namespace app::crash {
void openLink(const std::string&) {}
void openFolder(const std::filesystem::path&) {}
void showWhile(const std::string&, const std::string&, const std::atomic<bool>&) {}
void show(const Message&) {}
int choose(const std::string&, const std::string&, const std::vector<std::string>&) { return -1; }
} // namespace app::crash

#endif
