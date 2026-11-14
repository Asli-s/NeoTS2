#pragma once

#include "formats/xobj.hpp"

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <utility>
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

// The record as saved, every value kept in order, for changing it and writing it back: the
// inventories as listed (tokens as saved, without the properties parse gives counted tokens),
// the values the game reads past, and the bytes after the last one it reads.
struct Raw {
    std::uint32_t type = 0;  // 'NGBH'
    std::uint32_t version = 0;
    std::uint32_t value0 = 0;
    std::uint32_t value1 = 0, value2 = 0;  // above version 0x6A
    std::string terrain;                   // above version 0x6D
    std::vector<std::int16_t> values;      // 1 below version 0x14, else 16
    Inventory global;                      // the inventories, above version 0x65
    std::vector<std::pair<std::int32_t, Inventory>> lots, families, neighbors;
    std::uint8_t lastByte = 0;    // above version 0x6E
    std::uint32_t lastValue = 0;  // from version 0x70
    std::vector<std::uint8_t> tail;
};
Raw readRaw(std::span<const std::uint8_t> data);
// The record as the game writes it: encode(readRaw(bytes)) gives the bytes back.
std::vector<std::uint8_t> encode(const Raw& raw);

} // namespace ts2::ngbh
