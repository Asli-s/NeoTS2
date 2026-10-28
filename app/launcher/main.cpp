// NeoTS2's launcher: finds the game's installs and save folders and starts NeoTS2 with them.
// It never opens the game's files and draws with SDL's 2D renderer, so it still opens when the
// game data or the graphics driver are broken.
#include "launcher/launcher_style.hpp"
#include "launcher/launcher_ui.hpp"
#include "launcher/running_games.hpp"
#include "paths/names.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_sdlrenderer3.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// clang-format off
#include <commctrl.h>
#include <delayimp.h>
// clang-format on

// Version 6 of the common controls, for the current look of task dialogs and of SDL's messages, as
// in the game.
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

namespace {

#ifdef _WIN32
// A message in the same task dialog the game uses (app/message_box.cpp): a fixed width of 240
// dialog units with room around the text. It needs no SDL.
void showMessage(const std::wstring& text, PCWSTR icon = TD_ERROR_ICON) {
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    config.dwCommonButtons = TDCBF_OK_BUTTON;
    config.pszWindowTitle = L"NeoTS2";
    config.pszMainIcon = icon;
    config.pszContent = text.c_str();
    config.cxWidth = 240;
    if (FAILED(TaskDialogIndirect(&config, nullptr, nullptr, nullptr))) {
        MessageBoxW(nullptr, text.c_str(), L"NeoTS2", MB_OK | (icon == TD_ERROR_ICON ? MB_ICONERROR : MB_ICONINFORMATION));
    }
}

std::wstring wideFrom(const std::string& utf8) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring text(static_cast<std::size_t>(n > 1 ? n - 1 : 0), L'\0');
    if (n > 1) {
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, text.data(), n);
    }
    return text;
}

// What the player should do when files the launcher needs aren't beside it, or nothing when all
// are there. Running it from inside its zip leaves them out, and so can an antivirus.
std::wstring missingFilesProblem() {
    wchar_t exe[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    const std::filesystem::path folder = std::filesystem::path(exe).parent_path();
    std::wstring missing;
    std::error_code error;
    for (const wchar_t* name : {L"SDL3.dll", L"neots2.exe", L"shaders"}) {
        if (!std::filesystem::exists(folder / name, error)) {
            missing += std::wstring(L"\n    ") + name;
        }
    }
    if (missing.empty()) {
        return {};
    }
    // Windows runs a program from inside a zip by unpacking it alone into the temporary folder.
    wchar_t temp[MAX_PATH] = {};
    const DWORD tempLength = GetTempPathW(MAX_PATH, temp);
    if (tempLength > 0 && tempLength < MAX_PATH &&
        ts2::paths::spelledWithin(folder, std::filesystem::path(temp))) {
        return L"NeoTS2 can't start from inside its zip file.\n\n"
               L"Right-click the zip and choose Extract All. Then open the extracted folder and start "
               L"neots2-launcher.exe from there.";
    }
    return L"Some of NeoTS2's files are missing next to the launcher:\n" + missing +
           L"\n\nExtract the whole zip again, into a folder of its own. If the files go missing again, your "
           L"antivirus may be removing them. Add the NeoTS2 folder to its exceptions, then extract the zip again.";
}

// SDL3.dll is loaded when the launcher first uses it (see CMakeLists.txt), so a launcher started
// without it can still say why instead of failing with Windows' own error.
FARPROC WINAPI onDelayLoadFailure(unsigned notice, PDelayLoadInfo) {
    if (notice == dliFailLoadLib || notice == dliFailGetProc) {
        std::wstring problem = missingFilesProblem();
        if (problem.empty()) {
            problem = L"SDL3.dll next to the launcher couldn't be loaded. Extract the whole zip again.";
        }
        showMessage(problem);
        ExitProcess(1);
    }
    return nullptr;
}
#endif

// A message for the player, in UTF-8: the task dialog on Windows, SDL's message box elsewhere.
void tell(const std::string& text, bool error) {
#ifdef _WIN32
    showMessage(wideFrom(text), error ? TD_ERROR_ICON : TD_INFORMATION_ICON);
#else
    SDL_ShowSimpleMessageBox(error ? SDL_MESSAGEBOX_ERROR : SDL_MESSAGEBOX_INFORMATION, "NeoTS2", text.c_str(), nullptr);
#endif
}

// The launcher's settings, kept on this computer next to NeoTS2's logs because they hold its
// paths. Empty when there is no such folder.
std::filesystem::path settingsFile() {
#ifdef _WIN32
    wchar_t local[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        return std::filesystem::path(local) / L"NeoTS2" / L"launcher-settings.txt";
    }
#endif
    std::filesystem::path file;
    if (char* data = SDL_GetPrefPath(nullptr, "NeoTS2")) {
        if (const auto folder = ts2::paths::pathFromUtf8(data)) {
            file = *folder / "launcher-settings.txt";
        }
        SDL_free(data);
    }
    return file;
}

// The display's scale where the window is (1.5 at 150%).
float displayScale(SDL_Window* window) {
    const float scale = SDL_GetDisplayContentScale(SDL_GetDisplayForWindow(window));
    return scale > 0.0f ? scale : 1.0f;
}

// Builds the fonts and sizes for a display scale, replacing any built before.
ts2::launcher::LauncherFonts restyle(float scale) {
    ImGui::GetIO().Fonts->Clear();
    ImGui::GetStyle() = ImGuiStyle();
    return ts2::launcher::applyStyle(scale);
}

// The size the launcher's contents ask for, at least kMinWidth wide. ImGui's own auto-fit can't be
// used, as it never grows a window past the current SDL window. The height comes from ImGui's
// ContentSizeIdeal, an internal field, so check it still means this after an ImGui update.
ImVec2 neededSize(const ts2::launcher::LauncherState& state, float scale) {
    constexpr float kMinWidth = 760.0f;
    const ImVec2 padding = ImGui::GetStyle().WindowPadding;
    const float width = std::max({state.contentWidth + padding.x, state.playRowWidth + padding.x * 2, kMinWidth * scale});
    return ImVec2(width, state.contentHeight + padding.y * 2);
}

// The display's usable area (the screen without the taskbar), and the window's frame around its
// contents.
struct Room {
    SDL_Rect usable{};
    int top = 0, left = 0, bottom = 0, right = 0;

    int maxWidth() const { return std::max(usable.w - left - right, 1); }
    int maxHeight() const { return std::max(usable.h - top - bottom, 1); }
};

Room roomFor(SDL_Window* window) {
    Room room;
    SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(window), &room.usable);
    SDL_GetWindowBordersSize(window, &room.top, &room.left, &room.bottom, &room.right);
    return room;
}

