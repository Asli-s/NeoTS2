#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <string>

namespace ts2::stringmap {

// A lot's pattern list (cStringMap, resource type 0xCAC4FC40): the names of the floors or wall
// coverings it uses, by the id its grids and walls store.
struct Entry {
    std::string name;      // a pattern's GUID in hex, or a material name without its "floor_" prefix
    std::uint32_t uses = 0;
};

std::map<std::uint16_t, Entry> parse(std::span<const std::uint8_t> data);

} // namespace ts2::stringmap
