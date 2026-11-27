#pragma once

#include "content/floor_edit.hpp"
#include "content/lot.hpp"
#include "scene/assets.hpp"
#include "sim/floor_support.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace app {

// A quarter of a floor tile on a grid level. Quarters run 0 low-x edge, 1 high-y, 2 high-x, 3 low-y.
struct FloorSpot {
    std::int32_t level = 0, x = 0, y = 0;
    int quarter = 0;
    bool operator==(const FloorSpot&) const = default;
};

// The quarter under a point, in the floor grid's x and y: the triangle whose tile edge is nearest.
FloorSpot floorSpotAt(float x, float y, std::int32_t level);

ts2::sim::FloorSupport floorSupport(const ts2::content::Lot& lot, ts2::scene::Assets& assets);

// Whether the quarter has a surface a floor can go on. Ground over a pool or a hot tub's basin has none.
bool floorSurfaceAt(const ts2::content::Lot& lot, const FloorSpot& s);

// Whether Shift fills from `from`: inside a room, or on floor outside one. Elsewhere the game refuses
// with placement error 0x58.
bool floorFillable(const ts2::content::Lot& lot, const FloorSpot& from);

enum class FloorStroke { Lay, Remove };

// The quarters a stroke from `from` to `to` changes, with `pattern` (empty takes floors up): the quarter
// at `to`, the room at `from` (Shift), or the rectangle's tiles, half a tile where a diagonal wall
// crosses a single one (`diagonal` 1 from its low corner, 2 the other way). Quarters that would not
// change, and empty ones `allowed` refuses, are left out.
struct FloorStrokeShape {
    bool quarters = false, room = false;
};
std::vector<ts2::content::FloorQuarter> floorStroke(const ts2::content::Lot& lot, const FloorSpot& from, const FloorSpot& to,
                                                     const std::string& pattern, FloorStrokeShape shape,
                                                     const std::function<int(std::int32_t level, std::int32_t x, std::int32_t y)>& diagonal,
                                                     const std::function<bool(std::int32_t level, std::int32_t x, std::int32_t y)>& allowed);

// A stroke taking floors up puts a deck's, foundation's, pool's or stage's own floor back over one, as
// the game does. Quarters that would not change are left out.
std::vector<ts2::content::FloorQuarter> keepPlatformFloors(const ts2::content::Lot& lot, std::vector<ts2::content::FloorQuarter> stroke,
                                                            const std::map<std::int32_t, std::string>& platformFloors);

// Whether a deck, foundation, pool or stage is under the floor of any quarter of the tile.
bool platformTile(const ts2::content::Lot& lot, std::int32_t level, std::int32_t x, std::int32_t y);

// What a stroke costs, negative for what it gives back. A quarter costs a quarter of its pattern's price,
// and laying over a floor gives nothing for it. Taking up gives back four fifths of the old price, rounded
// down, then a quarter of that, even where a platform's floor goes back. Unknown patterns are free.
float floorStrokeCost(const ts2::content::Lot& lot, const std::vector<ts2::content::FloorQuarter>& stroke, FloorStroke kind,
                      const std::function<std::optional<int>(const std::string& pattern)>& price);

} // namespace app
