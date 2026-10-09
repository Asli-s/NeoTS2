#pragma once

#include "formats/xobj.hpp"

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace ts2::ngbh {

// An inventory: counted tokens (whose first property is the count) and singular tokens.
struct Inventory {
    std::uint32_t version = 0;
    std::vector<xobj::Token> counted;
    std::vector<xobj::Token> singular;
};

// The neighborhood record (NGBH): its name and settings, and every inventory the neighborhood
// keeps: one global inventory and one per lot, family and neighbor.
struct Neighborhood {
    std::uint32_t version = 0;
    std::string terrain;
    std::vector<std::int16_t> values;
    Inventory global;
    std::map<std::int32_t, Inventory> lots;
    std::map<std::int32_t, Inventory> families;
    std::map<std::int32_t, Inventory> neighbors;
    std::size_t extraBytes = 0;  // left after the last field the game reads
};

Neighborhood parse(std::span<const std::uint8_t> data);

} // namespace ts2::ngbh
