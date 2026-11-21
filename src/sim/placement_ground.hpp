#pragma once

#include "content/lot.hpp"
#include "formats/array3d.hpp"
#include "formats/terrain.hpp"

#include <cstdint>
#include <functional>
#include <optional>

namespace ts2::sim {

// What the placement test's tile rules read of an object: object data 8 (flags), 0x2A (placement
// flags) and 0x52.
struct PlacedFlags {
    std::uint16_t flags = 0;
    std::uint16_t placement = 0;
    std::uint16_t flags3 = 0;
    int type = 0;  // OBJD field 9, the object type
};

// Whether an object that stands in for a floor (object data 0x62 bit 0) is on tile (x, y) of `level`,
// in the game's axes and levels (0 = ground).
using FloorStandIn = std::function<bool(std::int32_t level, std::int32_t x, std::int32_t y)>;

// The ground part of the game's object placement test for one object on one tile: whether the
// spot is on the lot, has a floor, is out of the water and pools, is level, and is floor or ground
// as the object allows. Positions are in tiles on the game's axes (the lot's x is a position's y),
// levels count from 0 for the ground storey.
//
// Not run yet: the test for objects that stand on a fence or half wall (placement flag 0x2000),
// the second pool rule for pool objects, the objects the low ceiling rule lets through (a list
// the game looks up by the object's resource, not known yet), and the rules for objects bigger than
// a tile. The roof rule is PlacementRoofs.
class PlacementGround {
public:
    explicit PlacementGround(const content::Lot& lot);

    // The game's placement code for the first rule the spot breaks, none when it passes them all.
    // 2 off the lot, 3 no such storey, 0x8F no floor, 0x91 in water, 0x90 in a pool, 0x12 not level,
    // 0x8D on a floor the object may not stand on, 0x8E on the ground it may not stand on, 0xA6
    // either for an object that needs a beach, 0x9F, 0xA0 and 0xA1 in the beach bands, 0x2F under a
    // ceiling lower than a storey.
    // `beachSide` is the lot's beach side on a beach lot (see content::isBeachLocation).
    [[nodiscard]] std::optional<std::int16_t> code(const PlacedFlags& object, float x, float y, std::int32_t level,
                                                   const FloorStandIn& standIn, std::optional<std::uint8_t> beachSide) const;

private:
    struct Tile {
        std::int32_t x = 0, y = 0, level = 0;
    };
    bool validTile(const Tile& t) const;
    bool validCorner(std::int32_t x, std::int32_t y, std::int32_t level) const;
    // The floor test over all four quarters (`all`) or any of them, needing the surface flag too
    // when `surface` is set and `all` is.
    bool floorTest(const Tile& t, bool all, bool surface) const;
    // The same, with the ground storey always counting as floored.
    bool solidFloor(const Tile& t, bool all, bool surface) const;
    // The tile's region, when all four quarters share one, else -1.
    std::int32_t wholeRegion(const Tile& t) const;
    bool anyQuarterRegion(const Tile& t, std::int32_t a, std::int32_t b) const;
    bool cornerUnderWater(std::int32_t x, std::int32_t y, std::int32_t level) const;
    bool flat(const Tile& t) const;
    bool lowCeiling(const Tile& t, bool needFloorAbove) const;
    bool beach(std::optional<std::uint8_t> side, float x, float y, std::int32_t level, int mask) const;

    std::optional<content::WorldBounds> bounds_;
    std::int32_t firstLevel_ = 0;  // the level of grid level 0
    std::optional<array3d::Array3D> floors_, surfaces_, heights_, regions_;
    std::optional<terrain::Array2D> water_;
};

} // namespace ts2::sim
