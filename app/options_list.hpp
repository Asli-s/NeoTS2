#pragma once

#include "list_box_rows.hpp"
#include "ui/screen.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace app {

// A list box of the options panels: rows with a radio dot, one of them picked, and the game's list
// box scroll bar (see ListBoxRows). The script holds only an empty window for the list, and the
// game builds the rest in code.
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

    // The mouse and the wheel on the list's bar and rows (see ListBoxRows).
    bool mouseDown(int x, int y, std::uint64_t ms) { return box_.mouseDown(x, y, ms); }
    void mouseMove(int x, int y) { box_.mouseMove(x, y); }
    void mouseUp(int x, int y) { box_.mouseUp(x, y); }
    bool holdsMouse() const { return box_.holdsMouse(); }
    void tick(std::uint64_t ms) { box_.tick(ms); }
    bool wheel(int x, int y, int delta) { return box_.wheel(x, y, delta); }

private:
    void showChecks();

    ts2::ui::Screen* screen_ = nullptr;
    int list_ = -1;
    ListBoxRows box_;
    std::vector<int> rows_;
    int picked_ = -1;
    bool enabled_ = true;
};

} // namespace app
