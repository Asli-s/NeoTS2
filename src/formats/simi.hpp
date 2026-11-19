#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ts2::simi {

constexpr std::uint32_t kType = 0x53494D49;
constexpr std::size_t kGlobalCount = 60;

// Simulator state saved with a lot (SIMI): a 64-byte name, "IMIS", the version, a reserved word,
// then the simulator's global variables (hour, day of month, minute, second, month, year, the
// current house, the budget, the speed...), three values not read yet, the random number state the
// simulator's ticks run on, and a second random number state for what runs outside the ticks.
// Version 0xD8 saves 60 globals. Version 0xAD, which the lots the game ships are saved at, saves
// the first 54. Only the globals and the ticks' state are read so far.
struct SimulatorState {
    std::uint32_t version = 0;
    std::vector<std::int16_t> globals;
    std::optional<std::uint32_t> randomState;  // none in older versions or a file cut short
};

SimulatorState parse(std::span<const std::uint8_t> data);

// How many globals a record of `version` saves, as the game's reader counts them (60 from 0xD4).
// The rest read as 0. 0 for versions before 0x12, which keep them another way.
std::size_t savedGlobalCount(std::uint32_t version);

// The four parts of a lot's value, as its simulator state keeps them from version 0x25, in file order.
// Versions before 0xAD keep no third part.
struct LotValue {
    std::int32_t first = 0, second = 0, third = 0, fourth = 0;
    std::int16_t global15 = 0;  // global 0x15: while it isn't 0 the lot isn't shown for sale
};

// The lot value parts, or none for a version before 0x25, a wrong magic or a record cut short.
std::optional<LotValue> readLotValue(std::span<const std::uint8_t> data);

// A lot's value as the game adds it: the first three parts and the fourth times 0.8 with its fraction dropped.
// A lot whose description has flag 8 set (flags from version 5) leaves out the second part.
std::int32_t lotValue(const LotValue& parts, bool leaveOutSecond);

// The version the game writes. It reads older ones and saves them as this.
constexpr std::uint32_t kWrittenVersion = 0xD8;
constexpr std::size_t kRecordSize = 498;

// The version of the lots the game ships, which a lot keeps until the game first saves it.
constexpr std::uint32_t kShippedVersion = 0xAD;
constexpr std::size_t kShippedRecordSize = 438;
constexpr std::size_t kShippedGlobalCount = 54;

// The whole record of the version the game writes, every byte kept, to be written back. Fields
// whose meaning is not known yet are kept as their bytes.
struct Record {
    std::array<std::uint8_t, 64> name{};
    std::array<std::int16_t, kGlobalCount> globals{};
    std::array<std::uint8_t, 10> beforeRandomStates{};
    std::uint32_t tickRandomState = 0;   // the state the simulator's ticks draw from
    std::uint32_t otherRandomState = 0;  // the state draws outside the ticks use
    // Five values, then six blocks of a version (2) and ten values.
    std::array<std::uint8_t, 284> rest{};
};

// Reads a record of the version the game writes, or of the shipped version upgraded to it as the
// game upgrades one: the six globals the shipped version lacks are 0 until the simulator sets them,
// and each block moves to version 2 with its two newer values 0. Empty for any other version, a wrong
// magic or size, or a reserved word that is not 0 (the game always writes 0 there).
std::optional<Record> readRecord(std::span<const std::uint8_t> data);

// Writes the record as the game writes it, kRecordSize bytes.
std::vector<std::uint8_t> writeRecord(const Record& record);

} // namespace ts2::simi
