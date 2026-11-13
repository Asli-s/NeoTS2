#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The lot's world database record (cWorldDB, resource 0x49FF7D76) as the game writes it: 64 zero
// bytes, the type, the version 7, the class name, six numbers, then a flag and text when it is set.
// What the numbers and the text hold is not known yet.
namespace ts2::worlddb {

constexpr std::uint32_t kType = 0x49FF7D76;
constexpr std::uint32_t kWrittenVersion = 7;

struct Record {
    std::array<std::int32_t, 6> values{};
    std::optional<std::string> text;
};

// Empty for another type, version or class name, a name that is not all zeros, a flag other than 0
// or 1, a length written longer than needed, or bytes cut short or left over.
std::optional<Record> readRecord(std::span<const std::uint8_t> data);

std::vector<std::uint8_t> writeRecord(const Record& record);

} // namespace ts2::worlddb
