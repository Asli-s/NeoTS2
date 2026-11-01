#pragma once

#include <cstdint>

namespace ts2::ui {

// The game's upright scroll bar with arrows, as its text windows use it. An arrow cell 16 high at
// each end, the track between them and a thumb cell 16 high on it. Its value runs 0..1 and gives
// the position, the top line shown, from 0 to the last line that can be on top.
//
// A press on an arrow moves one line, and a press on the track above or below the thumb moves one
// page. Held, the press repeats each time more than 200 ms have passed while the mouse stays on
// that part. The thumb follows the mouse, centred on it, while it is dragged.
class ScrollBar {
public:
    enum class Part { None, Up, Down, PageUp, PageDown, Thumb };
    static constexpr int kCell = 16;
    static constexpr std::uint64_t kRepeatMs = 200;

    // The bar's height in pixels, kept taller than three cells so the thumb has room to move.
    void setHeight(int height);
    int height() const { return height_; }

    // What a text window of `lines` lines showing `visible` of them sets. The last position is
    // lines - visible (at least 0). There are lines / visible + 1 pages, and a page step is the
    // positions plus one, split into that many pages. With nothing visible the bar is left as it is.
    void setLines(int lines, int visible);
    int lastPosition() const { return last_; }

    // The position: the value times the last position, rounded.
    int position() const;
    // Moves to `position`, kept within 0..last. False when it was there already.
    bool setPosition(int position);

    // The thumb's top, in pixels from the bar's top. At the top while there is nothing to scroll.
    int thumbTop() const;
    // The part at `y` pixels from the bar's top.
    Part partAt(int y) const;
    Part pressed() const { return pressed_; }

    // A press at `y` at time `ms`. An arrow or the track moves at once. True when the position changed.
    bool press(int y, std::uint64_t ms);
    // The mouse at `y` while a part is pressed, `inside` the bar or not. A dragged thumb moves with
    // it, and a held arrow or track press repeats only while the mouse is on that part. True when
    // the bar changed.
    bool move(int y, bool inside);
    // The press ends at `y`. A dragged thumb takes its last place. True when the position changed.
    bool release(int y);
    // Called each frame: repeats a held arrow or track press. True when the position changed.
    bool tick(std::uint64_t ms);

private:
    bool step(Part part);
    bool setValue(float value);
    bool dragTo(int y);

    int height_ = 3 * kCell;
    int last_ = 0;
    int lineSteps_ = 0;  // how many line steps span the positions
    int pages_ = 1;
    float value_ = 0;
    Part pressed_ = Part::None;
    bool over_ = false;  // the mouse is still on the pressed part
    std::uint64_t pressedAt_ = 0;
};

} // namespace ts2::ui
