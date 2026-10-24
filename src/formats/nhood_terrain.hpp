#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ts2::nhood {

// A neighborhood's terrain (resource 0xABCB5DA4 in <hood>_Neighborhood.package): its size in tiles,
// a level (312.5 in Pleasantview: the sea), the terrain type naming its paints ("Temperate",
// "Desert", ...) and, one per tile corner row by row, the corner heights and a byte (0 everywhere in
// Pleasantview, meaning not known).
struct Terrain {
    std::uint32_t version = 0;
    std::uint32_t width = 0, height = 0;  // tiles
    float seaLevel = 0;
    std::string type;
    std::uint32_t columns = 0, rows = 0;  // corners: one more than the tiles each way
    std::vector<float> heights;
    std::vector<std::uint8_t> extra;
};

constexpr std::uint32_t kTerrainType = 0xABCB5DA4;

Terrain parseTerrain(std::span<const std::uint8_t> data);

// Where the neighborhood view's camera was when the neighborhood was last saved (resource
// 0xEC44BDDC, group 0x262A): its distance, its angle around the point it looks at, and that point.
struct SavedCamera {
    float distance = 0;
    float yaw = 0;  // radians, the camera sitting at (cos, sin) of it from the point
    float target[3] = {0, 0, 0};
};

constexpr std::uint32_t kSavedCameraType = 0xEC44BDDC;

// Empty when the record is not the 115-byte layout every shipped neighborhood uses.
[[nodiscard]] std::optional<SavedCamera> parseSavedCamera(std::span<const std::uint8_t> data);

} // namespace ts2::nhood