// Sizes the window to what its contents need, inside the display's usable area. The first time
// it also centres the window there.
void fitWindow(SDL_Window* window, ImVec2 needed, bool centre) {
    const Room room = roomFor(window);
    const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    const SDL_Rect& usable = room.usable;
    const int width = std::min(static_cast<int>(std::ceil(needed.x)), room.maxWidth());
    const int height = std::min(static_cast<int>(std::ceil(needed.y)), room.maxHeight());
    int oldWidth = 0;
    int oldHeight = 0;
    SDL_GetWindowSize(window, &oldWidth, &oldHeight);
    const bool resized = width != oldWidth || height != oldHeight;
    if (resized) {
        SDL_SetWindowSize(window, width, height);
    }
    if (centre) {
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display));
        return;
    }
    if (!resized) {
        return;  // where the player put the window is their choice
    }
    // A window that grew past the bottom of the screen moves up so all of it shows.
    int x = 0;
    int y = 0;
    SDL_GetWindowPosition(window, &x, &y);
    const int bottom = usable.y + usable.h - room.bottom;
    if (y + height > bottom) {
        SDL_SetWindowPosition(window, x, std::max(usable.y + room.top, bottom - height));
    }
}

// SDL picks a GPU-backed 2D renderer when it can. Software drawing always works.
SDL_Renderer* createRenderer(SDL_Window* window) {
    if (SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr)) {
        return renderer;
    }
    return SDL_CreateRenderer(window, SDL_SOFTWARE_RENDERER);
}

} // namespace

#ifdef _WIN32
extern "C" const PfnDliHook __pfnDliFailureHook2 = onDelayLoadFailure;
#endif

int main(int, char**) {
#ifdef _WIN32
    if (const std::wstring problem = missingFilesProblem(); !problem.empty()) {
        showMessage(problem);
        return 1;
    }
#endif
    if (ts2::launcher::otherLauncherOpen()) {
        tell("The NeoTS2 launcher is already open.", false);
        return 0;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        tell(std::string("The launcher couldn't start: ") + SDL_GetError(), true);
        return 1;
    }
    // Hidden until its first layout gives it its size, so it never opens at the wrong one.
    SDL_Window* window = SDL_CreateWindow("NeoTS2", 640, 480, SDL_WINDOW_HIDDEN);
    SDL_Renderer* renderer = window ? createRenderer(window) : nullptr;
    if (!renderer) {
        tell(std::string("The launcher couldn't open its window: ") + SDL_GetError(), true);
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    float scale = displayScale(window);
    ts2::launcher::LauncherFonts fonts = restyle(scale);
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    ts2::launcher::LauncherState state;
    state.settingsFile = settingsFile();
    const auto loaded = ts2::settings::loadLauncherSettings(state.settingsFile);
    ts2::launcher::findInstalls(state, loaded.settings);
    state.message = loaded.problem;

    // ImGui works out a window's size over its first two frames, and the columns' widths in a third.
    constexpr int kLayoutFrames = 3;
    int frame = 0;
    bool running = true;
    while (running) {
        SDL_Event event;
        bool rescale = false;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
            rescale = rescale || event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED;
        }
        if (rescale && displayScale(window) != scale) {
            scale = displayScale(window);
            ImGui_ImplSDLRenderer3_DestroyFontsTexture();
            fonts = restyle(scale);
            ImGui_ImplSDLRenderer3_CreateFontsTexture();
        }
        ts2::launcher::watchGame(state);

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        // The window fills the SDL window, which is sized to what the contents ask for. Contents
        // taller than the screen scroll above the Play row.
        const ImGuiViewport* view = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(view->WorkPos);
        ImGui::SetNextWindowSize(view->WorkSize);
        ImGui::Begin("NeoTS2", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        state.maxWidth = static_cast<float>(roomFor(window).maxWidth());
        ts2::launcher::drawLauncher(state, window, fonts);
        const ImVec2 needed = neededSize(state, scale);
        ImGui::End();
        ImGui::Render();
        if (frame >= kLayoutFrames) {
            fitWindow(window, needed, frame == kLayoutFrames);
            if (frame == kLayoutFrames) {
                SDL_ShowWindow(window);
            }
        }
        frame = std::min(frame + 1, kLayoutFrames + 1);

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    // A game started from here keeps running when the launcher closes.
    if (state.game) {
        SDL_DestroyProcess(state.game);
    }
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
