#pragma once

#include "platform/platform.hpp"
#include "ui/screen.hpp"

#include <cstdint>
#include <string>

namespace app {

// A script's wrapped, editable text box (IGZWinTextEdit) with its upright scroll bar
// (IGZWinScrollbar2), as the neighborhood preview's description uses them. The bar shows only
// while the text has more lines than the box shows: its value is the line at the top, from 0 to
// the lines less those shown. A press on the track above or below the thumb moves a page (the
// lines shown less one), held it repeats every 300 ms while the mouse stays on that part of the
// track, and the thumb follows a drag. The thumb lights up under the mouse and shows its pressed
// frame while held. The wheel over the text moves a line for each full 100 of its delta.
class DescriptionBox {
public:
    // Takes the text box and the scroll bar inside it. `maxCharacters` is the script's maxtext.
    void attach(ts2::ui::Screen& screen, int box, std::size_t maxCharacters);
    // Shows `text` from its first line, with no caret until the box is clicked.
    void setText(ts2::ui::Screen& screen, const std::string& text);
    const std::string& text() const { return text_; }
    bool focused() const { return focused_; }
    void unfocus(ts2::ui::Screen& screen);

    // A left press at screen (x, y): on the bar, or in the box (which then takes the keyboard).
    // True when it was either.
    bool mouseDown(ts2::ui::Screen& screen, int x, int y, std::uint64_t ms);
    bool holdsMouse() const { return pressed_ != Part::None; }
    // Every mouse move: drags a held thumb, otherwise updates which part of the bar is under the mouse.
    void mouseMove(ts2::ui::Screen& screen, int x, int y);
    void mouseUp(ts2::ui::Screen& screen, int x, int y);
    // A wheel turn at screen (x, y), `delta` 120 a notch (positive: away from the player). True
    // when it was over the text.
    bool wheel(ts2::ui::Screen& screen, int x, int y, int delta);
    // Called each frame: repeats a held press on the track.
    void tick(ts2::ui::Screen& screen, std::uint64_t ms);

    void type(ts2::ui::Screen& screen, const std::string& typed);
    void key(ts2::ui::Screen& screen, ts2::platform::Key key);

private:
    enum class Part : std::uint8_t { None, PageUp, PageDown, Thumb };
    Part hit(const ts2::ui::Screen& screen, int x, int y) const;
    bool inText(const ts2::ui::Screen& screen, int x, int y) const;
    void showThumbFrame(ts2::ui::Screen& screen);
    void setHover(ts2::ui::Screen& screen, Part part);
    void show(ts2::ui::Screen& screen);
    void showBar(ts2::ui::Screen& screen);
    int visibleLines(const ts2::ui::Screen& screen) const;
    int lastTop(const ts2::ui::Screen& screen) const;
    void page(ts2::ui::Screen& screen, int direction);
    std::array<int, 2> corner(const ts2::ui::Screen& screen, int window) const;

    int box_ = -1, bar_ = -1;
    int track_[3] = {-1, -1, -1}, thumb_[3] = {-1, -1, -1};  // top cap, stretched middle, bottom cap
    int trackCap_ = 0, thumbCap_ = 0, thumbFrame_ = 0, thumbHeight_ = 0;
    int thumbLength_ = 0, thumbOffset_ = 0;
    std::size_t maxCharacters_ = 0;
    std::string text_;
    std::size_t caret_ = 0;
    bool focused_ = false;
    Part pressed_ = Part::None, hover_ = Part::None;
    int grab_ = 0;     // where in the thumb it was taken
    int paging_ = 0;   // a held press on the track: -1 above the thumb, 1 below
    std::uint64_t pagedAt_ = 0;
};

} // namespace app
