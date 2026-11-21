#pragma once

#include "content/lot.hpp"

#include <cstdint>
#include <vector>

namespace ts2::sim {

// A footprint rectangle in tiles on the game's axes (the lot's x is a position's y).
struct GameRect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// The placement test's wall check (code 8): whether a wall of the lot's room wall graph (instance 5,
// room dividers included) runs through a footprint. The graph keeps per tile which of its low x
// edge, low y edge and two diagonals have a wall. A footprint shrunk by 0.001 tiles covers a block of
// tiles, and the edges inside that block count, as do the diagonals of its tiles, except a diagonal of
// a corner tile when the footprint lies wholly in the quarter of that tile it misses.
class PlacementWalls {
public:
    explicit PlacementWalls(const content::Lot& lot);

    // Whether a wall crosses `rect` on `level` (0 = ground).
    [[nodiscard]] bool crossed(const GameRect& rect, std::int32_t level) const;

private:
    std::uint8_t bits(std::int32_t level, std::int32_t x, std::int32_t y) const;

    std::int32_t minX_ = 0, minY_ = 0, width_ = 0, height_ = 0, firstLevel_ = 0, levels_ = 0;
    // Per level and tile: 1 low x edge, 2 low y edge, 4 the diagonal from (x, y) to (x + 1, y + 1),
    // 8 the one from (x, y + 1) to (x + 1, y).
    std::vector<std::uint8_t> bits_;
};

} // namespace ts2::sim
