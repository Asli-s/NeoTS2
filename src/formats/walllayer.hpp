#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace ts2::walllayer {

// One wall segment's coverings: ids into the lot's wall pattern map, 0 for none.
struct Entry {
    std::uint32_t wall = 0;  // the segment's id in the room wall graph
    std::uint32_t type = 0;  // the wall style, or for a fence its GUID in the build catalog
    std::uint16_t left = 0;
    std::uint16_t right = 0;
};

// cWallLayer (resource type 0x8A84D7B0).
std::vector<Entry> parse(std::span<const std::uint8_t> data);

} // namespace ts2::walllayer
