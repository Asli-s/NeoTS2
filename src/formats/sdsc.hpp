#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ts2::sdsc {

// Sim description (SDSC): one neighbor of a neighborhood. Holds the neighbor's copy of its person
// data, its id and its GUID. The rest of the record is kept raw.
struct SimDescription {
    std::uint32_t version = 0;
    std::int32_t personDataVersion = 0;
    std::uint32_t personDataFlag = 0;
    std::vector<std::int16_t> personData;
    std::int16_t neighborId = 0;
    std::uint32_t guid = 0;
    std::vector<std::uint8_t> rest;
};

// How many person-data values a record with this person-data version stores.
std::size_t personDataCount(std::int32_t personDataVersion);

SimDescription parse(std::span<const std::uint8_t> data);

constexpr std::uint32_t kType = 0xAACE2EFB;
constexpr std::uint32_t kWrittenVersion = 0x71;
constexpr std::int32_t kWrittenPersonDataVersion = 0x36;
constexpr std::size_t kWrittenPersonDataCount = 231;

// Every byte of a version 0x71 record, as the game writes it: the version, the person-data version
// 0x36 twice, the 231 person-data values, the neighbor id and GUID, the ids of the neighbors it has a
// relationship with (in the game's own order), a zero byte and two numbers not known yet.
struct Record {
    std::array<std::int16_t, kWrittenPersonDataCount> personData{};
    std::int16_t neighborId = 0;
    std::uint32_t guid = 0;
    std::vector<std::uint32_t> relationships;
    std::uint32_t value1 = 0, value2 = 0;
};

// Empty for another version, a list version other than 3, a byte the game writes as 0 that is not,
// or bytes cut short or left over.
std::optional<Record> readRecord(std::span<const std::uint8_t> data);

// Throws std::runtime_error for a list too long to write.
std::vector<std::uint8_t> writeRecord(const Record& record);

} // namespace ts2::sdsc
