#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace ts2::nhood {

// A road tile of a neighborhood: its four corners (x, y, z in world units, then u, v of the road
// texture), corner order (x0,y0) (x0,y1) (x1,y1) (x1,y0), and the piece it shows; the game names
// its material "neighborhood-roads-<terrain type>-<piece, 8 hex digits>".
struct RoadTile {
    std::array<float, 3> center{};
    std::array<float, 4> bounds{};  // x0, y0, x1, y1
    std::array<std::array<float, 5>, 4> corners{};
    std::uint32_t piece = 0;
};

// A tree or other plant of a neighborhood: where it stands, the ground it covers (x0, y0, x1, y1),
// its turn in degrees and its type (the "guid" of its entry in the neighborhood object catalog).
struct Flora {
    std::array<float, 3> position{};
    std::array<float, 4> bounds{};
    float angle = 0;
    std::uint32_t type = 0;
};

// What stands on a neighborhood's terrain (resource 0xABD0DC63 in <hood>_Neighborhood.package):
// its plants and roads; the other lists in the same resource are skipped.
struct Objects {
    std::uint32_t version = 0;
    std::vector<Flora> flora;
    std::vector<RoadTile> roads;
};

constexpr std::uint32_t kObjectsType = 0xABD0DC63;

Objects parseObjects(std::span<const std::uint8_t> data);

} // namespace ts2::nhood
