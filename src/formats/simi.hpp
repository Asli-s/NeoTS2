#pragma once

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

} // namespace ts2::simi
