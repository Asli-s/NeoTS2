// The platform layer on SDL 1.2, for systems with no SDL 3 (Mac OS X 10.4 and 10.5 on PowerPC).
// SDL 1.2 has one window with one OpenGL context, no keys by where they are, no hidden windows
// and cursors of black and white only. The folders, message boxes and system details it doesn't have
// are in system_unix.cpp, and the PNG files in png.cpp.
#include "platform/platform.hpp"

#include <SDL.h>

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <vector>

namespace ts2::platform {

namespace {

struct KeyInfo {
    Key key;
    SDLKey sym;
};

constexpr KeyInfo kKeys[] = {
    {Key::A, SDLK_a},
    {Key::D, SDLK_d},
    {Key::E, SDLK_e},
    {Key::P, SDLK_p},
    {Key::Q, SDLK_q},
    {Key::R, SDLK_r},
    {Key::S, SDLK_s},
    {Key::W, SDLK_w},
    {Key::X, SDLK_x},
    {Key::Y, SDLK_y},
    {Key::Z, SDLK_z},
    {Key::Num1, SDLK_1},
    {Key::Num2, SDLK_2},
    {Key::Num3, SDLK_3},
    {Key::Pad0, SDLK_KP0},
    {Key::Pad1, SDLK_KP1},
    {Key::Pad2, SDLK_KP2},
    {Key::Pad3, SDLK_KP3},
    {Key::PadPlus, SDLK_KP_PLUS},
    {Key::PadMinus, SDLK_KP_MINUS},
    {Key::Comma, SDLK_COMMA},
    {Key::Period, SDLK_PERIOD},
    {Key::Minus, SDLK_MINUS},
    {Key::Equals, SDLK_EQUALS},
    {Key::Grave, SDLK_BACKQUOTE},
    {Key::Tab, SDLK_TAB},
    {Key::Escape, SDLK_ESCAPE},
    {Key::Pause, SDLK_PAUSE},
    {Key::PageUp, SDLK_PAGEUP},
    {Key::PageDown, SDLK_PAGEDOWN},
    {Key::Home, SDLK_HOME},
    {Key::End, SDLK_END},
    {Key::F5, SDLK_F5},
    {Key::F9, SDLK_F9},
    {Key::F10, SDLK_F10},
    {Key::F11, SDLK_F11},
    {Key::Up, SDLK_UP},
    {Key::Down, SDLK_DOWN},
    {Key::Left, SDLK_LEFT},
    {Key::Right, SDLK_RIGHT},
};

Key keyOf(SDLKey sym) {
    for (const KeyInfo& k : kKeys) {
        if (k.sym == sym) {
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

// SDL 1.2's button masks are one bit for each button, from the left: the layout the game's is.
std::uint8_t buttonsOf(Uint8 state) {
    return static_cast<std::uint8_t>((state & SDL_BUTTON(SDL_BUTTON_LEFT) ? 1u << static_cast<unsigned>(MouseButton::Left) : 0u) |
                                     (state & SDL_BUTTON(SDL_BUTTON_MIDDLE) ? 1u << static_cast<unsigned>(MouseButton::Middle) : 0u) |
                                     (state & SDL_BUTTON(SDL_BUTTON_RIGHT) ? 1u << static_cast<unsigned>(MouseButton::Right) : 0u));
}

Window* gWindow = nullptr;            // the only one
SDL_Cursor* gDefaultCursor = nullptr; // the arrow the window started with
bool gContextMade = false;
bool gKeyDown[SDLK_LAST] = {};          // the keys held, to tell a press from its repeats
GlContext* gContext = nullptr;          // the context made with the window
std::thread::id gDrawingThread;         // the one that made it, where it is current

} // namespace

struct GlContext {};  // stands for the one context SDL makes with the window

struct Window::Impl {
    bool glWindow = false;
};

bool init() {
    return SDL_Init(SDL_INIT_VIDEO) == 0;
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
        case SDL_QUIT:
            out.type = Event::Type::Quit;
            return true;
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            out.type = e.type == SDL_KEYDOWN ? Event::Type::KeyDown : Event::Type::KeyUp;
            // SDL 1.2 knows a key by its symbol only, and sends a held key's repeats as presses again.
            out.key = out.scancode = keyOf(e.key.keysym.sym);
            if (const auto sym = static_cast<std::size_t>(e.key.keysym.sym); sym < std::size(gKeyDown)) {
                out.repeat = e.type == SDL_KEYDOWN && gKeyDown[sym];
                gKeyDown[sym] = e.type == SDL_KEYDOWN;
            }
            return true;
        case SDL_MOUSEMOTION:
            out.type = Event::Type::MouseMotion;
            out.x = e.motion.x;
            out.y = e.motion.y;
            out.xrel = e.motion.xrel;
            out.yrel = e.motion.yrel;
            out.buttons = buttonsOf(e.motion.state);
            return true;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            // The wheel is two more buttons here, pressed once a notch.
            if (e.button.button == SDL_BUTTON_WHEELUP || e.button.button == SDL_BUTTON_WHEELDOWN) {
                if (e.type == SDL_MOUSEBUTTONDOWN) {
                    out.type = Event::Type::MouseWheel;
                    out.wheel = e.button.button == SDL_BUTTON_WHEELUP ? 1.0f : -1.0f;
                    return true;
                }
                break;
            }
            out.type = e.type == SDL_MOUSEBUTTONDOWN ? Event::Type::MouseDown : Event::Type::MouseUp;
            out.button = buttonOf(e.button.button);
            out.x = e.button.x;
            out.y = e.button.y;
            return true;
        default:
            break;
        }
    }
    return false;
}

bool keyHeld(Key key) {
    const Uint8* state = SDL_GetKeyState(nullptr);
    for (const KeyInfo& k : kKeys) {
        if (k.key == key) {
            return state[k.sym] != 0;
        }
    }
    return false;
}

void mousePosition(float& x, float& y) {
    int ix = 0, iy = 0;
    SDL_GetMouseState(&ix, &iy);
    x = static_cast<float>(ix);
    y = static_cast<float>(iy);
}

Window::Window() : impl_(std::make_unique<Impl>()) {}

Window::~Window() {
    if (gWindow == this) {
        gWindow = nullptr;
    }
    // SDL 1.2's window goes with SDL_Quit.
}

std::unique_ptr<Window> Window::create(const WindowOptions& options) {
    if (gWindow != nullptr) {
        throw std::runtime_error("this system has one window only");
    }
    int width = options.width, height = options.height;
    // Larger than the screen isn't any use: take the screen's size less its menu bar and title bar.
    if (const SDL_VideoInfo* screen = SDL_GetVideoInfo(); screen != nullptr && screen->current_w > 0 && screen->current_h > 0) {
        width = std::min(width, screen->current_w);
        height = std::min(height, std::max(240, screen->current_h - 60));
    }
    Uint32 flags = 0;
    if (options.gl) {
        const GlRequest& gl = *options.gl;
        // The version and the profile aren't asked of SDL 1.2: the system gives the context it has.
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, gl.doubleBuffer ? 1 : 0);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, gl.depthBits);
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
        // Shown with the display, as the other platforms' are. This can't be asked after the window is made.
        SDL_GL_SetAttribute(SDL_GL_SWAP_CONTROL, 1);
        flags |= SDL_OPENGL;
    }
    // A window that is resized is made again by SDL 1.2, and its OpenGL context with it, which would lose
    // everything in it. So it stays the size it was made.
    SDL_WM_SetCaption(options.title.c_str(), options.title.c_str());
    SDL_Surface* screen = SDL_SetVideoMode(width, height, 0, flags);
    if (screen == nullptr && options.gl) {
        // A Mac set to "Thousands" of colours has no 8 bits for each: ask for what it has.
        const GlRequest& gl = *options.gl;
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, gl.doubleBuffer ? 1 : 0);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, gl.depthBits);
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 5);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 5);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 5);
        SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
        screen = SDL_SetVideoMode(width, height, 0, flags);
        if (screen == nullptr) {
            SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
            screen = SDL_SetVideoMode(width, height, 0, flags);
        }
    }
    if (screen == nullptr) {
        throw std::runtime_error(SDL_GetError());
    }
    // Held keys repeat as they do on the other platforms (the camera keys turn for as long as they are held).
    SDL_EnableKeyRepeat(SDL_DEFAULT_REPEAT_DELAY, SDL_DEFAULT_REPEAT_INTERVAL);
    gDefaultCursor = SDL_GetCursor();
    std::unique_ptr<Window> window(new Window);
    window->impl_->glWindow = options.gl.has_value();
    gWindow = window.get();
    // It can't be made hidden. A picture taken to a file shows the window for as long as it takes.
    return window;
}

