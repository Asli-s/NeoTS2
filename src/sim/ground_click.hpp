#pragma once

#include "sim/world.hpp"

#include <cstdint>

namespace ts2::sim {

class Vm;
class WallTiles;

// A click on empty floor or ground in live mode makes a "Destination" object at the clicked point
// and opens its pie menu (Go Here, Run Here, Watch Clouds...). Each click makes a new one. The
// object's own scripts remove it later.
namespace destination {
constexpr std::uint32_t kGoHere = 0x000007C4;  // "Destination - For Go Here & Shoo"
constexpr std::uint32_t kSwimHere = 0xECFEDB37;
constexpr std::uint32_t kFishHere = 0xB21ADA68;
constexpr std::uint32_t kBeachGoHere = 0x532FB5A1;
} // namespace destination

struct GroundPoint {
    float x = 0;  // tiles
    float y = 0;
    int level = 1;  // 1 = ground
};

// Moves a clicked point away from the walls of its tile: a point closer than 0.45 tiles to a wall
// (along an edge or across the tile) is pushed to just past 0.45 from it. The tile's walls are tested
// in a fixed order, each on the point as the earlier ones left it.
GroundPoint clearOfWalls(const WallTiles& walls, GroundPoint point);

// Makes the Go Here destination for `sim` at a clicked point and places it there, clear of walls.
// Null when there is no Sim to send or the point is off the lot.
SimObject* groundDestination(Vm& vm, World& world, const SimObject* sim, GroundPoint point);

} // namespace ts2::sim
