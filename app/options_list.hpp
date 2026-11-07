#pragma once

#include "ui/screen.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace app {

// Draws an empty scroll bar window with its track and thumb at the top. Returns the thumb.
int addScrollBarParts(ts2::ui::Screen& screen, int bar);

// A list box of the options panels: rows with a radio dot, one of them picked, and a scroll bar
// inside the list's right end that shows when the rows don't all fit. The script holds only an empty
// window for it, and the game builds the rest in code.
class OptionsList {
public:
    // `list` is the script's empty window. Its width and height are the rows' area.
    void attach(ts2::ui::Screen& screen, int list);
    void setRows(const std::vector<std::string>& rows);
    // Picks a row, -1 for none. A row below the ones shown scrolls up to be the last one shown.
    void pick(int row);
    int picked() const { return picked_; }
    void setEnabled(bool enabled);

    // A click on one of its rows picks it. True when the window is one of them.
    bool clicked(int window);

    // The mouse, in UI pixels. A press on the bar drags its thumb or moves a page, and true means the
    // press is the list's.
    bool mouseDown(int x, int y);
    void mouseMove(int y);
    void mouseUp() { dragging_ = false; }
    bool holdsMouse() const { return dragging_; }
    // Scrolls by `rows` when the mouse is over the list. True when it is.
    bool wheel(int x, int y, int rows);

private:
    void showRows();
    int lastTop() const;
    int barY() const;
    int thumbTravel() const;

    ts2::ui::Screen* screen_ = nullptr;
    int list_ = -1, bar_ = -1, thumb_ = -1;
    int width_ = 0, height_ = 0, visible_ = 0;
    std::vector<int> rows_;
    int picked_ = -1, top_ = 0;
    bool enabled_ = true, dragging_ = false;
};

} // namespace app
