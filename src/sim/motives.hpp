#pragma once

#include "sim/world.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace ts2::sim {

// The motives (needs) a Sim keeps, by index.
namespace motive {
constexpr std::size_t kMood = 3;
constexpr std::size_t kEnergy = 5;
constexpr std::size_t kComfort = 6;
constexpr std::size_t kHunger = 7;
constexpr std::size_t kHygiene = 8;
constexpr std::size_t kBladder = 9;
constexpr std::size_t kEnvironment = 13;
constexpr std::size_t kSocial = 14;
constexpr std::size_t kFun = 15;
} // namespace motive

// Person data that, when not 0, stands in for the Environment motive.
constexpr std::size_t kEnvironmentScore = 0x79;
static_assert(kEnvironmentScore < kPersonDataCount);

// A motive as the game reads it: the stored value, except Environment, which person data 0x79
// replaces when it is not 0.
inline float motiveValue(const SimObject& sim, std::size_t index) {
    if (index >= kMotiveCount) {
        return 0;
    }
    if (index == motive::kEnvironment && sim.personData[kEnvironmentScore] != 0) {
        return sim.personData[kEnvironmentScore];
    }
    return sim.motives[index];
}

// A motive as the panels show it: a whole number, cut toward zero, kept within -100..100.
inline int wholeMotive(const SimObject& sim, std::size_t index) {
    const float value = motiveValue(sim, index);
    if (std::isnan(value)) {
        return 0;
    }
    return static_cast<int>(std::fmax(-100.0f, std::fmin(100.0f, value)));
}

} // namespace ts2::sim