void Window::setTitle(const std::string& title) {
    SDL_WM_SetCaption(title.c_str(), title.c_str());
}

void Window::show() {}

void Window::hide() {}

void Window::size(int& width, int& height) const {
    width = height = 0;
    if (const SDL_Surface* surface = SDL_GetVideoSurface()) {
        width = surface->w;
        height = surface->h;
    }
}

void Window::sizeInPixels(int& width, int& height) const {
    size(width, height);
}

bool Window::minimized() const {
    return (SDL_GetAppState() & SDL_APPACTIVE) == 0;
}

void Window::warpMouse(float x, float y) {
    SDL_WarpMouse(static_cast<Uint16>(std::max(0.0f, x)), static_cast<Uint16>(std::max(0.0f, y)));
}

void* Window::nativeHandle() const {
    return nullptr;
}

GlContextPtr Window::createGlContext(bool shared) {
    // The context is made with the window, once, and there is no making another to share with.
    if (shared || !impl_->glWindow || gContextMade) {
        return nullptr;
    }
    gContextMade = true;
    gContext = new GlContext;
    gDrawingThread = std::this_thread::get_id();
    return GlContextPtr(gContext);
}

bool Window::makeCurrent(GlContext* context) {
    // The one context is current on the thread that made the window, and stays: no other thread can have it,
    // and nothing releases it.
    return context != nullptr && context == gContext && std::this_thread::get_id() == gDrawingThread;
}

