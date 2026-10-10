#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace ts2::nhood {

// A lot as the neighborhood places it (resource 0x0BF999E7 in <hood>_Neighborhood.package, one per
// lot, instance = lot id). It starts with the lot's own description, the same block its lot package
// keeps (resource 0x6C589723 there).
struct LotDescription {
    std::uint16_t version = 0;
    std::uint32_t width = 0, depth = 0;  // tiles
    std::uint8_t type = 0;
    std::uint8_t roadSides = 0;  // a bit per side with a road, 1 << ownFacing among them
    std::uint8_t ownFacing = 0;  // the quarter turn the lot was built facing
    std::string name;
    std::uint32_t x = 0, y = 0;  // the lot's corner, in neighborhood tiles
    float elevation = 0;
    std::uint32_t id = 0;
    std::uint8_t facing = 0;  // the quarter turn it is placed at in the neighborhood
    std::string texture;
};

constexpr std::uint32_t kLotDescriptionType = 0x0BF999E7;

LotDescription parseLotDescription(std::span<const std::uint8_t> data);

} // namespace ts2::nhood
