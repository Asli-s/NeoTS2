#pragma once

#include "content/ui_data.hpp"
#include "ui/fonts.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ts2::ui {

// A textured rectangle to draw, in screen pixels from the top left; texture -1 is a plain color.
struct Quad {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    int texture = -1;
    std::array<float, 4> color{1, 1, 1, 1};
};

// The windows on screen, built from UI scripts: what each draws (its image, its frame of a
// button's image strip, its text) and which button the mouse is over or pressing.
class Screen {
public:
    Screen(content::UiData& data, Fonts& fonts);

    // Adds a script's windows; the top window's area is moved to (x, y) (inside `parent`'s area when
    // given). Returns the top window, or -1 when the script is missing.
    int load(std::uint32_t group, std::uint32_t instance, int x, int y, int parent = -1);

    // A window by its id within the windows added from `root` (the first match), or -1.
    int find(int root, std::uint32_t id) const;
    // The same among the windows shown in `root` (as if `root` were shown).
    int findShown(int root, std::uint32_t id) const;
    void setVisible(int window, bool visible);
    bool visible(int window) const;
    void setEnabled(int window, bool enabled);  // a disabled button shows its disabled frame and can't be pressed
    void setChecked(int window, bool checked);  // a checked button shows its pressed frame
    // Only the buttons of this window and the windows inside it respond to the mouse (-1: all).
    void setModal(int window) { modal_ = window; }
    void setText(int window, const std::string& text);
    void setImage(int window, tga::Pixels pixels);  // drawn over the window's whole area
    // Textures added or changed since the last call (a window's own picture or text keeps its
    // texture when it changes).
    std::vector<int> takeChangedTextures();
    void move(int window, int x, int y);  // its area's top left, relative to its parent
    void resize(int window, int w, int h);
    // Draws a top window (one a script was loaded as) over the others.
    void raise(int window);
    // Takes a window and the windows inside it off the screen for good, freeing their pictures and
    // text; their numbers are given to windows added later.
    void remove(int window);
    // How wide a window's text is in its font, in pixels.
    int textWidth(int window);
    int x(int window) const { return windows_[static_cast<std::size_t>(window)].x; }
    int y(int window) const { return windows_[static_cast<std::size_t>(window)].y; }
    // A window's caption (its text).
    const std::string& text(int window) const { return windows_[static_cast<std::size_t>(window)].text; }
    // The texts of a window's children (a script's table of constants, written as captions).
    std::vector<std::string> childTexts(int window) const;
    int width(int window) const;
    int height(int window) const;

    // The mouse moved or a button changed (at `seconds`, for tooltips); returns the button released
    // over, if any.
    std::optional<int> mouse(int x, int y, bool down, double seconds = 0);
    // Whether (x, y) is over a shown window that draws something.
    bool over(int x, int y) const;
    // A button's tooltip text (empty: none).
    void setTip(int window, const std::string& text);
    std::uint32_t id(int window) const;
    int parent(int window) const;
    const std::vector<int>& children(int window) const { return windows_[static_cast<std::size_t>(window)].children; }

    // What to draw; a tooltip shows once the mouse has rested on a button for a while (at `seconds`).
    std::vector<Quad> quads(double seconds = 0) const;
    const std::vector<tga::Pixels>& textures() const { return textures_; }

private:
    enum class Kind { Generic, Bitmap, Button, Text, TextEdit, Rect, Other };
    enum class ButtonState { Normal, Hover, Pressed, Disabled };
    struct Window {
        Kind kind = Kind::Other;
        std::uint32_t id = 0;
        int parent = -1;
        std::vector<int> children;
        int x = 0, y = 0, w = 0, h = 0;  // relative to the parent
        bool visible = true;
        int image = -1;                  // texture
        bool ownImage = false;           // the texture is this window's own (not a shared UI image)
        std::array<int, 4> imageRect{};  // part of the image shown (imagerect), when set
        bool edge = false;               // the image is stretched around its border (edgeimage)
        bool toggle = false;             // a toggle button (style=toggle): pressed frame only while checked
        std::string text, font, align;
        std::array<float, 4> textColor{1, 1, 1, 1};
        std::array<int, 2> gutters{};
        bool wrapped = false, showCaption = true;
    bool paint = true;  // a generic window draws its image
        int textImage = -1;
        std::string tip;
        int tipImage = -1, tipBaseline = 0;
        int baseline = 0;
        ButtonState state = ButtonState::Normal;
        bool checked = false;
    };

    int add(const ui::Window& w, int parent);
    int texture(tga::Pixels pixels);
    void release(int texture);
    void layoutText(int window);
    std::array<int, 2> origin(int window) const;
    bool shown(int window) const;
    bool inside(int window, int root) const;
    void emit(int window, std::vector<Quad>& out) const;

    content::UiData& data_;
    Fonts& fonts_;
    std::vector<Window> windows_;
    std::vector<int> roots_;
    std::vector<tga::Pixels> textures_;
    std::map<std::uint64_t, int> images_;  // texture by image key
    int pressed_ = -1;
    int modal_ = -1;
    int hover_ = -1;          // the button the mouse is over
    double hoverSince_ = 0;
    int white_ = -1;          // a white texture for plain colors
    std::vector<int> changed_;
    std::vector<int> freeWindows_, freeTextures_;  // slots of removed windows and their pictures, for reuse
};

} // namespace ts2::ui
