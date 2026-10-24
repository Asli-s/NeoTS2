#pragma once

#include "content/lot.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <tuple>

namespace ts2::sim {

class World;
struct SimObject;

// The walls on each tile's sides and across it, by level (0 = the ground storey), in route
// coordinates (the game's positions: the wall graph's x and y swapped).
class WallTiles {
public:
    enum Side : std::uint8_t {
        kLowY = 0x01,
        kLowX = 0x02,
        kHighY = 0x04,
        kHighX = 0x08,
        kDiagonalFalling = 0x10,  // from the tile's (low x, high y) corner to its (high x, low y) corner
        kDiagonalRising = 0x20,   // from (low x, low y) to (high x, high y)
    };

    // Every wall segment of the lot's room graph (walls and fences alike).
    static WallTiles fromLot(const content::Lot& lot);
    // Only the segments built as walls (a wall style): no fences, no lines that only divide rooms.
    static WallTiles builtWalls(const content::Lot& lot);

    // A wall from (x0, y0) to (x1, y1) along the grid or diagonally across tiles.
    void add(int level, float x0, float y0, float x1, float y1);
    // The sides of tile (x, y) a wall runs along or across.
    std::uint8_t at(int level, int x, int y) const;

private:
    void mark(int level, int x, int y, std::uint8_t side);
    std::map<std::tuple<int, int, int>, std::uint8_t> sides_;
};

// Whether the Sim is partway through a route: its Go To Routing Slot is still on its stack. A walk
// left behind by a tree that was left some other way doesn't count.
bool routing(const SimObject& sim);

// Generic Sims Call 0x2B "Is It Ok to Idle Here": false when another Sim stands within 0.6 tiles of
// the Sim, or a wall on the Sim's tile passes within 0.4 of it. A Sim partway through a route
// doesn't count, so Sims walking past don't crowd the spot.
bool okToIdleHere(World& world, const SimObject& sim);

// Generic Sims Call 0x34 "Wall in Front?": whether a wall crosses the line from the Sim to 1.5 tiles
// ahead of it, along its facing (object data 1, 45 degrees a step). When one does, the result is the room of the
// wall's tile nearer the Sim.
[[nodiscard]] std::optional<std::int32_t> wallInFront(const World& world, const SimObject& sim);

// Whether a wall on level `level` (0 = ground) crosses the line from (x0, y0) to (x1, y1), in tiles.
bool wallBetween(const World& world, int level, float x0, float y0, float x1, float y1);

// wallBetween on the given walls.
bool wallCrosses(const WallTiles& walls, int level, float x0, float y0, float x1, float y1);

// How far (x, y) is from the nearest wall along its tile's sides or across it, in tiles, infinity
// when the tile has none.
float wallDistanceOnTile(const WallTiles& walls, int level, float x, float y);

// Like wallBetween on the given walls, but a wall the line only touches (at either's end) counts too.
bool wallTouches(const WallTiles& walls, int level, float x0, float y0, float x1, float y1);

} // namespace ts2::sim
