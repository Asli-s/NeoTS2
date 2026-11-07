#include "sim/advertisement.hpp"

#include <cmath>
#include <cstdint>
#include <optional>

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

// An advertisement value times the multiplier, cut toward zero to its low 16 bits as the game's
// float-to-integer step does. A product too large for that step, or not a number, gives 0.
std::int16_t scaled(std::int16_t value, float multiplier) {
    const double product = static_cast<double>(value) * static_cast<double>(multiplier);
    if (!(std::fabs(product) < 9.0e18)) {
        return 0;
    }
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(static_cast<std::int64_t>(product) & 0xFFFF));
}

// Motives an "every motive" multiply leaves alone.
bool skippedByEveryMotive(std::size_t motive) {
    return motive == 2 || motive == 4 || (motive >= 10 && motive <= 12);
}

void multiplyHumanAd(ttab::Interaction& line, std::size_t group, std::size_t motive, float multiplier) {
    if (group < line.motives.size() && motive < line.motives[group].size()) {
        ttab::MotiveEffect& e = line.motives[group][motive];
        e.min = scaled(e.min, multiplier);
    }
}

// A pet's entry for a motive is a list of records, one per pair of personality bits (person data
// 0xC7, bits 0-1 for the first record). Each record keeps three values: the first is used with the
// pair's low bit set, the second with only its high bit, the third with neither. The multiply
// scales the first five records only.
void multiplyPetAd(ttab::Interaction& line, std::size_t group, std::size_t motive, std::int16_t personality,
                   float multiplier) {
    if (group >= line.moreMotives.size() || motive >= line.moreMotives[group].size()) {
        return;
    }
    auto& records = line.moreMotives[group][motive];
    for (std::size_t i = 0; i < records.size() && i < 5; ++i) {
        const unsigned low = 1u << (2 * i), high = 1u << (2 * i + 1);
        const auto bits = static_cast<unsigned>(static_cast<std::uint16_t>(personality));
        std::int16_t& value = (bits & low) ? records[i].type : (bits & high) ? records[i].min : records[i].delta;
        value = scaled(value, multiplier);
    }
}

// A pet's advertisement: the sum over every record of its kind's list for the motive of the value
// its personality picks (as multiplyPetAd picks it, with no limit on records), in thousandths.
// Motives 2, 4 and 10-13 advertise nothing to pets. Pair bits past 31 wrap round to bit 0, and bits
// past 15 are the sign of person data 0xC7, as the game reads them.
float petAdvertisedChange(const ttab::Interaction& interaction, const SimObject& sim, std::size_t motive) {
    constexpr std::size_t kPersonalityPersonData = 0xC7;
    if (motive >= kMotiveCount || motive == 2 || motive == 4 || (motive >= 10 && motive <= 13)) {
        return 0;
    }
    const auto kind = petKind(sim);
    if (!kind) {
        return 0;
    }
    const std::size_t group = *kind - kFirstPetKind;
    if (group >= interaction.moreMotives.size() || motive >= interaction.moreMotives[group].size()) {
        return 0;
    }
    const auto bits = static_cast<std::uint32_t>(static_cast<std::int32_t>(sim.personData[kPersonalityPersonData]));
    const auto bit = [bits](std::size_t n) { return ((bits >> (n & 31)) & 1u) != 0; };
    double sum = 0;
    const auto& records = interaction.moreMotives[group][motive];
    for (std::size_t i = 0; i < records.size(); ++i) {
        const ttab::MotiveEffect& r = records[i];
        sum += bit(2 * i) ? r.type : bit(2 * i + 1) ? r.min : r.delta;
    }
    return static_cast<float>(sum * static_cast<double>(0.001f));
}

} // namespace

void multiplyAdvertisements(ttab::Interaction& line, const SimObject& sim, float multiplier, std::int32_t motive) {
    constexpr std::size_t kSpeciesPersonData = 0xBA, kPersonalityPersonData = 0xC7;
    if (multiplier == 1.0f) {
        return;
    }
    const bool one = motive >= 0 && motive < static_cast<std::int32_t>(kMotiveCount);
    for (std::size_t m = 0; m < kMotiveCount; ++m) {
        if (one ? m != static_cast<std::size_t>(motive) : skippedByEveryMotive(m)) {
            continue;
        }
        if (sim.personData[kSpeciesPersonData] == 0) {
            multiplyHumanAd(line, advertisementGroup(sim), m, multiplier);
        } else if (const auto kind = petKind(sim)) {
            multiplyPetAd(line, *kind - kFirstPetKind, m, sim.personData[kPersonalityPersonData], multiplier);
        }
    }
}

std::optional<std::size_t> petKind(const SimObject& sim) {
    constexpr std::size_t kSpeciesPersonData = 0xBA;
    const std::int16_t age = sim.personData[kAgePersonData];
    const bool elder = age > 50, young = age <= 18;
    switch (sim.personData[kSpeciesPersonData]) {
    case 1: return elder ? 6 : young ? 11 : 5;
    case 2: return elder ? 8 : young ? 11 : 7;
    case 3: return elder ? 10 : young ? 12 : 9;
    default: return std::nullopt;
    }
}

std::size_t advertisementGroup(const SimObject& sim) {
    const std::int16_t age = sim.personData[kAgePersonData];
    const bool adult = age > 18 || sim.personData[kYoungAdultPersonData] != 0;
    return age > 50 ? 4 : adult ? 0 : age > 15 ? 3 : age > 2 ? 1 : age == 2 ? 2 : 0;
}

float advertisedChange(const ttab::Interaction& interaction, const SimObject& sim, std::size_t motive) {
    constexpr std::size_t kSpeciesPersonData = 0xBA;
    if (sim.personData[kSpeciesPersonData] != 0) {
        return petAdvertisedChange(interaction, sim, motive);
    }
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
