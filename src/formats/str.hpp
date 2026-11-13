#pragma once

#include <cstdint>
#include <optional>
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

// Every byte of a string table (STR#, CTSS catalog strings, TTAs) as the game writes it, to be
// written back. The game always writes format 0xFFFD: the name padded with zero bytes to 64, then
// the entries grouped by language. Languages come in the order of (language - 1) as a signed byte,
// so 129 to 255 come first, then 0, then 1 to 128. Each language keeps its texts in order. When the
// game writes a language that has no description list of its own, it writes language 1's.
constexpr std::uint16_t kWrittenFormat = 0xFFFD;

struct Record {
    std::string name;
    std::vector<Entry> entries;
};

// Empty for another format, a name not followed by only zero bytes, entries out of the game's
// order, or bytes cut short or left over.
std::optional<Record> readRecord(std::span<const std::uint8_t> data);

// Throws std::runtime_error for a name over 64 bytes, a name or text holding a zero byte, entries
// out of the game's order, or more entries than 65,535.
std::vector<std::uint8_t> writeRecord(const Record& record);

} // namespace ts2::str
