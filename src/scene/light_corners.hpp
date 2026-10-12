#pragma once

#include "scene/floor_light.hpp"
#include "scene/wall_light.hpp"

#include <cstdint>
#include <vector>

namespace ts2::scene::lightcorners {

// The game's last touch to indoor light: where a lamp-lit wall meets an unlit one at a right
// angle, the light near the corner is pulled toward the ambient, on the walls and on the floor.

// A right-angle corner between two wall runs: the tile of the first run's last segment, and which
// way the corner points (0..7, as the floor light's directions).
struct Corner {
    int x = 0, y = 0, level = 0;
    std::uint8_t octant = 0;
    bool single = false;  // either run is one segment long
};

// Blends the last texel columns of lamp-lit runs toward their ambient where they turn a right
// angle into a run that is not lamp-lit (runs need `texels` filled by walllight::pad).
void blendWalls(walllight::Walls& walls);

// The corners between runs where not both are lamp-lit, each pair once. `keys` gives each run's
// segments' tiles (x, y, level), as wallruns::Layout::keys.
struct Tile {
    int x = 0, y = 0, level = 0;
};
std::vector<Corner> corners(const walllight::Walls& walls, const std::vector<std::vector<Tile>>& tiles);

// Blends the floor cells inside each corner on the grid's level toward the floor ambient.
void blendFloor(floorlight::Grid& grid, const std::vector<Corner>& corners);

} // namespace ts2::scene::lightcorners
