#pragma once

#include "formats/tga.hpp"
#include "ui/screen.hpp"

#include <cstdint>

namespace app {

// A bar the game fills in steps by a value from 0 to 1000 (a personality trait, an interest,
// fitness): its fill window shows part of the bar's picture, from the left or from the bottom.
class MeterBar {
public:
    MeterBar() = default;
    // `fill` is the bar's fill window (none: the bar shows nothing), `picture` the image in the
    // UI images group that fills it.
    MeterBar(ts2::ui::Screen& screen, int fill, std::uint32_t picture, bool vertical);

    // Shows `value` (0 to 1000) in `steps` steps.
    void show(int value, int steps);

private:
    ts2::ui::Screen* screen_ = nullptr;
    int fill_ = -1;
    int width_ = 0, height_ = 0;
    bool vertical_ = false;
    int pixels_ = -1;  // how much of it is shown
    ts2::tga::Pixels picture_;
};

} // namespace app
