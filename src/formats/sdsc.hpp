#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ts2::sdsc {

// Sim description (SDSC): one neighbor of a neighborhood. Holds the neighbor's copy of its person
// data, its id and its GUID; the rest of the record is kept raw.
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

} // namespace ts2::sdsc
