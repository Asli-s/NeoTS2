#pragma once

#include "ui/screen.hpp"

#include <array>

namespace app {

// The red frame round the screen with the pause sign in its top left corner, shown while the game
// is paused (UI script 0x8C364B93): the sign and three corners at the screen's corners, the four
// edges stretched between them.
class PauseFrame {
public:
    void attach(ts2::ui::Screen& screen, int width, int height);
    void show(bool shown);

private:
    ts2::ui::Screen* screen_ = nullptr;
    std::array<int, 8> pieces_{-1, -1, -1, -1, -1, -1, -1, -1};
    bool shown_ = false;
};

} // namespace app
