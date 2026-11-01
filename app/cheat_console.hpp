#pragma once

#include "platform/platform.hpp"
#include "sim/cheats.hpp"
#include "ui/scroll_bar.hpp"
#include "ui/screen.hpp"

#include <array>
#include <cstdint>
#include <cstddef>
#include <deque>
#include <string>
#include <vector>

namespace app {

// The game's cheat window, opened and closed with Ctrl+Shift+C ("Toggle Cheat Entry Window"). It
// has a line to type into and an output part, closed at first. Enter runs the line and writes it to
// the output after a ">". After an unknown command, bad arguments or help the output part opens.
// After any other line the window closes, unless the output part was open already. Escape closes
// the window. Up and Down step through the lines run before. A mouse press anywhere else leaves it
// open but takes the keyboard away from it, until a press in its text field. The output opens above
// the text field and scrolls with the mouse wheel and the game's scroll bar, drawn from the game's
// own scroll bar picture. The window is laid out as the game's. The game asks Windows for Lucida
// Sans Typewriter, and the output here uses Lucida Console when the system has it, which matches
// the game's character width.
class CheatConsole {
public:
    explicit CheatConsole(ts2::sim::CheatProperties& properties) : properties_(properties) {}

    void toggle(ts2::ui::Screen& screen, int width);
    void close(ts2::ui::Screen& screen);
    bool isOpen() const { return open_; }
    // Whether typing goes to the window. Then the camera's keys are left alone.
    bool takesKeys() const { return open_ && focused_; }
    // A mouse button pressed at (x, y) in UI units at time `ms`. The window stays open, and it takes
    // the keyboard only when the press is in its text field. A left press on the scroll bar works it.
    // True when the press is on the window's bar.
    bool mouseDown(ts2::ui::Screen& screen, int x, int y, bool left, std::uint64_t ms);
    // While the scroll bar holds the mouse it takes its moves and its release.
    bool holdsMouse() const { return scroll_.pressed() != ts2::ui::ScrollBar::Part::None; }
    void mouseMove(ts2::ui::Screen& screen, int x, int y);
    void mouseUp(ts2::ui::Screen& screen, int x, int y);
    // Called each frame: a held scroll arrow or track press repeats.
    void tick(ts2::ui::Screen& screen, std::uint64_t ms);
    // The mouse wheel at (x, y): over the open output it scrolls `lines` back (up) or on (down), and
    // is true.
    bool scroll(ts2::ui::Screen& screen, int x, int y, int lines);

    // Typing into the line and the keys the window uses while it is open. Enter runs the line on
    // `lot`, null outside a lot.
    void text(ts2::ui::Screen& screen, const std::string& typed);
    void key(ts2::ui::Screen& screen, ts2::platform::Key key, ts2::sim::CheatLot* lot);

private:
    void build(ts2::ui::Screen& screen, int width);
    void focus(ts2::ui::Screen& screen, bool on);
    // Sizes the panel and places the field for the output being open or not.
    void layout(ts2::ui::Screen& screen);
    void run(ts2::ui::Screen& screen, ts2::sim::CheatLot* lot);
    void showLine(ts2::ui::Screen& screen);
    void showOutput(ts2::ui::Screen& screen);
    void addOutput(std::string line);

    ts2::sim::CheatProperties& properties_;
    bool open_ = false;
    bool expanded_ = false;  // the output part is shown
    bool focused_ = false;   // typing goes to the text field
    std::array<int, 4> fieldArea_{};  // the text field: x, y, width, height
    // Whether (x, y) is on the scroll bar, and y from the bar's top.
    bool onScrollBar(int x, int y) const;
    int scrollY(int y) const;
    void showScrollBar(ts2::ui::Screen& screen);

    int bar_ = -1, edge_ = -1, lineText_ = -1, pane_ = -1;
    int scrollUp_ = -1, scrollTrack_ = -1, scrollDown_ = -1, scrollThumb_ = -1;
    ts2::ui::ScrollBar scroll_;  // its position is the output's top line
    std::vector<int> paneLines_;  // the output part's rows, top first
    int width_ = 0;
    std::string line_;
    std::size_t caret_ = 0;  // in bytes, between characters
    std::deque<std::string> output_;
    std::vector<std::string> history_;
    std::size_t historyAt_ = 0;  // the line shown from the history, history_.size() for a new one
};

} // namespace app
