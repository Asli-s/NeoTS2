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
// then the simulator's 60 global variables (hour, day of month, minute, second, month, year, the
// current house, the budget, the speed...), then from version 0xD4 three values not read yet, the
// random number state the simulator's ticks run on, and a second random number state for what runs
// outside the ticks. Only the globals and the ticks' state are read so far.
struct SimulatorState {
    std::uint32_t version = 0;
    std::vector<std::int16_t> globals;
    std::optional<std::uint32_t> randomState;  // none in older versions or a file cut short
};

SimulatorState parse(std::span<const std::uint8_t> data);

// The version the game writes. It reads older ones and saves them as this.
constexpr std::uint32_t kWrittenVersion = 0xD8;
constexpr std::size_t kRecordSize = 498;

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

// Reads a record of the version the game writes. Empty for any other version, a wrong magic or
// size, or a reserved word that is not 0 (the game always writes 0 there).
std::optional<Record> readRecord(std::span<const std::uint8_t> data);

// Writes the record as the game writes it, kRecordSize bytes.
std::vector<std::uint8_t> writeRecord(const Record& record);

} // namespace ts2::simi
