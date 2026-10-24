#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace ts2::nhood {

// A road tile of a neighborhood: its four corners (x, y, z in world units, then u, v of the road
// texture), corner order (x0,y0) (x0,y1) (x1,y1) (x1,y0), and the piece it shows. The game names
// its material "neighborhood-roads-<terrain type>-<piece, 8 hex digits>".
struct RoadTile {
    std::array<float, 3> center{};
    std::array<float, 4> bounds{};  // x0, y0, x1, y1
    std::array<std::array<float, 5>, 4> corners{};
    std::uint32_t piece = 0;
};

// A plant or decoration of a neighborhood: where it stands, the ground it covers (x0, y0, x1, y1),
// its turn in degrees and its type (the "guid" of its entry in the neighborhood object catalog).
struct PlacedObject {
    std::array<float, 3> position{};
    std::array<float, 4> bounds{};
    float angle = 0;
    std::uint32_t type = 0;
};

// A bridge piece: a road tile whose piece names the model drawn there ("<piece >> 16, 4 hex
// digits>-bridge"), turned by `rotation` (a quaternion x, y, z, w) about the tile's centre.
struct Bridge {
    RoadTile road;
    std::array<float, 4> rotation{0, 0, 0, 1};
};

// What stands on a neighborhood's terrain (resource 0xABD0DC63 in <hood>_Neighborhood.package):
// its plants, roads, bridges and decorations (rocks, signs, surf and the other catalog items placed
// with the decorations tool, kept as plants are).
struct Objects {
    std::uint32_t version = 0;
    std::vector<PlacedObject> flora;
    std::vector<RoadTile> roads;
    std::vector<Bridge> bridges;
    std::vector<PlacedObject> decorations;
};

constexpr std::uint32_t kObjectsType = 0xABD0DC63;

Objects parseObjects(std::span<const std::uint8_t> data);

} // namespace ts2::nhood
