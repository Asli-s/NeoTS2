#pragma once

#include "formats/tga.hpp"

#include <array>

namespace ts2::ui {

// The balloon the game shows over a spot of the lot for a short message, as Buy mode's "This object
// is not designable." Its box and arrow are cut from one UI picture, a 5 by 5 grid of cells: the
// box in the top left 4 by 4, the up and down arrows in the last column and the left and right
// arrows in the last row.
struct WorldTipLayout {
    // Where the box sits from its spot: 0 left (arrow pointing right), 1 above (pointing down),
    // 2 right (pointing left), 3 below (pointing up).
    int side = 1;
    // Rects as left, top, right, bottom in screen pixels. The window holds the box and the arrow.
    std::array<int, 4> window{}, box{}, arrow{};
    // The text's top left inside the window.
    int textX = 0, textY = 0;
};

// Lays the balloon out for text `textW` by `textH` pixels pointing at (anchorX, anchorY). The box
// stays inside `bounds` (left, top, right, bottom). `cell` is the picture's cell size.
WorldTipLayout layoutWorldTip(int anchorX, int anchorY, int textW, int textH, std::array<int, 4> bounds, int cell);

// The balloon's box and arrow drawn from the cell picture `sheet`, the window's size.
tga::Pixels drawWorldTip(const tga::Pixels& sheet, const WorldTipLayout& layout);

} // namespace ts2::ui
