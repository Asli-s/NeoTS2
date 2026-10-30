#pragma once

#include <array>

namespace ts2::ui {

// Where the game puts a hover tip's box: anchored on the centre of the button, kept inside the
// screen less 16 pixels on each side. It goes above the button when there is room, else below, else
// to a side. The box includes the frame's shadow (2 pixels right, 3 below).
struct TipBox {
    int x = 0, y = 0, w = 0, h = 0;
};
// `w` and `h` are the box's size: the text's width and height plus 24 and 12.
// `screen` is the screen's width and height (0 for no limits).
TipBox placeTip(int anchorX, int anchorY, int w, int h, std::array<int, 2> screen);
// The same with the bounds the box stays inside (left, top, right, bottom) and the gap between the
// anchor and the box given. A tip over a Sim in the world keeps 28 pixels from its anchor.
TipBox placeTip(int anchorX, int anchorY, int w, int h, std::array<int, 4> bounds, int gap);

} // namespace ts2::ui
