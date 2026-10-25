#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ts2::fami {

// Family (FAMI). Values whose meaning is not known yet keep neutral names.
struct Family {
    std::uint32_t magic = 0;     // "IMAF"
    std::uint32_t version = 0;
    std::uint32_t unknown0 = 0;
    std::int32_t lot = 0;        // the lot the family lives on (Lots/<hood>_Lot<n>), 0 for none
    std::int32_t currentLot = 0;   // CachedFamily:getCurrentLotId, from version 0x50, else the lot
    std::int32_t vacationLot = 0;  // CachedFamily:getVacationLotId, from version 0x55, else 0
    std::int32_t value4 = 0;
    std::int32_t funds = 0;      // from version 2
    std::int32_t value6 = 0;     // from version 2
    std::uint32_t flags = 0;     // from version 7
    std::vector<std::uint32_t> members;  // the Sims' GUIDs
    std::uint32_t id7 = 0;       // above version 0x4A
    std::uint32_t value8 = 0;    // above version 0x4E
    std::int32_t funds2 = 0;     // from version 0x51, else the funds
    std::size_t extraBytes = 0;  // left after the last value
};

Family parse(std::span<const std::uint8_t> data);

} // namespace ts2::fami
