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

// A textured rectangle to draw, in screen pixels from the top left. Texture -1 is a plain color.
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

    // The game's UI scripts, pictures and texts the windows come from.
    content::UiData& data() { return data_; }

    // Adds a script's windows. The top window's area is moved to (x, y) (inside `parent`'s area when
    // given). Returns the top window, or -1 when the script is missing.
    int load(std::uint32_t group, std::uint32_t instance, int x, int y, int parent = -1);
    // The same for windows described in code the way a script describes them.
    int addWindows(const std::vector<ui::Window>& windows, int x, int y, int parent = -1);

    // A window by its id within the windows added from `root` (the first match), or -1.
    int find(int root, std::uint32_t id) const;
    // A top window by its id, the one drawn last when several match, or -1. A script can have more
    // than one top window, and load returns only its first.
    int findTop(std::uint32_t id) const;
    // The same among the windows shown in `root` (as if `root` were shown).
    int findShown(int root, std::uint32_t id) const;
    void setVisible(int window, bool visible);
    void setPassive(int window);  // the mouse goes through the window and its children
    bool visible(int window) const;
    void setEnabled(int window, bool enabled);  // a disabled button shows its disabled frame and can't be pressed
    void setChecked(int window, bool checked);  // a checked button shows its pressed frame
    // Only the buttons of this window and the windows inside it respond to the mouse (-1: all).
    void setModal(int window) { modal_ = window; }
    int modal() const { return modal_; }
    void setText(int window, const std::string& text);
    // A text box's selection (bytes `begin` to `end` of its text, shown in its highlight colour, none
    // when they are equal) and its caret (before byte `at`, none when not given).
    void setSelection(int window, std::size_t begin, std::size_t end);
    void setCaret(int window, std::optional<std::size_t> at);
    // The place in a window's text (a byte index between characters) nearest to screen x.
    std::size_t textIndexAt(int window, int x);
    void setTextColor(int window, const std::array<float, 4>& color);
    void setImage(int window, tga::Pixels pixels);  // drawn over the window's whole area
    // Textures added or changed since the last call (a window's own picture or text keeps its
    // texture when it changes).
    std::vector<int> takeChangedTextures();
    void move(int window, int x, int y);  // its area's top left, relative to its parent
    void resize(int window, int w, int h);
    // Draws a top window (one a script was loaded as) over the others.
    void raise(int window);
    // Takes a window and the windows inside it off the screen for good, freeing their pictures and
    // text. Their numbers are given to windows added later.
    void remove(int window);
    // How wide a window's text is in its font, in pixels.
    int textWidth(int window);
    // Breaks the window's text into lines that fit its width (from now on).
    void setWrapped(int window, bool wrapped);
    // The height of the window's text as laid out (0 without text).
    int textHeight(int window) const;
    // Makes the window as high as its text plus its top and bottom gutters (autosize). Our text image
    // is 2 pixels taller than its lines, so the last line's descenders aren't cut.
    void fitHeightToText(int window);
    // Makes the window as wide and high as its text plus its gutters.
    void fitToText(int window);
    // How wide and high a text is in a font (by name or GUID), in pixels.
    std::array<int, 2> textSize(const std::string& font, const std::string& text);
    // A text broken into lines no wider than `width` in a font, as wrapped windows break theirs.
    std::vector<std::string> wrapText(const std::string& font, const std::string& text, int width);
    int x(int window) const;
    int y(int window) const;
    // A window's caption (its text).
    const std::string& text(int window) const;
    // The texts of a window's children (a script's table of constants, written as captions).
    std::vector<std::string> childTexts(int window) const;
    int width(int window) const;
    int height(int window) const;

    // The mouse moved or a button changed (at `seconds`, for tooltips). Returns the button released
    // over, if any.
    std::optional<int> mouse(int x, int y, bool down, double seconds = 0);
    // Whether a button is held down with the mouse still over it.
    bool held(int window) const;
    // The topmost shown, enabled button at (x, y), if any.
    std::optional<int> buttonAt(int x, int y) const;
    // Whether (x, y) is over a shown window that draws something.
    bool over(int x, int y) const;
    // A window's tooltip text (empty: none).
    void setTip(int window, const std::string& text);
    // A tip pointing at a spot rather than a window, as the game's tip over a Sim in the world: one
    // line in the Default style, shown at once at the spot set by moveSpotTip. Empty text takes it
    // away.
    void setSpotTip(const std::string& text);
    void moveSpotTip(int x, int y) { spotTip_.at = {x, y}; }
    std::uint32_t id(int window) const;
    int parent(int window) const;
    const std::vector<int>& children(int window) const;

    // The screen's size, which tooltips stay inside (0: no limits).
    void setSize(int width, int height) { size_ = {width, height}; }

    // What to draw. A tooltip shows once the mouse has rested on a button for a while (at `seconds`).
    std::vector<Quad> quads(double seconds = 0) const;
    const std::vector<tga::Pixels>& textures() const { return textures_; }

