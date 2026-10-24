#pragma once

#include "content/ui_meters.hpp"
#include "formats/tga.hpp"
#include "ui/screen.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

namespace app {

// The live panel's needs: one bar per motive, laid out for the Sim's life stage (toddlers and
// children have fewer needs). Each bar is coloured from the game's bar gradient by the motive's
// value. A bar is repainted only when its colour row changes. Arrows beside a bar show how fast
// its need is rising or falling (see NeedTrend).
class NeedsPanel {
public:
    // `content` is the loaded needs script's root window.
    void attach(ts2::ui::Screen& screen, int content);

    // Shows a Sim's needs: its life stage (person data 0x3A) and its motives as whole numbers.
    // `open` is whether the needs panel is showing: the arrows only follow the needs while it is.
    void show(std::int16_t sim, std::int16_t lifeStage, const std::array<int, 16>& motives, bool open);

private:
    struct Bar {
        std::size_t motive = 0;
        int fill = -1;  // the bar's fill window
        int row = -1;   // the gradient row it shows
        int window = -1;  // the bar script's window, which holds the arrows
        ts2::content::NeedTrend trend;
    };
    struct Layout {
        int group = -1;
        std::vector<Bar> bars;
    };

    Layout build(std::uint32_t group, const std::vector<std::pair<std::uint32_t, std::size_t>>& slots);
    // Shows `arrows` arrows beside the bar: falling ones when negative, rising ones when positive.
    void showArrows(const Bar& bar, int arrows);

    ts2::ui::Screen* screen_ = nullptr;
    int content_ = -1;
    std::array<Layout, 3> layouts_;  // toddler, child, everyone else
    ts2::tga::Pixels fill_, gradient_;
    std::int16_t sim_ = 0;  // whose needs the arrows follow
    std::chrono::steady_clock::time_point lastSample_{};
};

} // namespace app
