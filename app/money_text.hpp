#pragma once

#include "formats/tga.hpp"
#include "ui/screen.hpp"

#include <array>
#include <string>

#include <cstdint>

namespace app {

// `text` in the CostIndicator font as the game draws its cost texts: in `colour` with a one pixel black
// outline, ending 4 pixels in from the right of a picture at least 128 pixels wide.
ts2::tga::Pixels outlinedCostText(ts2::ui::Screen& screen, const std::string& text, const std::array<std::uint8_t, 3>& colour);

// The money text Buy mode shows when funds change ("+§200" for a sale), as the game draws it: the
// amount in the CostIndicator font, white with a one pixel black outline, its right edge 15 pixels
// left of the mouse and its top 15 pixels above it. Over 1.5 seconds it rises 200 pixels and fades
// out. A new change replaces the one shown.
class MoneyText {
public:
    // `change` is what the household paid, so a sale is negative and shows with a plus.
    void show(ts2::ui::Screen& screen, std::int32_t change, int mouseX, int mouseY);
    // Moves the text on by `seconds`.
    void update(ts2::ui::Screen& screen, double seconds);
    void hide(ts2::ui::Screen& screen);

private:
    void place(ts2::ui::Screen& screen);

    int window_ = -1;
    int mouseX_ = 0, mouseY_ = 0;
    float left_ = 0;   // milliseconds, 0 when nothing is shown
};

} // namespace app
