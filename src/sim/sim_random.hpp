#pragma once

#include <cstdint>
#include <limits>

namespace ts2::sim {

// The simulator's random numbers, as the game makes them: a 32-bit state stepped by
// state * 0x41C64E6D + 12345, each draw the bits 16 to 47 of that 64-bit sum. A draw in a range n is
// (draw * n) >> 32. The state is saved with the lot (SIMI) and set straight back on loading it.
class SimRandom {
public:
    using result_type = std::uint32_t;

    explicit SimRandom(std::uint32_t state) : state_(state) {}

    result_type operator()() {
        const std::uint64_t sum = static_cast<std::uint64_t>(state_) * 0x41C64E6Du + 12345u;
        state_ = static_cast<std::uint32_t>(sum);
        return static_cast<std::uint32_t>(sum >> 16);
    }
    void seed(std::uint32_t state) { state_ = state; }
    std::uint32_t state() const { return state_; }

    static constexpr result_type min() { return 0; }
    static constexpr result_type max() { return std::numeric_limits<result_type>::max(); }

private:
    std::uint32_t state_;
};

} // namespace ts2::sim
