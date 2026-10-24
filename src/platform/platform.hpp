// What the game needs of the system under it: a window with an OpenGL context, the keyboard and
// mouse, cursors, message boxes, and the folders it keeps things in. Behind it is SDL 3, or SDL 1.2
// where there is no SDL 3 (Mac OS X on PowerPC), chosen when the game is built (NEOTS2_PLATFORM).
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace ts2::platform {

// Starts the video system. False, with lastError() saying why, when it can't.
[[nodiscard]] bool init();
void quit();
[[nodiscard]] std::string lastError();

// Nanoseconds since the program started its video system, and a pause for the thread.
[[nodiscard]] std::uint64_t ticksNs();
void delayMs(std::uint32_t milliseconds);

// The keys the game uses. A key it doesn't use is Unknown.
enum class Key : std::uint8_t {
    Unknown,
    A, D, E, P, Q, R, S, W, X, Y, Z,
    Num1, Num2, Num3,
    Pad0, Pad1, Pad2, Pad3, PadPlus, PadMinus,
    Comma, Period, Minus, Equals, Grave,
    Tab, Escape, Pause,
    PageUp, PageDown, Home, End,
    F5,
    Up, Down, Left, Right,
};

enum class MouseButton : std::uint8_t { Left, Middle, Right, Other };

struct Event {
    enum class Type : std::uint8_t { Quit, KeyDown, KeyUp, MouseMotion, MouseDown, MouseUp, MouseWheel };
    Type type = Type::Quit;
    // A key event: the key by what is printed on it (so the Q key of a French keyboard is A), and by
    // where it is (so W is above S on every layout). The same where the system doesn't tell them apart.
    Key key = Key::Unknown;
    Key scancode = Key::Unknown;
    bool repeat = false;  // the key is held and this is not its first press
    // A mouse event: the pointer in the window's own units, and for motion how far it moved.
    float x = 0, y = 0;
    float xrel = 0, yrel = 0;
    MouseButton button = MouseButton::Left;  // for a button's press or release
    std::uint8_t buttons = 0;                // for motion, the buttons held: one bit for each MouseButton
    float wheel = 0;                         // for the wheel, notches (positive: away from the player)

    [[nodiscard]] bool held(MouseButton b) const { return (buttons >> static_cast<unsigned>(b)) & 1u; }
};

// The next event the game uses, false when there is none. Others are skipped.
[[nodiscard]] bool pollEvent(Event& out);
[[nodiscard]] bool keyHeld(Key key);
// The pointer's place in the window that has it.
void mousePosition(float& x, float& y);

// The OpenGL context a window was asked for.
struct GlRequest {
    int major = 2, minor = 1;
    bool compatibility = true;  // the old fixed-function entry points kept, as the oldest drivers have it
    int depthBits = 24;
    bool doubleBuffer = true;
};

struct WindowOptions {
    std::string title;
    int width = 1280, height = 800;
    bool resizable = true;
    bool hidden = false;
    bool highDensity = true;      // as many pixels as the screen has, where it has more than points
    std::optional<GlRequest> gl;  // a window to draw in with OpenGL
};

// An OpenGL context. Made by a window, and dropped before it.
struct GlContext;
struct GlContextDeleter {
    void operator()(GlContext* context) const;
};
using GlContextPtr = std::unique_ptr<GlContext, GlContextDeleter>;

class Window {
public:
    // Throws a runtime_error saying why when the window can't be made.
    [[nodiscard]] static std::unique_ptr<Window> create(const WindowOptions& options);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    void setTitle(const std::string& title);
    void show();
    void hide();
    // The size in the window's own units (the mouse's), and in pixels (what is drawn into).
    void size(int& width, int& height) const;
    void sizeInPixels(int& width, int& height) const;
    [[nodiscard]] bool minimized() const;
    void warpMouse(float x, float y);
    // The system's handle for the window (a Windows HWND), null where it has none to give.
    [[nodiscard]] void* nativeHandle() const;

    // OpenGL. A context shares its objects with the one that is current when `shared`. Null when
    // none can be made (a system with one context only makes the first, and not a shared one).
    [[nodiscard]] GlContextPtr createGlContext(bool shared = false);
    // Makes `context` current on this thread (none: releases the thread's). False when it can't.
    bool makeCurrent(GlContext* context);
    void swapBuffers();
    void setSwapInterval(int interval);

    // What the backend keeps for the window.
    struct Impl;
    [[nodiscard]] Impl& impl() const { return *impl_; }

private:
    Window();
    std::unique_ptr<Impl> impl_;
};

// The window the pointer is over, if it is one of the game's.
[[nodiscard]] Window* mouseFocus();
// An OpenGL function of the current context, null if there is none by that name.
[[nodiscard]] void* glProcAddress(const char* name);

// A mouse cursor.
struct Cursor;
struct CursorDeleter {
    void operator()(Cursor* cursor) const;
};
using CursorPtr = std::unique_ptr<Cursor, CursorDeleter>;

// A cursor from RGBA pixels, rows from the top. Null when it can't be made. A system with only black
// and white cursors makes them from the picture's light and dark.
[[nodiscard]] CursorPtr createCursor(const std::uint8_t* rgba, unsigned width, unsigned height, unsigned hotX, unsigned hotY);
// Shows a cursor (none: the system's arrow).
void setCursor(const Cursor* cursor);
void showCursor();
// Hides the pointer. Moves are still the pointer's own, so the game can put it back after each one.
void hideCursor();

enum class MessageKind { Information, Warning, Error };

// A message in a box of the system's own, the player answers with OK. `parent` is the window it
// belongs to, if any. Waits until it is closed.
void showMessageBox(MessageKind kind, const std::string& title, const std::string& text, Window* parent);

// The folder the program is in (empty when that isn't known), with a separator at the end.
[[nodiscard]] std::filesystem::path basePath();
// The folder for the program's own files, made if it isn't there (empty when there is none).
[[nodiscard]] std::filesystem::path prefPath(const char* application);
// The player's Documents folder (empty when that isn't known).
[[nodiscard]] std::filesystem::path documentsFolder();

struct SystemInfo {
    std::string name;  // the system's name, as the log shows it
    int cores = 1;     // logical
    int memoryMb = 0;
};
[[nodiscard]] SystemInfo systemInfo();

// Writes RGBA pixels, rows from the top, as a PNG file.
[[nodiscard]] bool savePng(const std::filesystem::path& path, const std::uint8_t* rgba, unsigned width, unsigned height);

} // namespace ts2::platform
