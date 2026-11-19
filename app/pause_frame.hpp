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
    // Lays the frame out for a screen of this size.
    void layout(int width, int height);
    void show(bool shown);
    // The pause sign is a button that resumes the game, and only in live mode.
    void setEnabled(bool enabled);
    // Whether `window` is the pause sign.
    bool clicked(int window) const { return window >= 0 && window == pieces_[0]; }

private:
    ts2::ui::Screen* screen_ = nullptr;
    std::array<int, 8> pieces_{-1, -1, -1, -1, -1, -1, -1, -1};
    bool shown_ = false;
};

} // namespace app
