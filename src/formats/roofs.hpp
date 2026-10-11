#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace ts2::roofs {

// A roof a player built: the rectangle between two corners on the wall graph's grid, at a level
// (as the lot's height grids count them), its type and its pattern's GUID in the build catalog.
struct Roof {
    std::uint32_t id = 0;
    float x0 = 0, y0 = 0;
    std::int32_t level0 = 0;
    float x1 = 0, y1 = 0;
    std::int32_t level1 = 0;
    std::uint32_t type = 0;
    std::uint32_t pattern = 0;
    // From version 2: the roof's own slope, stored as half the tangent of its angle (0.5 at
    // 45 degrees); 0 when the lot's global slope applies.
    float slope = 0;
    std::uint32_t extra = 0;
};

// The lot's roofs (resource type 0xAB9406AA, versions 1 to 3).
std::vector<Roof> parse(std::span<const std::uint8_t> data);

} // namespace ts2::roofs
