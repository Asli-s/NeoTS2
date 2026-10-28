// The platform layer on SDL 3.
#include "platform/platform.hpp"

#include "platform/sdl3/sdl3_window.hpp"
#include "platform/sdl3/window_frame.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace ts2::platform {

namespace {

// Sizes and places a window to fill its screen's free area, title bar and borders included. The
// window is left as it is when the system can't say where that area is or how tall its title bar is.
// Some systems only know a window's borders once it is shown, and report none before.
void fitScreen(SDL_Window* window) {
    SDL_Rect usable{};
    int top = 0, left = 0, bottom = 0, right = 0;
    if (!SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(window), &usable) ||
        !SDL_GetWindowBordersSize(window, &top, &left, &bottom, &right) || top <= 0) {
        return;
    }
    const int width = usable.w - left - right, height = usable.h - top - bottom;
    if (width <= 0 || height <= 0) {
        return;
    }
    SDL_SetWindowSize(window, width, height);
    SDL_SetWindowPosition(window, usable.x + left, usable.y + top);
    SDL_SyncWindow(window);
    // As with a maximized window, the frame's invisible parts may lie outside the free area, so the
    // visible frame meets its edges.
    const HiddenFrame hidden = hiddenFrame(window);
    if (hidden.left || hidden.top || hidden.right || hidden.bottom) {
        SDL_SetWindowSize(window, width + hidden.left + hidden.right, height + hidden.top + hidden.bottom);
        SDL_SetWindowPosition(window, usable.x + left - hidden.left, usable.y + top - hidden.top);
        SDL_SyncWindow(window);
    }
}

// Each key by what is printed on it and by where it is.
struct KeyInfo {
    Key key;
    SDL_Keycode code;
    SDL_Scancode scan;
};

constexpr KeyInfo kKeys[] = {
    {Key::A, SDLK_A, SDL_SCANCODE_A},
    {Key::D, SDLK_D, SDL_SCANCODE_D},
    {Key::E, SDLK_E, SDL_SCANCODE_E},
    {Key::P, SDLK_P, SDL_SCANCODE_P},
    {Key::Q, SDLK_Q, SDL_SCANCODE_Q},
    {Key::R, SDLK_R, SDL_SCANCODE_R},
    {Key::S, SDLK_S, SDL_SCANCODE_S},
    {Key::W, SDLK_W, SDL_SCANCODE_W},
    {Key::X, SDLK_X, SDL_SCANCODE_X},
    {Key::Y, SDLK_Y, SDL_SCANCODE_Y},
    {Key::Z, SDLK_Z, SDL_SCANCODE_Z},
    {Key::Num1, SDLK_1, SDL_SCANCODE_1},
    {Key::Num2, SDLK_2, SDL_SCANCODE_2},
    {Key::Num3, SDLK_3, SDL_SCANCODE_3},
    {Key::Pad0, SDLK_KP_0, SDL_SCANCODE_KP_0},
    {Key::Pad1, SDLK_KP_1, SDL_SCANCODE_KP_1},
    {Key::Pad2, SDLK_KP_2, SDL_SCANCODE_KP_2},
    {Key::Pad3, SDLK_KP_3, SDL_SCANCODE_KP_3},
    {Key::PadPlus, SDLK_KP_PLUS, SDL_SCANCODE_KP_PLUS},
    {Key::PadMinus, SDLK_KP_MINUS, SDL_SCANCODE_KP_MINUS},
    {Key::Comma, SDLK_COMMA, SDL_SCANCODE_COMMA},
    {Key::Period, SDLK_PERIOD, SDL_SCANCODE_PERIOD},
    {Key::Minus, SDLK_MINUS, SDL_SCANCODE_MINUS},
    {Key::Equals, SDLK_EQUALS, SDL_SCANCODE_EQUALS},
    {Key::Grave, SDLK_GRAVE, SDL_SCANCODE_GRAVE},
    {Key::Tab, SDLK_TAB, SDL_SCANCODE_TAB},
    {Key::Escape, SDLK_ESCAPE, SDL_SCANCODE_ESCAPE},
    {Key::Pause, SDLK_PAUSE, SDL_SCANCODE_PAUSE},
    {Key::PageUp, SDLK_PAGEUP, SDL_SCANCODE_PAGEUP},
    {Key::PageDown, SDLK_PAGEDOWN, SDL_SCANCODE_PAGEDOWN},
    {Key::Home, SDLK_HOME, SDL_SCANCODE_HOME},
    {Key::End, SDLK_END, SDL_SCANCODE_END},
    {Key::F5, SDLK_F5, SDL_SCANCODE_F5},
    {Key::Up, SDLK_UP, SDL_SCANCODE_UP},
    {Key::Down, SDLK_DOWN, SDL_SCANCODE_DOWN},
    {Key::Left, SDLK_LEFT, SDL_SCANCODE_LEFT},
    {Key::Right, SDLK_RIGHT, SDL_SCANCODE_RIGHT},
};

