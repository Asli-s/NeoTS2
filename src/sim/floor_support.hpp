#pragma once

#include "content/lot.hpp"
#include "sim/rooms.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace ts2::sim {

// A wall or fence style as the floor support test needs it: walls.txt's bearsLoad, or a fence's
// bearsload property, and whether it is a fence.
struct PartitionSupport {
    bool bearsLoad = false;
    bool fence = false;
};

// Looks up a wall layer type: a wall style number, or a fence's GUID. Nothing for an unknown type.
using PartitionLookup = std::function<std::optional<PartitionSupport>(std::uint32_t type)>;
// Whether an object of this definition GUID holds up a floor above it: its OBJD field 0x4A (the
// Build subsort) is 8, as columns have.
using ColumnLookup = std::function<bool(std::uint32_t guid)>;

// Where Build mode lets a floor go on the storeys above the ground: a level tile inside the lot's
// edge that is held up from below (a room of wall graph 6, a column, a load-bearing wall or fence
// post), or one beside a tile held up by a room or column. Beside a floor, a tile two or three steps
// from such support, or within one tile of a load-bearing wall, counts too. Platform pieces below
// don't count yet.
class FloorSupport {
public:
    FloorSupport(const content::Lot& lot, const PartitionLookup& partitions, const ColumnLookup& columns);

    // Whether a floor may go on tile (x, y) of grid level `level` (grid levels count from the lot's
    // lowest, as its grids do). The ground storey and below always allow one.
    bool allowed(std::int32_t level, std::int32_t x, std::int32_t y) const;

private:
    bool inBounds(std::int32_t level, std::int32_t x, std::int32_t y) const;
    bool insideEdge(std::int32_t x, std::int32_t y) const;
    bool level(std::int32_t gridLevel, std::int32_t x, std::int32_t y) const;
    // Whether any of the quarters (bits in the room map's order) of the tile below lies in a region, or
    // a column stands on that tile.
    bool regionBelow(std::int32_t gridLevel, std::int32_t x, std::int32_t y, int quarters) const;
    bool wallsBelow(std::int32_t gridLevel, std::int32_t x, std::int32_t y) const;
    bool floored(std::int32_t gridLevel, std::int32_t x, std::int32_t y, int quarter) const;

    const content::Lot& lot_;
    std::int32_t ground_ = 0;  // the ground storey's grid level
    std::int32_t width_ = 0, height_ = 0, levels_ = 0;
    std::optional<content::WorldBounds> bounds_;
    std::optional<RoomMap> regions_;
    // Per grid level and tile, bit 0: a load-bearing wall on the tile's low-y edge, bit 1: on its low-x
    // edge, bit 2: on its rising diagonal, bit 3: on its falling diagonal.
    std::vector<std::uint8_t> walls_;
    // Per grid level and tile corner (one more each way than tiles): a load-bearing fence post there.
    std::vector<std::uint8_t> posts_;
    std::vector<std::uint8_t> columns_;  // per grid level and tile: a column stands there
};

} // namespace ts2::sim
