#pragma once

#include <array>
#include <cstdint>
#include <optional>
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

// Every byte of what stands on a neighborhood's terrain as the game writes it now (version 0xCB),
// to be written back. Plants and decorations (list version 8) and roads and bridges (list version 3)
// each start with the tile's centre and bounds. Floats are kept as read, so they write back exactly.
constexpr std::uint32_t kObjectsWrittenVersion = 0xCB;

struct PlacedRecord {
    std::array<float, 3> position{};
    std::array<float, 4> bounds{};
    float angle = 0;
    std::uint32_t type = 0;  // the catalog guid, 0 for none
};

// A smaller piece of road drawn on a tile, with three corners.
struct RoadPart {
    std::uint32_t value = 0;  // meaning not known yet
    std::array<std::array<float, 5>, 3> corners{};
};

struct RoadRecord {
    std::array<float, 3> center{};
    std::array<float, 4> bounds{};
    std::array<std::array<float, 5>, 4> corners{};
    std::uint32_t piece = 0;
    std::uint32_t value = 0;            // meaning not known yet
    std::uint8_t flags1 = 0, flags2 = 0;  // the piece's connections and flags, not known yet
    std::vector<RoadPart> parts;
};

struct BridgeRecord {
    RoadRecord road;
    std::array<float, 3> start{};
    std::array<float, 4> rotation{0, 0, 0, 1};
    std::array<float, 3> end{};
};

struct ObjectsRecord {
    std::vector<PlacedRecord> flora;
    std::vector<RoadRecord> roads;
    std::vector<BridgeRecord> bridges;
    std::vector<PlacedRecord> decorations;
};

// Empty for another version of the record, a list or an entry, or bytes cut short or left over.
std::optional<ObjectsRecord> readObjectsRecord(std::span<const std::uint8_t> data);

// Throws std::runtime_error for a list too long to write.
std::vector<std::uint8_t> writeObjectsRecord(const ObjectsRecord& record);

} // namespace ts2::nhood
