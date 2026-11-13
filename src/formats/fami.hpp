#pragma once

#include <cstddef>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
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

constexpr std::uint32_t kType = 0x46414D49;
constexpr std::uint32_t kWrittenVersion = 0x55;

// The family as the game writes it, at version 0x55 with every value from the struct. Throws
// std::runtime_error for a member list too long to write.
std::vector<std::uint8_t> write(const Family& family);

// A family's history (FAMh 0x46414D68, version 0x55): items of numbers not known yet, each with a
// list of texts (32-bit lengths, as the neighborhood package writes text).
constexpr std::uint32_t kHistoryType = 0x46414D68;
struct HistoryItem {
    std::array<std::uint16_t, 8> shorts1{};
    std::array<std::int32_t, 2> values1{};
    std::array<std::uint16_t, 2> shorts2{};
    std::array<std::int32_t, 8> values2{};
    std::array<std::uint16_t, 3> shorts3{};
    std::array<bool, 3> flags{};
    std::vector<std::string> texts;
};
struct History {
    std::vector<HistoryItem> items;
};

// Empty for another type or version, a value the game writes as 0 that is not, a flag other than 0
// or 1, or bytes cut short or left over.
std::optional<History> readHistory(std::span<const std::uint8_t> data);
// Throws std::runtime_error for a list or text too long to write.
std::vector<std::uint8_t> writeHistory(const History& history);

} // namespace ts2::fami
