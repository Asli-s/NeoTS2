#pragma once

#include "game_scroll_bar.hpp"
#include "ui/screen.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace app {

// The rows of one of the game's list boxes and its scroll bar. The rows are shown from the top row,
// one row height apart, as many as fit the list's height. The bar (see GameScrollBar) stands 3 past
// the rows at the list's right end, as tall as the rows shown, and the list grows to hold it. It
// shows only while the rows don't all fit. A wheel turn over the rows moves one row.
class ListBoxRows {
public:
    // The bar's pictures: image group, track and thumb.
    struct BarImages {
        std::uint32_t images = 0, track = 0, thumb = 0;
    };

    // How a list lays out its rows, when not one row height apart from the top (see attach).
    struct Layout {
        int rowHeight = 1;
        // With a gap, as many rows show as fit with the gap above each and below the last, spread
        // evenly over the list's height below the first gap, as the game's music list places them.
        int gap = 0;
        int rowX = 0, rowY = 0;  // where each row stands in its place
        // The bar's place in the list, its width the bar's own. Without one it stands 3 past the rows.
        std::optional<std::array<int, 3>> bar;  // x, y, height
    };

    // `list` is the list window. Its width and height are the rows' area.
    void attach(ts2::ui::Screen& screen, int list, int rowHeight, BarImages bar);
    void attach(ts2::ui::Screen& screen, int list, const Layout& layout, BarImages bar);
    // Takes `rows`, windows inside the list in the order shown, and shows them from the first.
    void setRows(std::vector<int> rows);
    // Scrolls so that `row` shows. A row below the ones shown becomes the last one shown.
    void showRow(int row);
    int width() const { return width_; }

    // The mouse, in UI pixels, at `ms` milliseconds. A press on the bar drags its thumb or moves a
    // page, and true means the press is the list's. Every move goes to mouseMove, for the thumb's look.
    bool mouseDown(int x, int y, std::uint64_t ms);
    void mouseMove(int x, int y);
    void mouseUp(int x, int y);
    bool holdsMouse() const { return scroll_.holdsMouse(); }
    // Called each frame: repeats a held press on the bar's track.
    void tick(std::uint64_t ms);
    // A wheel turn over the list scrolls a row, down for a `delta` below 0. True when it was over the list.
    bool wheel(int x, int y, int delta);

private:
    void showRows();
    int lastTop() const;
    // The rows follow the bar's value.
    void followBar();

    ts2::ui::Screen* screen_ = nullptr;
    int list_ = -1, bar_ = -1;
    GameScrollBar scroll_;
    int rowHeight_ = 1, width_ = 0, height_ = 0, visible_ = 0;
    int pitch_ = 1, firstY_ = 0, rowX_ = 0;  // rows' spacing, the first row's top and every row's left
    std::vector<int> rows_;
    int top_ = 0;
};

} // namespace app