void Window::swapBuffers() {
    SDL_GL_SwapBuffers();
}

void Window::setSwapInterval(int) {}

void GlContextDeleter::operator()(GlContext* context) const {
    if (context == gContext) {
        gContext = nullptr;
    }
    delete context;
    gContextMade = false;
}

Window* mouseFocus() {
    return (SDL_GetAppState() & SDL_APPMOUSEFOCUS) != 0 ? gWindow : nullptr;
}

void* glProcAddress(const char* name) {
    return SDL_GL_GetProcAddress(name);
}

CursorPtr createCursor(const std::uint8_t* rgba, unsigned width, unsigned height, unsigned hotX, unsigned hotY) {
    // A picture of two shades and what is clear: dark where the picture is dark, white where it is
    // light, and clear where it is more clear than not. Rows are a whole number of bytes long.
    const unsigned stride = (width + 7) / 8;
    std::vector<Uint8> data(std::size_t{stride} * height, 0), mask(std::size_t{stride} * height, 0);
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            const std::uint8_t* pixel = rgba + (std::size_t{y} * width + x) * 4;
            if (pixel[3] < 128) {
                continue;
            }
            const unsigned light = (pixel[0] * 30u + pixel[1] * 59u + pixel[2] * 11u) / 100u;
            const Uint8 bit = static_cast<Uint8>(0x80u >> (x % 8));
            mask[std::size_t{y} * stride + x / 8] |= bit;
            if (light < 128) {
                data[std::size_t{y} * stride + x / 8] |= bit;
            }
        }
    }
    SDL_Cursor* made = SDL_CreateCursor(data.data(), mask.data(), static_cast<int>(stride * 8), static_cast<int>(height), static_cast<int>(hotX),
                                        static_cast<int>(hotY));
    return CursorPtr(reinterpret_cast<Cursor*>(made));
}

void setCursor(const Cursor* cursor) {
    SDL_Cursor* shown = cursor ? reinterpret_cast<SDL_Cursor*>(const_cast<Cursor*>(cursor)) : gDefaultCursor;
    if (shown != nullptr) {
        SDL_SetCursor(shown);
    }
}

void showCursor() {
    SDL_ShowCursor(SDL_ENABLE);
}

void hideCursor() {
    SDL_ShowCursor(SDL_DISABLE);
}

void CursorDeleter::operator()(Cursor* cursor) const {
    SDL_FreeCursor(reinterpret_cast<SDL_Cursor*>(cursor));
}

} // namespace ts2::platform
