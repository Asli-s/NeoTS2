#pragma once

#include "formats/tga.hpp"
#include "ui/screen.hpp"

#include <array>
#include <cstdint>

namespace app {

// The live panel's bars between the portrait and the wants: the aspiration bar (the aspiration
// score, -100..100) in front of the lifetime aspiration bar (its happiness, 0..30000) in front of
// the influence bar (0..100, in one of five pictures by influence level), each painted from its
// own gradient, the first two with their platinum pictures.
class AspirationBars {
public:
    // `panel` is the loaded live panel script's root window.
    void attach(ts2::ui::Screen& screen, int panel);

    // Shows an aspiration score (person data 0xA0), a lifetime aspiration happiness (0xD8) and
    // the influence bar's value at an influence level (0..4).
    void show(int aspirationScore, int lifetimeHappiness, int influence, int influenceLevel);

private:
    struct Bar {
        int fill = -1;
        ts2::tga::Pixels picture, gradient;
        int platinum = -1;
        int column = -1;
        int platinumShown = -1;
    };
    void paint(Bar& bar, int value, int min, int max, bool platinum);

    ts2::ui::Screen* screen_ = nullptr;
    Bar aspiration_, lifetime_;
    std::array<Bar, 5> influence_;
    std::array<int, 5> capacities_{-1, -1, -1, -1, -1};
    int influenceLevel_ = -1;
};

} // namespace app
