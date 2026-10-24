#pragma once

#include "formats/tga.hpp"
#include "ui/screen.hpp"

#include <array>
#include <cstdint>

namespace app {

// The live panel's mood meter, beside the small buttons: the selected Sim's mood, painted from the
// meter's own gradient, and full with its platinum glow while the Sim's aspiration is platinum.
// The panel has one for wants closed and one for wants open. Both show the same.
class MoodMeter {
public:
    // `panel` is the loaded live panel script's root window.
    void attach(ts2::ui::Screen& screen, int panel);

    // Shows a mood (-100..100) and an aspiration score (person data 0xA0).
    void show(int mood, int aspirationScore);

private:
    struct Meter {
        int fill = -1;                 // the meter's fill window
        ts2::tga::Pixels picture;      // the fill's own picture (its shape)
        std::array<int, 2> glows{-1, -1};  // the platinum glows
        int column = -1;               // the gradient column it shows
    };

    ts2::ui::Screen* screen_ = nullptr;
    std::array<Meter, 2> meters_;
    ts2::tga::Pixels gradient_;
    int platinum_ = -1;  // whether the glows show (-1: not set yet)
};

} // namespace app
