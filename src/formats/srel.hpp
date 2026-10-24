#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ts2::srel {

// Sim relationship (SREL): the relationship variables one neighbor holds towards another,
// stored with instance (A << 16) | B. The game stops reading at the count. Some records have
// leftover bytes after it.
struct Relationship {
    std::int32_t version = 0;
    std::vector<std::int32_t> values;
    std::size_t extraBytes = 0;
};

Relationship parse(std::span<const std::uint8_t> data);

} // namespace ts2::srel
