#include "platform/platform.hpp"

#include <chrono>
#include <thread>

namespace ts2::platform {

namespace {

using Clock = std::chrono::steady_clock;

// Counted from the first look at the clock, which the game takes as it starts. A count from when the
// computer started would be too large for the floats the game's animations are timed in.
const Clock::time_point kStart = Clock::now();

} // namespace

std::uint64_t ticksNs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - kStart).count());
}

void delayMs(std::uint32_t milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

} // namespace ts2::platform
