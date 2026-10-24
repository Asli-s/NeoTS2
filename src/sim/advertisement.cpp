#include "sim/advertisement.hpp"

#include <cstdint>

namespace ts2::sim {

namespace {

constexpr std::size_t kAgePersonData = 0x3A;
constexpr std::size_t kYoungAdultPersonData = 0xAF;

// For each advertisement type, the person data that scales its range (-1 for none) and whether it
// counts down from 1000 instead of up.
struct Scaling {
    int personData;
    bool inverted;
};
constexpr std::array<Scaling, 23> kScalings{{
    {-1, false}, {2, false}, {2, true}, {3, false}, {3, true}, {-1, false}, {-1, true},
    {5, false}, {5, true}, {6, false}, {6, true}, {7, false}, {7, true},
    {9, false}, {0xA, false}, {0xB, false}, {0xC, false}, {0xD, false}, {0xE, false},
    {0xF, false}, {0x10, false}, {0x11, false}, {0x12, false},
}};

bool advertises(std::size_t motive) {
    return motive < kMotiveCount && motive != 1 && motive != 2 && motive != 4 && (motive < 10 || motive > 12);
}

} // namespace

std::size_t advertisementGroup(const SimObject& sim) {
    const std::int16_t age = sim.personData[kAgePersonData];
    const bool adult = age > 18 || sim.personData[kYoungAdultPersonData] != 0;
    return age > 50 ? 4 : adult ? 0 : age > 15 ? 3 : age > 2 ? 1 : age == 2 ? 2 : 0;
}

float advertisedChange(const ttab::Interaction& interaction, const SimObject& sim, std::size_t motive) {
    const std::size_t group = advertisementGroup(sim);
    if (!advertises(motive) || group >= interaction.motives.size() || motive >= interaction.motives[group].size()) {
        return 0;
    }
    const ttab::MotiveEffect& e = interaction.motives[group][motive];
    if (e.type == 0) {
        return static_cast<float>(static_cast<std::int16_t>(e.min + e.delta)) * 0.001f;
    }
    // Types the game has no row for read past its table. Here they scale by 0.
    int value = 0;
    if (e.type > 0 && static_cast<std::size_t>(e.type) < kScalings.size()) {
        const Scaling s = kScalings[static_cast<std::size_t>(e.type)];
        if (s.personData >= 0 && static_cast<std::size_t>(s.personData) < sim.personData.size()) {
            value = sim.personData[static_cast<std::size_t>(s.personData)];
        }
        if (s.inverted) {
            value = 1000 - value;
        }
    }
    return (static_cast<float>(e.delta * value) * 0.001f + static_cast<float>(e.min)) * 0.001f;
}

std::array<float, kMotiveCount> advertisedMotives(const ttab::Interaction& interaction, const SimObject& sim,
                                                  const std::array<float, kMotiveCount>& motives) {
    std::array<float, kMotiveCount> after = motives;
    for (std::size_t m = 0; m < after.size(); ++m) {
        after[m] += advertisedChange(interaction, sim, m);
    }
    return after;
}

} // namespace ts2::sim
