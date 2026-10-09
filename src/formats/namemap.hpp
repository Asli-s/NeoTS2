#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ts2::namemap {

// A package's name map (NmaP, resource type 0x4E6D6150): the names of its resources of one type
// (the map's instance), by group and instance.
struct Entry {
    std::uint32_t group = 0;
    std::uint32_t instance = 0;
    std::string name;
};

std::vector<Entry> parse(std::span<const std::uint8_t> data);

} // namespace ts2::namemap
