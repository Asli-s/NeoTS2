#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
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

constexpr std::uint32_t kType = 0xCC364C2A;
constexpr std::int32_t kWrittenVersion = 2;

// A relationship as the game writes it now: the version 2, the count, then the values, and nothing
// after them.
struct Record {
    std::vector<std::int32_t> values;
};

// Empty for another version, more values than the Relationship primitive can address, or bytes cut
// short or left over.
std::optional<Record> readRecord(std::span<const std::uint8_t> data);

// Throws std::runtime_error for more values than the Relationship primitive can address.
std::vector<std::uint8_t> writeRecord(const Record& record);

} // namespace ts2::srel
