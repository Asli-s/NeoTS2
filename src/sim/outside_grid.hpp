#pragma once

#include "content/lot.hpp"
#include "sim/rooms.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace ts2::sim {

// Where rain and snow reach on a lot, as the game works it out when a lot opens: one byte per tile
// and storey (0 the ground storey), a bit per quarter open to the sky (1 low x, 2 high y, 4 high x,
// 8 low y), 0x10 for a tile covered by a roof or a floor above.
class OutsideGrid {
public:
    OutsideGrid() = default;
    // Built from the lot's floors, heights and roofs. Only tiles outside every region of the second
    // room layer (`regions`) are worked out. The others stay 0.
    OutsideGrid(const content::Lot& lot, const RoomMap& regions);

    // Whether the quarter of the tile holding (x, y) on `level` is open to the sky, nullopt off the grid.
    [[nodiscard]] std::optional<bool> open(std::int32_t level, float x, float y) const;
    // The tile's byte, 0 off the grid.
    std::uint8_t tile(std::int32_t level, std::int32_t x, std::int32_t y) const;
    // Whether quarter `quarter` of the tile on `level` is pool water (its region kind 3 or 5).
    bool pool(std::int32_t level, std::int32_t x, std::int32_t y, int quarter) const;
    std::int32_t width() const { return width_; }
    std::int32_t height() const { return height_; }
    std::int32_t levels() const { return levels_; }

private:
    std::int32_t width_ = 0, height_ = 0, levels_ = 0;
    std::vector<std::uint8_t> bytes_;
    std::vector<std::uint8_t> pools_;  // per tile, a bit per quarter
};

} // namespace ts2::sim
