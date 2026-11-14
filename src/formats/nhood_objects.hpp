#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <utility>
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

// Where a plant or decoration list lies in the resource: from its list version (`start`) to past
// its last record (`end`), and each record's first byte and end. Saving changes to the lists keeps
// every other byte as it was.
struct PlacedSpans {
    std::uint16_t listVersion = 0;
    std::size_t start = 0, end = 0;
    std::vector<std::pair<std::size_t, std::size_t>> records;
};
struct ObjectsLayout {
    PlacedSpans flora, decorations;
};

constexpr std::uint32_t kObjectsType = 0xABD0DC63;

// Reads the resource, and where its plants and decorations lie into `layout` when given.
Objects parseObjects(std::span<const std::uint8_t> data, ObjectsLayout* layout = nullptr);

// The resource `data` (laid out as `layout`) with only the plant and decoration records flagged in
// `keepFlora` and `keepDecorations` (one flag per record, in order; a record past a list's flags is
// kept) and the records in `addFlora` and `addDecorations` after them, written as the game writes
// them now: placed (2), with their bounds, record version 8. Every other byte is as in `data`.
// Throws when a list gaining records is laid out as before record versions (plants before list
// version 6, decorations before 7).
std::vector<std::uint8_t> rewritePlaced(std::span<const std::uint8_t> data, const ObjectsLayout& layout, const std::vector<bool>& keepFlora,
                                        const std::vector<PlacedObject>& addFlora, const std::vector<bool>& keepDecorations,
                                        const std::vector<PlacedObject>& addDecorations);

} // namespace ts2::nhood
