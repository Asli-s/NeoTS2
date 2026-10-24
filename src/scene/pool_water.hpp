#pragma once

#include "content/lot.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace ts2::scene::poolwater {

// A quarter of a tile holding water: the tile, and the quarter by its edge 0 x low, 1 y high,
// 2 x high, 3 y low (the grids' quarter order).
struct Quarter {
    int x = 0, y = 0, quarter = 0;
};

// The ground storey's quarters with water: open ground (the surface grid) over a floor on the level
// below it, as a pool is built.
std::vector<Quarter> waterQuarters(const content::Lot& lot);

// One of the see-through sheets under the water's surface that darken deeper water.
struct Layer {
    std::vector<float> positions;  // x, y, z per vertex
    std::vector<float> alphas;     // per vertex, times `opacity`
    std::vector<std::uint32_t> indices;
    float opacity = 0;
};

// The game's three depth layers: per quarter the triangle between its outer tile edge and the tile's
// centre, 0.75, 1.5 and 2.25 below the water's surface. Clear on the pool's edge, faint beside it.
// `height(x, y)` is the ground's height at a tile corner or centre.
std::array<Layer, 3> depthLayers(const std::vector<Quarter>& quarters, const std::function<float(float x, float y)>& height);

} // namespace ts2::scene::poolwater