private:
    enum class Kind { Generic, Bitmap, Button, Text, TextEdit, Rect, Other };
    enum class ButtonState { Normal, Hover, Pressed, Disabled };
    enum class HAlign { Left, Centre, Right };
    enum class VAlign { Top, Middle, Bottom };
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
        bool passive = false;            // shown only: the mouse goes through it to what is under it
        // Drawn through a buffer whose magenta (255, 0, 255) shows through (winflag_pbufftrans), as
        // is everything inside it.
        bool keyed = false;
        bool toggle = false;             // a toggle button (style=toggle): pressed frame only while checked
        bool autosize = false;           // a button sized to its caption (autosize)
        bool checkBox = false;           // a check box or radio button (style=checkbox, radiobtn or radiocheck)
        std::string text, font;
        HAlign hAlign = HAlign::Left;  // the text's place in the window (align)
        VAlign vAlign = VAlign::Top;
        std::array<float, 4> textColor{1, 1, 1, 1};
        std::optional<std::array<float, 4>> disabledTextColor;  // a button's text while disabled
        std::array<int, 4> gutters{};  // left, top, right, bottom
        bool wrapped = false, showCaption = true;
        bool wrapCaption = false;  // a button's caption measured as wrapped lines (wrapcaption)
        // A button caption's lines as the game's font engine measures them (Type 1 faces), and how
        // many lines it has. The caption is then placed as the game places it.
        std::optional<LineMetrics> captionLine;
        int lines = 1;
        bool paint = true;  // a generic window draws its image
        int textImage = -1;
        std::string tip;
        // The tip's lines drawn into one image, where its first baseline is, and its widest line.
        int tipImage = -1, tipBaseline = 0, tipWidth = 0, tipLines = 0;
        bool tipTimeout = true;  // the tip hides after a while (tipstimeout)
        int baseline = 0;
        // A text window in a Type 1 font: each line's width without trailing spaces (empty for
        // other text), the height of a line's band in the text image, and, for it and a button
        // caption in a Type 1 font, the step between lines and the first baseline's distance below
        // the top of the text, all as the game measures them.
        std::vector<int> lineWidths;
        int band = 0, lineStep = 0, firstBaseline = 0;
        ButtonState state = ButtonState::Normal;
        bool checked = false;
        // A text box: its selection in bytes, its caret's byte, and their colours (highlightcolor,
        // colorfonthilited, caretcolor).
        std::size_t selectionBegin = 0, selectionEnd = 0;
        std::optional<std::size_t> caret;
        std::array<float, 4> highlightColor{0, 0, 1, 1}, selectedTextColor{1, 1, 1, 1}, caretColor{0, 0, 0, 1};
    };

    int add(const ui::Window& w, int parent);
    int texture(tga::Pixels pixels);
    void release(int texture);
    void layoutText(int window);
    std::vector<std::string> wrapLines(const Window& w, const FontStyle& style) const;
    std::vector<std::string> wrapAt(const std::string& text, const FontStyle& style, int room) const;
    void emitLines(const Window& w, float x0, float y0, std::vector<Quad>& out) const;
    // Draws a window's tooltip text into an image, if it has a tip and none is drawn yet.
    void drawTip(Window& window);
    // A tip's frame, w by h at (x, y), and its text image with its baseline at `baselineY`. The frame
    // is a dark blue border and a see-through beige inside on all but its last 2 columns and 3 rows,
    // which hold a soft shadow.
    void emitTip(int x, int y, int w, int h, int image, int baseline, int baselineY, std::vector<Quad>& out) const;
    // The topmost shown window with a tip under the mouse, or -1 when none. With `within`, only
    // windows inside that one count.
    int tipWindowAt(int x, int y, int within = -1) const;
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
    std::map<std::uint64_t, int> keyedImages_;  // the same with their magenta made transparent
    int pressed_ = -1;
    int modal_ = -1;
    std::array<int, 2> size_{0, 0};
    int hover_ = -1;          // the button with a tip, or else the window with a tip, the mouse is over
    struct SpotTip {
        std::string text;
        int image = -1, width = 0, baseline = 0;
        std::array<int, 2> at{0, 0};
    } spotTip_;
    double hoverSince_ = 0;
    int white_ = -1;          // a white texture for plain colors
    std::vector<int> changed_;
    std::vector<int> freeWindows_, freeTextures_;  // slots of removed windows and their pictures, for reuse
};

} // namespace ts2::ui
