#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

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

// A tip's text taken apart as the game takes it. Fields are separated by '|': with exactly five
// separators they are title, body, extra, picture, picture and mode, otherwise title, body,
// picture, picture and mode. A field that isn't there is empty (a picture 0). Text without '|' is
// all title. The mode field "L" asks for the large tip and a hexadecimal number names a UI script
// shown as a popup. Anything else is the drawn box.
struct TipText {
    enum class Mode { Box, Large, Popup };
    std::string title, body, extra;
    std::uint32_t picture1 = 0, picture2 = 0;
    Mode mode = Mode::Box;
    std::uint32_t popup = 0;  // the popup's UI script
};
TipText splitTip(std::string_view text);

// Reads a hexadecimal number as the game's "%x" does: spaces, then an optional 0x, then digits
// until the first that isn't one. Empty when no digit comes.
std::optional<std::uint32_t> hexNumber(std::string_view s);

} // namespace ts2::ui
