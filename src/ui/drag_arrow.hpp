#pragma once

#include "formats/tga.hpp"

#include <cstdint>
#include <span>

namespace ts2::ui {

// The arrow cursor shown while the camera is dragged: a 32 x 32 grey picture (one byte a pixel,
// rows from the top) turned by `angle` radians about its middle, as the game turns it each frame.
// Each pixel is the picture sampled between its four nearest pixels. One of 32 or darker is left
// transparent, and the rest are shown in that grey. The angle is the drag's direction on screen, from
// the press point to the mouse, with y up.
constexpr std::uint32_t kDragArrowSide = 32;
[[nodiscard]] tga::Pixels turnDragArrow(std::span<const std::uint8_t> grey, float angle);

} // namespace ts2::ui
