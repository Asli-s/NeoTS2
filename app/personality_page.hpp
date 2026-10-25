#pragma once

#include "meter_bar.hpp"
#include "ui/screen.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace app {

// Simology's personality page: the five trait bars (each with a thin line for the trait the Sim was
// born with), the fitness bar with its body icon, and the zodiac sign.
class PersonalityPage {
public:
    // `page` is the Simology script's personality window.
    void attach(ts2::ui::Screen& screen, int page);

    // Shows a Sim's person data.
    void show(std::span<const std::int16_t> personData);

private:
    ts2::ui::Screen* screen_ = nullptr;
    int page_ = -1;
    std::array<MeterBar, 5> traits_, born_;
    MeterBar fitness_;
    int zodiac_ = -1;  // the sign shown
};

} // namespace app
