#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ts2::str {

// A string table (STR#): numbered texts, each in one language, each with an optional description.
struct Entry {
    std::uint8_t language = 1;
    std::string value;
    std::string description;
};

struct StringTable {
    std::string name;
    std::uint16_t format = 0;
    bool supported = true;
    std::vector<Entry> entries;

    std::vector<const Entry*> forLanguage(std::uint8_t language) const;
};

StringTable parse(std::span<const std::uint8_t> data);

} // namespace ts2::str
