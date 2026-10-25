#include "platform/platform.hpp"

#ifdef NEOTS2_PLATFORM_SDL3
#include <SDL3/SDL.h>
#else
#include <chrono>
#include <thread>
#ifdef __APPLE__
#include <mach/mach_time.h>
#endif
#endif

namespace ts2::platform {

#ifdef NEOTS2_PLATFORM_SDL3

// SDL's own clock and wait, as the game always used: on Windows, SDL_Delay waits more finely than a
// thread's sleep does (which goes by the system's 15.6 ms timer ticks).
std::uint64_t ticksNs() {
    return SDL_GetTicksNS();
}

void delayMs(std::uint32_t milliseconds) {
    SDL_Delay(milliseconds);
}

#else

namespace {

// Counted from the first look at the clock, which the game takes as it starts. A count from when the
// computer started would be too large for the floats the game's animations are timed in.
#ifdef __APPLE__
// The system's own tick count. Before Mac OS X 10.12 the standard clock is the wall clock, which jumps
// when it is set, so the time since the start would go backwards (and wrap, being unsigned).
std::uint64_t machNs() {
    static const mach_timebase_info_data_t base = [] {
        mach_timebase_info_data_t b{};
        mach_timebase_info(&b);
        return b;
    }();
    const std::uint64_t ticks = mach_absolute_time();
    // Split so that the ticks times the numerator can't overflow.
    const std::uint64_t whole = ticks / base.denom, rest = ticks % base.denom;
    return whole * base.numer + rest * base.numer / base.denom;
}

const std::uint64_t kStart = machNs();
#else
using Clock = std::chrono::steady_clock;
const Clock::time_point kStart = Clock::now();
#endif

} // namespace

std::uint64_t ticksNs() {
#ifdef __APPLE__
    return machNs() - kStart;
#else
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - kStart).count());
#endif
}

void delayMs(std::uint32_t milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

#endif

} // namespace ts2::platform
