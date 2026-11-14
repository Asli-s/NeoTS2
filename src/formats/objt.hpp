#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ts2::objt {

// A lot's object type table (OBJT): one entry per object type the lot's saved objects use, with its
// GUID and kind.
constexpr std::uint16_t kPersonKind = 2;

struct Entry {
    std::uint32_t guid = 0;
    std::array<std::uint16_t, 10> unknown{};
    std::uint16_t index = 0;
    std::uint16_t kind = 0;
    std::string name;
    std::uint32_t flag = 0;
    // Equal to guid for ordinary objects, different for some custom and downloaded content.
    std::uint32_t secondGuid = 0;
};

struct ObjectTypeTable {
    std::uint32_t version = 0;
    std::uint32_t unknown = 0;
    std::vector<Entry> entries;

    const Entry* find(std::uint16_t index) const;
};

ObjectTypeTable parse(std::span<const std::uint8_t> data);
// The table saved as parse reads it, after `name` (the 64-byte name its resource starts with).
std::vector<std::uint8_t> write(const ObjectTypeTable& table, std::span<const std::uint8_t> name);

} // namespace ts2::objt
