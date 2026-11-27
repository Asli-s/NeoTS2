#pragma once

#include "ui/screen.hpp"

#include <cstdint>
#include <string>

namespace app {

// The price and size a build tool shows beside the mouse while dragging, as the game draws them in
// the CostIndicator font with a black outline. The price ("-§280", "+§56" for money back) is yellow at
// 150 of 255 alpha, red and opaque when the household can't pay, its right edge 15 pixels left of the
// mouse and its top 15 pixels above it. The size ("4 x 0") is white, right below at 5 pixels under
// the mouse.
class CostLabels {
public:
    // `cost` is what the household would pay. An empty `size` shows the price alone.
    void show(ts2::ui::Screen& screen, std::int32_t cost, bool cantAfford, const std::string& size, int mouseX, int mouseY);
    void hide(ts2::ui::Screen& screen);

private:
    // Puts `text` in `window` (made the first time) with its right edge 15 pixels left of the mouse
    // and its top at `top`.
    void place(ts2::ui::Screen& screen, int& window, const std::string& text, const std::array<std::uint8_t, 3>& colour, float alpha,
               int mouseX, int top);

    int price_ = -1, size_ = -1;
    std::string priceText_, sizeText_;  // what each window shows, so an unchanged one isn't drawn again
    bool cantAfford_ = false;
};

} // namespace app
