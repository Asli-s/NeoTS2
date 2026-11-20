#pragma once

#include "formats/xobj.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <utility>
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

constexpr std::uint32_t kType = 0x4E474248;
constexpr std::uint32_t kWrittenVersion = 0xCB;
constexpr std::uint32_t kInventoryVersion = 0xBE;

// Every byte of a version 0xCB neighborhood record, as the game writes it, to be written back: the
// tokens exactly as stored (the reader above adds the properties the game gives counted tokens when
// it loads them), and each keyed inventory in the game's order.
struct Record {
    std::uint32_t value0 = 0;  // not known yet
    std::uint32_t value1 = 0, value2 = 0;  // not known yet
    std::string terrain;
    std::array<std::int16_t, 16> values{};
    Inventory global;
    std::vector<std::pair<std::int16_t, Inventory>> lots;
    std::vector<std::pair<std::int16_t, Inventory>> families;
    std::vector<std::pair<std::int16_t, Inventory>> neighbors;
    std::uint8_t flag = 0;     // not known yet
    std::uint32_t value3 = 0;  // not known yet
};

// Empty for another type or version, an inventory of another version, a key outside 16 bits, or
// bytes cut short or left over.
std::optional<Record> readRecord(std::span<const std::uint8_t> data);

// Throws std::runtime_error for a list or text too long to write.
std::vector<std::uint8_t> writeRecord(const Record& record);

// The game-wide inventory, which no neighborhood owns (the Game Tips flags are kept in it). The game
// keeps it in Neighborhoods/NeighborhoodManager.package as this resource: the object save version,
// then the inventory as the neighborhood record stores one. The game writes it when it quits.
constexpr std::uint32_t kGameWideType = 0x0ABA73AF;
constexpr std::uint32_t kGameWideGroup = 0xFFFFFFFF;
constexpr std::uint32_t kGameWideInstance = 1;
constexpr std::uint32_t kGameWideVersion = 0xD8;

// Empty for another version, an inventory of another version, or bytes cut short or left over.
// The tokens are as stored, with no properties added.
std::optional<Inventory> readGameWideRecord(std::span<const std::uint8_t> data);

// Writes the record at kGameWideVersion. Throws std::runtime_error for a list too long to write.
std::vector<std::uint8_t> writeGameWideRecord(const Inventory& inventory);

} // namespace ts2::ngbh
