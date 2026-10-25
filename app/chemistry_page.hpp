#pragma once

#include "ui/screen.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace app {

// Simology's chemistry page: the Sim's first two turn-ons and first turn-off as icons, each with
// its name as the tip.
class ChemistryPage {
public:
    // `page` is the Simology script's chemistry window.
    void attach(ts2::ui::Screen& screen, int page);

    // Shows a Sim's person data.
    void show(std::span<const std::int16_t> personData);

private:
    // The turn-on or turn-off a slot shows (0: none), as its place in the game's list counted from 1.
    void showSlot(std::size_t slot, int turnOn);

    ts2::ui::Screen* screen_ = nullptr;
    int page_ = -1;
    std::array<int, 3> shown_{-1, -1, -1};
};

} // namespace app