Key keyOf(SDL_Keycode code) {
    for (const KeyInfo& k : kKeys) {
        if (k.code == code) {
            return k.key;
        }
    }
    return Key::Unknown;
}

Key keyOf(SDL_Scancode scan) {
    for (const KeyInfo& k : kKeys) {
        if (k.scan == scan) {
            return k.key;
        }
    }
    return Key::Unknown;
}

MouseButton buttonOf(Uint8 button) {
    switch (button) {
    case SDL_BUTTON_LEFT: return MouseButton::Left;
    case SDL_BUTTON_MIDDLE: return MouseButton::Middle;
    case SDL_BUTTON_RIGHT: return MouseButton::Right;
    default: return MouseButton::Other;
    }
}

std::uint8_t buttonsOf(SDL_MouseButtonFlags state) {
    return static_cast<std::uint8_t>((state & SDL_BUTTON_LMASK ? 1u << static_cast<unsigned>(MouseButton::Left) : 0u) |
                                     (state & SDL_BUTTON_MMASK ? 1u << static_cast<unsigned>(MouseButton::Middle) : 0u) |
                                     (state & SDL_BUTTON_RMASK ? 1u << static_cast<unsigned>(MouseButton::Right) : 0u));
}

std::filesystem::path utf8Path(const char* text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text)));
}

std::vector<Window*>& windows() {
    static std::vector<Window*> all;
    return all;
}

} // namespace

struct Window::Impl {
    SDL_Window* window = nullptr;
};

bool init() {
    return SDL_Init(SDL_INIT_VIDEO);
}

void quit() {
    SDL_Quit();
}

std::string lastError() {
    return SDL_GetError();
}

bool pollEvent(Event& out) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        out = Event{};
        switch (e.type) {
        case SDL_EVENT_QUIT:
            out.type = Event::Type::Quit;
            return true;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            out.type = e.type == SDL_EVENT_KEY_DOWN ? Event::Type::KeyDown : Event::Type::KeyUp;
            out.key = keyOf(e.key.key);
            out.scancode = keyOf(e.key.scancode);
            out.repeat = e.key.repeat;
            return true;
        case SDL_EVENT_MOUSE_MOTION:
            out.type = Event::Type::MouseMotion;
            out.x = e.motion.x;
            out.y = e.motion.y;
            out.xrel = e.motion.xrel;
            out.yrel = e.motion.yrel;
            out.buttons = buttonsOf(e.motion.state);
            return true;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            out.type = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? Event::Type::MouseDown : Event::Type::MouseUp;
            out.button = buttonOf(e.button.button);
            out.x = e.button.x;
            out.y = e.button.y;
            return true;
        case SDL_EVENT_MOUSE_WHEEL:
            out.type = Event::Type::MouseWheel;
            out.wheel = e.wheel.y;
            return true;
        default:
            break;
        }
    }
    return false;
}

bool keyHeld(Key key) {
    const bool* state = SDL_GetKeyboardState(nullptr);
    for (const KeyInfo& k : kKeys) {
        if (k.key == key) {
            return state[k.scan];
        }
    }
    return false;
}

void mousePosition(float& x, float& y) {
    SDL_GetMouseState(&x, &y);
}

Window::Window() : impl_(std::make_unique<Impl>()) {}

Window::~Window() {
    auto& all = windows();
    all.erase(std::remove(all.begin(), all.end(), this), all.end());
    if (impl_->window) {
        SDL_DestroyWindow(impl_->window);
    }
}

std::unique_ptr<Window> Window::create(const WindowOptions& options) {
    SDL_WindowFlags flags = 0;
    if (options.resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    if (options.highDensity) {
        flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
    }
    if (options.hidden || options.fitScreen) {
        flags |= SDL_WINDOW_HIDDEN;  // a fitted window is shown once it is in place
    }
    if (options.gl) {
        const GlRequest& gl = *options.gl;
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, gl.major);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, gl.minor);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, gl.compatibility ? SDL_GL_CONTEXT_PROFILE_COMPATIBILITY : SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, gl.doubleBuffer ? 1 : 0);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, gl.depthBits);
        flags |= SDL_WINDOW_OPENGL;
    }
    SDL_Window* made = SDL_CreateWindow(options.title.c_str(), options.width, options.height, flags);
    if (made == nullptr) {
        throw std::runtime_error(SDL_GetError());
    }
    if (options.fitScreen) {
        fitScreen(made);
        if (!options.hidden) {
            SDL_ShowWindow(made);
        }
    }
    std::unique_ptr<Window> window(new Window);
    window->impl_->window = made;
    windows().push_back(window.get());
    return window;
}

void Window::setTitle(const std::string& title) {
    SDL_SetWindowTitle(impl_->window, title.c_str());
}

void Window::show() {
    SDL_ShowWindow(impl_->window);
}

void Window::hide() {
    SDL_HideWindow(impl_->window);
}

void Window::size(int& width, int& height) const {
    width = height = 0;
    SDL_GetWindowSize(impl_->window, &width, &height);
}

void Window::sizeInPixels(int& width, int& height) const {
    width = height = 0;
    SDL_GetWindowSizeInPixels(impl_->window, &width, &height);
}

