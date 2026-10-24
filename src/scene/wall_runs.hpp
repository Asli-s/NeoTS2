#pragma once

#include "scene/wall_light.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace ts2::content {
struct Lot;
}

namespace ts2::scene::wallruns {

// Lays a lot's walls out the way the game's lighting manager lights them: every wall face is
// followed round the room (or outdoors) it looks into, each straight piece of that loop becomes a
// run of one-tile segments, and a segment is linked to the same wall tile on the level above or
// below when no floor lies between them.

// A wall between whole tile points on a level: along a tile edge or diagonally across tiles.
struct Wall {
    int level = 0;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    std::uint32_t covering = 0;  // the wall style, or a fence's GUID
};

// Which tile a segment's face looks into and from which side, as the game names wall tiles:
// side 1 (-y), 2 (-x), 4 (+y), 8 (+x) for walls on a tile edge. 0x10 (along x + y = constant) or
// 0x20 (along x = y) for a diagonal across the tile, with `part` the half the face looks into
// (0x10: 2 toward -x -y, 4 toward +x +y, 0x20: 1 toward +x -y, 3 toward -x +y).
struct Key {
    int x = 0, y = 0, level = 0, side = 0, part = 0;
    bool operator==(const Key&) const = default;
};

// The room at a point (in tiles) on a level, as the floor light grid sees it.
using RoomAt = std::function<std::int16_t(int level, float x, float y)>;
// Whether solid floor on `level` lies under the wall tile a key names (so light can't pass it).
using FloorSolid = std::function<bool(int level, const Key& key)>;

struct Layout {
    std::vector<walllight::Run> runs;
    std::vector<std::vector<Key>> keys;  // per run, per segment
    bool complete = true;                // false when walls were left out at the safety limits
};

Layout build(const std::vector<Wall>& walls, const RoomAt& roomAt, const FloorSolid& floorSolid);

// The wall tile a face walked from (x0, y0) to (x1, y1) on a level looks into, and the room there.
Key keyOf(int level, int x0, int y0, int x1, int y1);
std::int16_t roomInFront(const Key& key, const RoomAt& roomAt);

// The quarters of the key's tile the face looks into: bit 0 -x, 1 +y, 2 +x, 3 -y.
int quarters(const Key& key);

// A lot's floor test: ground is always solid. Above it, every quarter the face looks into must
// have floor.
FloorSolid floorSolidOf(const content::Lot& lot);

// The wall light the game reads for a portal: one texel (the second half, fourth row) of the
// wall face at that wall tile, as 0..1. With `dimOutside`, faces looking outdoors (room 0) have the
// outside wall ambient (colour times strength) taken off. Black where no face is.
std::array<float, 3> portalSample(const std::vector<walllight::Run>& runs, const Layout& layout, const Key& key, bool dimOutside,
                                  const std::array<float, 3>& outsideColor, float outsideStrength);

// The walls of a lot that stop light, from its room wall graph.
using BlocksLight = std::function<bool(std::uint32_t covering)>;
std::vector<Wall> wallsOf(const content::Lot& lot, const BlocksLight& blocksLight);

} // namespace ts2::scene::wallruns
