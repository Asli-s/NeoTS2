#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace ts2::stringmap {

// A lot's pattern list (cStringMap, resource type 0xCAC4FC40): the names of the floors or wall
// coverings it uses, by the id its grids and walls store.
struct Entry {
    std::string name;      // a pattern's GUID in hex, or a material name without its "floor_" prefix
    std::uint32_t uses = 0;
};

std::map<std::uint16_t, Entry> parse(std::span<const std::uint8_t> data);

constexpr std::uint32_t kType = 0xCAC4FC40;

// The map as the game writes it (version 1), entries in the order of their names ignoring case.
// Throws std::runtime_error for two names that differ only in case, which the game keeps as one,
// or a list too long to write.
std::vector<std::uint8_t> write(const std::map<std::uint16_t, Entry>& entries);

} // namespace ts2::stringmap
