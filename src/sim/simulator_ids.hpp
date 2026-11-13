#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace ts2::sim {

// Simulator global 0x24 (named "Current Interaction GUID" in the game's list) is the simulator's
// counter of ids. It is saved with the lot, so ids carry on across loads.
constexpr std::size_t kIdCounterGlobal = 0x24;

// The next id the simulator hands out, as the game does: the counter's value, then the counter one
// up. 16 bits wrap, and a counter at 0 starts at 1, so 0 is never given out. Object timers take
// their ids from it.
inline std::uint16_t nextSimulatorId(std::span<std::int16_t> globals) {
    std::int16_t& counter = globals[kIdCounterGlobal];
    if (counter == 0) {
        counter = 1;
    }
    const auto id = static_cast<std::uint16_t>(counter);
    counter = static_cast<std::int16_t>(static_cast<std::uint16_t>(id + 1));
    return id;
}

} // namespace ts2::sim
