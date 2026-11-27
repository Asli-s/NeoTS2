#pragma once

#include "content/lot.hpp"

#include <array>
#include <cstdint>
#include <optional>
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
    [[nodiscard]] bool crossed(const GameRect& rect, std::int32_t level) const { return crossing(rect, level).has_value(); }
    // The middle of the first wall crossing `rect`, where the game's message points.
    [[nodiscard]] std::optional<std::array<float, 2>> crossing(const GameRect& rect, std::int32_t level) const;

    // The placement rule for an object's sides (object data 0x0D). Bits 0x01 to 0x40 need a wall,
    // 0x100 to 0x800 forbid one, and a diagonal forbids one unless 0x20 or 0x80 allows it. A wall or
    // fence on a forbidden side is code 8. A needed wall missing, or only a fence or half wall, is code
    // 7, 0x3B on a tile with a diagonal wall, or 1 for `standsInFor` objects. Objects with bit 0x1000
    // (they stand on fences) aren't tested yet.
    struct SideRefusal {
        std::int16_t code = 0;
        std::array<float, 2> at{};
    };
    [[nodiscard]] std::optional<SideRefusal> sides(std::uint16_t sides, std::int32_t x, std::int32_t y, std::int32_t level,
                                                   std::int16_t direction, bool standsInFor) const;

    // The tile edges of the sides that need a wall, each as (x0, y0, x1, y1).
    [[nodiscard]] static std::vector<std::array<std::int32_t, 4>> neededEdges(std::uint16_t sides, std::int32_t x, std::int32_t y,
                                                                              std::int16_t direction);

private:
    std::uint8_t bits(std::int32_t level, std::int32_t x, std::int32_t y) const { return at(bits_, level, x, y); }
    std::uint8_t at(const std::vector<std::uint8_t>& grid, std::int32_t level, std::int32_t x, std::int32_t y) const;

    std::int32_t minX_ = 0, minY_ = 0, width_ = 0, height_ = 0, firstLevel_ = 0, levels_ = 0;
    // Per level and tile: 1 low x edge, 2 low y edge, 4 the diagonal from (x, y) to (x + 1, y + 1),
    // 8 the one from (x, y + 1) to (x + 1, y). The same for the segments with a wall or fence on them
    // (room dividers have none), and for those with a wall rather than a fence or half wall.
    std::vector<std::uint8_t> bits_, built_, solid_;
};

} // namespace ts2::sim
