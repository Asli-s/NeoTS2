#pragma once

#include "meter_bar.hpp"
#include "ui/screen.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace app {

// Simology's hobbies page: each hobby's name over a bar of the Sim's enthusiasm for it (person data
// 0xCC to 0xD5), two in a row. The Sim's favourite hobby glows and a hobby whose Secret Lot the Sim
// has found shows its mark.
class HobbiesPage {
public:
    // `page` is the Simology script's hobbies window.
    void attach(ts2::ui::Screen& screen, int page);

    // Shows the "maximized" art (wants open) or the "minimized" one.
    void setMaximized(bool maximized);

    // Shows a Sim's person data and the hobbies whose Secret Lot it has found (by person data
    // index).
    void show(std::span<const std::int16_t> personData, std::span<const std::int16_t> secretLots);

private:
    struct Item {
        int window = -1;
        MeterBar bar;
        int value = -1;
        int glow = -1, secretLot = -1;  // shown or not, -1 before the first show
    };

    ts2::ui::Screen* screen_ = nullptr;
    int page_ = -1;
    std::vector<Item> items_;
};

} // namespace app
