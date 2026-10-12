#pragma once

#include "scene/floor_light.hpp"
#include "scene/wall_runs.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace ts2::scene::lightgrid {

// Builds one level's floor light grid (floorlight::Grid, without light) the way the game's
// lighting manager does: which half tiles have floor, where walls cut the links light spreads
// along, which tiles a diagonal wall splits, and which room each part belongs to.

// The sides of a tile a wall can stand on.
enum Side : std::uint8_t {
    kMinusY = 0x01,
    kMinusX = 0x02,
    kPlusY = 0x04,
    kPlusX = 0x08,
    kDiagonalXPlusY = 0x10,  // across the tile along x + y = constant
    kDiagonalXEqualsY = 0x20,  // across the tile along x = y
};

struct LevelInput {
    int level = 0;
    int width = 0, height = 0;           // in tiles
    std::vector<std::uint8_t> floor;      // per tile, row by row: non-zero where there is floor
    std::vector<std::uint8_t> walls;      // per tile: the Side bits of walls that stop light
    // The room at a point (in tiles) on this level, as the lighting sees rooms; 0 is outside.
    std::function<std::int16_t(float x, float y)> roomAt;
};

// The grid for a level; empty (not fitting) when the input's sizes are out of range or don't match.
floorlight::Grid build(const LevelInput& in);

// A level's floor and walls from a lot, as the game reads them for lighting: a tile counts as
// floor when it or the tile above it has floor in any quarter (every ground tile does); `walls`
// are the lot's light-stopping walls (wallruns::wallsOf), of any level. roomAt is left for the
// caller to fill.
LevelInput fromLot(const content::Lot& lot, int level, const std::vector<wallruns::Wall>& walls);

} // namespace ts2::scene::lightgrid