bool Window::minimized() const {
    return (SDL_GetWindowFlags(impl_->window) & SDL_WINDOW_MINIMIZED) != 0;
}

void Window::warpMouse(float x, float y) {
    SDL_WarpMouseInWindow(impl_->window, x, y);
}

void* Window::nativeHandle() const {
#ifdef _WIN32
    return SDL_GetPointerProperty(SDL_GetWindowProperties(impl_->window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#else
    return nullptr;
#endif
}

GlContextPtr Window::createGlContext(bool shared) {
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, shared ? 1 : 0);
    SDL_GLContext context = SDL_GL_CreateContext(impl_->window);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
    return GlContextPtr(reinterpret_cast<GlContext*>(context));
}

bool Window::makeCurrent(GlContext* context) {
    return SDL_GL_MakeCurrent(impl_->window, reinterpret_cast<SDL_GLContext>(context));
}

void Window::swapBuffers() {
    SDL_GL_SwapWindow(impl_->window);
}

void Window::setSwapInterval(int interval) {
    SDL_GL_SetSwapInterval(interval);
}

void GlContextDeleter::operator()(GlContext* context) const {
    SDL_GL_DestroyContext(reinterpret_cast<SDL_GLContext>(context));
}

SDL_Window* sdlWindow(Window& window) {
    return window.impl().window;
}

Window* mouseFocus() {
    SDL_Window* focus = SDL_GetMouseFocus();
    for (Window* w : windows()) {
        if (w->impl().window == focus) {
            return w;
        }
    }
    return nullptr;
}

void* glProcAddress(const char* name) {
    return reinterpret_cast<void*>(SDL_GL_GetProcAddress(name));
}

CursorPtr createCursor(const std::uint8_t* rgba, unsigned width, unsigned height, unsigned hotX, unsigned hotY) {
    SDL_Surface* surface = SDL_CreateSurfaceFrom(static_cast<int>(width), static_cast<int>(height), SDL_PIXELFORMAT_RGBA32,
                                                 const_cast<std::uint8_t*>(rgba), static_cast<int>(width * 4));
    if (!surface) {
        return nullptr;
    }
    SDL_Cursor* made = SDL_CreateColorCursor(surface, static_cast<int>(hotX), static_cast<int>(hotY));
    SDL_DestroySurface(surface);
    return CursorPtr(reinterpret_cast<Cursor*>(made));
}

void setCursor(const Cursor* cursor) {
    SDL_SetCursor(cursor ? reinterpret_cast<SDL_Cursor*>(const_cast<Cursor*>(cursor)) : SDL_GetDefaultCursor());
}

void showCursor() {
    SDL_ShowCursor();
}

void hideCursor() {
    // Real pointer moves, not the raw relative input SDL would switch to (that skips the system's
    // pointer speed and acceleration).
    SDL_SetHint(SDL_HINT_MOUSE_EMULATE_WARP_WITH_RELATIVE, "0");
    SDL_HideCursor();
}

void CursorDeleter::operator()(Cursor* cursor) const {
    SDL_DestroyCursor(reinterpret_cast<SDL_Cursor*>(cursor));
}

void showMessageBox(MessageKind kind, const std::string& title, const std::string& text, Window* parent) {
    const SDL_MessageBoxFlags flags = kind == MessageKind::Error     ? SDL_MESSAGEBOX_ERROR
                                      : kind == MessageKind::Warning ? SDL_MESSAGEBOX_WARNING
                                                                     : SDL_MESSAGEBOX_INFORMATION;
    SDL_ShowSimpleMessageBox(flags, title.c_str(), text.c_str(), parent ? parent->impl().window : nullptr);
}

std::filesystem::path basePath() {
    const char* base = SDL_GetBasePath();
    return base ? utf8Path(base) : std::filesystem::path();
}

std::filesystem::path prefPath(const char* application) {
    std::filesystem::path folder;
    if (char* data = SDL_GetPrefPath(nullptr, application)) {
        folder = utf8Path(data);
        SDL_free(data);
    }
    return folder;
}

std::filesystem::path documentsFolder() {
    const char* documents = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    return documents ? utf8Path(documents) : std::filesystem::path();
}

SystemInfo systemInfo() {
    SystemInfo info;
    info.name = SDL_GetPlatform();
    info.cores = SDL_GetNumLogicalCPUCores();
    info.memoryMb = SDL_GetSystemRAM();
    return info;
}

bool savePng(const std::filesystem::path& path, const std::uint8_t* rgba, unsigned width, unsigned height) {
    SDL_Surface* surface = SDL_CreateSurfaceFrom(static_cast<int>(width), static_cast<int>(height), SDL_PIXELFORMAT_RGBA32,
                                                 const_cast<std::uint8_t*>(rgba), static_cast<int>(width * 4));
    const bool saved = surface != nullptr && SDL_SavePNG(surface, path.string().c_str());
    SDL_DestroySurface(surface);
    return saved;
}

} // namespace ts2::platform
