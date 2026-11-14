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
    // For writing it back as it was: the 64-byte name field as saved (empty: the name, padded with
    // zeros), whether the table ended after it, and the bytes after the entries (all of them after
    // the format word when the format isn't supported).
    std::vector<std::uint8_t> nameField;
    bool nameOnly = false;
    std::vector<std::uint8_t> tail;

    std::vector<const Entry*> forLanguage(std::uint8_t language) const;
};

StringTable parse(std::span<const std::uint8_t> data);
// The table as the game writes it: encode(parse(bytes)) gives the bytes back.
std::vector<std::uint8_t> encode(const StringTable& table);

} // namespace ts2::str
