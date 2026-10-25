#pragma once

#include <cstdint>
#include <span>
#include <unordered_map>

namespace ts2::neighbor_remap {

// A lot's neighbor id remap tables (resource 0x6E9C59CF, instance 2): a version, then seven tables of
// 32-bit numbers. Tables 1 and 2 are id pairs, 3 and 4 id lists, 5 to 7 id pairs. Version 1 has only
// table 1, version 0 none. Only table 6 is kept: the neighbor id each id maps to, which Generic Sims
// Call 0x30 looks up. A key saved twice keeps its last value.
struct NeighborRemap {
    std::uint32_t version = 0;
    std::unordered_map<std::uint32_t, std::uint32_t> neighborIds;
};

NeighborRemap parse(std::span<const std::uint8_t> data);

} // namespace ts2::neighbor_remap
