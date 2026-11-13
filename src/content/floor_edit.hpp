#pragma once

#include "formats/array3d.hpp"
#include "formats/stringmap.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace ts2::content {

// A rectangle of tiles on a lot's floor grid, both corners included, in either order.
struct TileRect {
    std::int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    std::int32_t width() const { return (x1 > x0 ? x1 - x0 : x0 - x1) + 1; }
    std::int32_t height() const { return (y1 > y0 ? y1 - y0 : y0 - y1) + 1; }
};

// What laying or taking up floor changed: how many quarter tiles, and the patterns they had before
// (by name, as the lot's pattern list names them), each with its count of quarters.
struct FloorChange {
    std::size_t quarters = 0;
    std::map<std::string, std::size_t> replaced;
};

// The id a lot's floor grid stores for a floor pattern (named as the lot's pattern list names its
// catalog floors: the GUID in hex), added to the list with the lowest free id when it isn't there.
// 0 when every id is taken.
std::uint16_t floorPatternId(std::map<std::uint16_t, stringmap::Entry>& patterns, const std::string& name);

// The name a lot's pattern list gives a catalog floor: its GUID in lowercase hex.
std::string floorPatternName(std::uint32_t guid);

// Lays floor pattern `name` over the rectangle on grid level `level` of the floor grid (kFloorGridInstance:
// four 16-bit pattern ids per tile). With `anywhere`, every quarter of each tile gets it (as on the
// ground storey) but those the surface grid (kSurfaceGridInstance, when given) marks as having no
// surface, such as over a pool; otherwise only quarters that have floor already, so the storey's
// shape stays as it is. Quarters already of that pattern are left. The pattern list's use counts
// follow.
FloorChange layFloor(array3d::Array3D& floors, std::map<std::uint16_t, stringmap::Entry>& patterns, std::int32_t level, TileRect rect,
                     const std::string& name, bool anywhere, const array3d::Array3D* surface = nullptr);

// Takes up the floor in the rectangle on grid level `level`, leaving its quarters bare.
FloorChange removeFloor(array3d::Array3D& floors, std::map<std::uint16_t, stringmap::Entry>& patterns, std::int32_t level, TileRect rect);

// The price of `quarters` quarter tiles of a floor that costs `pricePerTile` a tile, a part tile
// rounded up.
std::int32_t floorPrice(std::int32_t pricePerTile, std::size_t quarters);

} // namespace ts2::content
