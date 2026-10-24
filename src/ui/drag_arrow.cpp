#include "ui/drag_arrow.hpp"

#include <algorithm>
#include <cmath>

namespace ts2::ui {

namespace {

// The game's rounding of its turn steps: halves go up.
std::int32_t roundHalfUp(double value) {
    return static_cast<std::int32_t>(std::floor(value + 0.5));
}

} // namespace

tga::Pixels turnDragArrow(std::span<const std::uint8_t> grey, float angle) {
    constexpr std::int32_t side = kDragArrowSide;
    tga::Pixels out;
    out.width = out.height = kDragArrowSide;
    out.rgba.assign(std::size_t{kDragArrowSide} * kDragArrowSide * 4, 0);
    if (grey.size() < std::size_t{kDragArrowSide} * kDragArrowSide) {
        return out;
    }
    // Steps through the picture, 16.16 fixed point, for one pixel across and one down. The walk
    // starts where turning about the middle (15.5, 15.5) puts the top left corner.
    const std::int32_t cosine = roundHalfUp(std::cos(angle) * 65536.0), sine = roundHalfUp(std::sin(angle) * 65536.0);
    std::int32_t rowU = (side << 15) - (((cosine - sine) * 31) >> 1) - 0x8000;
    std::int32_t rowV = (side << 15) - (((cosine + sine) * 31) >> 1) - 0x8000;
    // Sample points stop one pixel short of the right and bottom edges, so four pixels are always there.
    const std::int32_t last = (side << 8) - 0x101;
    for (std::int32_t y = 0; y < side; ++y) {
        std::int32_t u = rowU, v = rowV;
        for (std::int32_t x = 0; x < side; ++x) {
            const std::int32_t pu = u >> 8 < 0 ? 0 : std::min(u >> 8, last), pv = v >> 8 < 0 ? 0 : std::min(v >> 8, last);
            const std::size_t at = static_cast<std::size_t>((pv >> 8) * side + (pu >> 8));
            const std::int32_t fx = pu & 0xFF, fy = pv & 0xFF;
            const std::int32_t top = (grey[at] << 8) + (grey[at + 1] - grey[at]) * fx;
            const std::int32_t bottom = (grey[at + side] << 8) + (grey[at + side + 1] - grey[at + side]) * fx;
            const auto value = static_cast<std::uint8_t>((static_cast<std::uint32_t>((bottom - top) * fy + ((top + 0x80) << 8))) >> 16);
            if (value > 0x20) {
                auto* p = &out.rgba[(static_cast<std::size_t>(y) * side + static_cast<std::size_t>(x)) * 4];
                p[0] = p[1] = p[2] = value;
                p[3] = 255;
            }
            u += cosine;
            v += sine;
        }
        rowU -= sine;
        rowV += cosine;
    }
    return out;
}

} // namespace ts2::ui
