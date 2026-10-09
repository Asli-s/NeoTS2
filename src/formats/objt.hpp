#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ts2::objt {

constexpr std::uint16_t kPersonKind = 2;

struct Entry {
    std::uint32_t guid = 0;
    std::array<std::uint16_t, 10> unknown{};
    std::uint16_t index = 0;
    std::uint16_t kind = 0;
    std::string name;
    std::uint32_t flag = 0;
    // Equal to guid for ordinary objects; differs for some custom and downloaded content.
    std::uint32_t secondGuid = 0;
};

struct ObjectTypeTable {
    std::uint32_t version = 0;
    std::uint32_t unknown = 0;
    std::vector<Entry> entries;

    const Entry* find(std::uint16_t index) const;
};

ObjectTypeTable parse(std::span<const std::uint8_t> data);

} // namespace ts2::objt
