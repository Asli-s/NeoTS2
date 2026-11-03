#pragma once

#include "content/lot.hpp"
#include "scene/scene.hpp"

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
// `height(x, y)` is the water's height at a tile corner or centre (see surfaceHeight).
std::array<Layer, 3> depthLayers(const std::vector<Quarter>& quarters, const std::function<float(float x, float y)>& height);

// The water's surface lies 2.75 above the level below the ground storey, a quarter of a tile under
// the ground where storeys are 3 apart.
float surfaceHeight(float levelBelow);

// The water's surface, as the game's pool water material waves it: per quarter 8 triangles, its
// outer edge split in four and its inner edges in two. The caller gives it normals straight up.
struct Surface {
    std::vector<float> positions;  // x, y, z per vertex
    std::vector<std::uint32_t> indices;
};
Surface surface(const std::vector<Quarter>& quarters, const std::function<float(float x, float y)>& height);

// Scales the pools' surface highlights for the time of day, as the pool water material does: 0.75
// by day, 0.2 at night. Returns the instances changed.
std::vector<std::size_t> lightPoolWater(Scene& scene, bool day);

} // namespace ts2::scene::poolwater
