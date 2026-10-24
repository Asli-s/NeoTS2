#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ts2::sim {

// A timer On Timer (0x7A) set on an object: it counts down once a tick, and when it runs out the
// object runs `tree` from its own code, with the object that set it as stack object and `args`.
// A repeating timer whose tree returns true starts over from `interval`. Any other one is removed.
struct ObjectTimer {
    std::int32_t countdown = 0;
    std::int32_t interval = 0;
    std::uint16_t tree = 0;
    bool repeats = false;
    std::uint16_t id = 0;
    std::int16_t setter = 0;
    std::array<std::int16_t, 8> args{};
};

// The game keeps no limit. This bounds what a runaway script can pile onto one object.
constexpr std::size_t kMaxObjectTimers = 256;

} // namespace ts2::sim
