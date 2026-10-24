#include "sim/motive_update.hpp"

#include "sim/code_source.hpp"
#include "sim/motives.hpp"

#include <cmath>

namespace ts2::sim {

namespace {

constexpr std::int64_t kTicksPerUpdate = 60;
// The share of a day's rate one update adds.
constexpr float kRateShare = 1.0f / 720.0f;

constexpr std::size_t kTickCounter = 0x1B;
constexpr std::size_t kMotivesPaused = 0x1D;
constexpr std::size_t kTemperature = 0x14;
constexpr std::size_t kAge = 0x3A;
constexpr std::size_t kSpecies = 0xBA;
constexpr std::size_t kMoodLeftOut = 0x76;
constexpr std::size_t kMoodBonus = 0xA3;

// Motive, its rate's person data, and the person data slowing its decay (0: none).
struct Rate {
    std::size_t motive, rate, modifier;
};
constexpr std::array<Rate, 9> kRates{{
    {motive::kFun, 0x65, 0xE0},
    {motive::kHygiene, 0x61, 0xDF},
    {motive::kBladder, 0x5F, 0xDD},
    {motive::kSocial, 0x63, 0xE1},
    {motive::kHunger, 0x5D, 0xDB},
    {motive::kEnergy, 0x60, 0xDE},
    {motive::kComfort, 0x5E, 0xDC},
    {0, 0x64, 0},
    {1, 0x6A, 0},
}};

// The mood weights ("Happy Curve Data") by age, as STR# instances of the global group.
constexpr std::uint16_t kElderWeights = 0x226, kAdultWeights = 0x227, kTeenWeights = 0x228, kChildWeights = 0x229,
                        kToddlerWeights = 0x22A, kBabyWeights = 0x22B;
// The pets' weights by species (person data 0xBA 1-3): elder, adult, young.
constexpr std::uint16_t kPetWeights[3][3] = {{0x22C, 0x22D, 0x230}, {0x22E, 0x22F, 0x230}, {0x231, 0x232, 0x233}};

// Seasons' temperature limits: the temperatures (BCON 0x17F of the global group) and the mood
// limits (BCON 0x1017 of the temperature object).
constexpr std::uint32_t kSeasonsPack = 0x80;
constexpr std::uint16_t kTemperatureSteps = 0x17F, kMoodLimitSteps = 0x1017;
constexpr std::uint32_t kTemperatureObject = 0xD216EE01;

float kept(float value) {
    if (value > 100.0f) return 100.0f;
    if (value < -100.0f) return -100.0f;
    return value;
}

// 1 - modifier / 100, within 0..1 as the game works it out.
float slowing(std::int16_t modifier) {
    const float y = 1.0f - static_cast<float>(modifier) * 0.01f;
    const float low = (std::fabs(y) + y) * 0.5f;
    const float z = 1.0f - low;
    return 1.0f - (std::fabs(z) + z) * 0.5f;
}

std::uint16_t weightsFor(const SimObject& sim) {
    const std::int16_t age = sim.personData[kAge];
    const std::int16_t species = sim.personData[kSpecies];
    if (species == 0) {
        if (age > 50) return kElderWeights;
        if (age > 18) return kAdultWeights;
        if (age > 15) return kTeenWeights;
        if (age > 2) return kChildWeights;
        return age == 2 ? kToddlerWeights : kBabyWeights;
    }
    if (species < 1 || species > 3) {
        return kAdultWeights;
    }
    const auto& pets = kPetWeights[species - 1];
    return age > 50 ? pets[0] : age > 18 ? pets[1] : pets[2];
}

} // namespace

std::size_t motiveRateField(std::size_t motive) {
    for (const Rate& r : kRates) {
        if (r.motive == motive) {
            return r.rate;
        }
    }
    return 0;
}

void decayMotives(SimObject& sim) {
    for (const Rate& r : kRates) {
        float rate = sim.personData[r.rate];
        if (r.modifier != 0 && !(rate > 0)) {
            rate *= slowing(sim.personData[r.modifier]);
        }
        sim.motives[r.motive] = static_cast<float>(static_cast<double>(rate) * kRateShare + sim.motives[r.motive]);
    }
    for (float& m : sim.motives) {
        m = kept(m);
    }
}

void applyMotiveChanges(SimObject& sim) {
    for (const MotiveChange& change : sim.motiveChanges) {
        if (change.motive >= sim.motives.size()) {
            continue;
        }
        float& m = sim.motives[change.motive];
        if (change.perTick < 0) {
            if (m > change.limit) {
                m += change.perTick;
                if (m < change.limit) m = change.limit;
            }
        } else if (m < change.limit) {
            m += change.perTick;
            if (m > change.limit) m = change.limit;
        }
    }
}

float weightedMood(const ScoreCurves& weights, const SimObject& sim) {
    const std::int16_t leftOut = sim.personData[kMoodLeftOut];
    float sum = 0, total = 0;
    for (std::size_t m = 0; m < weights.size(); ++m) {
        if (m == 2 || m == 10 || m == 11 || m == 12 || (leftOut & (1 << m)) != 0) {
            continue;
        }
        const float level = motiveValue(sim, m);
        const float weight = weights[m](level);
        sum += level * weight;
        total += weight;
    }
    const float average = total != 0 ? sum / total : 0.0f;
    return static_cast<float>(sim.personData[kMoodBonus]) + average;
}

MotiveUpdate::MotiveUpdate(CodeSource& code, std::uint32_t installedPacks)
    : code_(code), temperatureLimits_((installedPacks & kSeasonsPack) == kSeasonsPack) {}

void MotiveUpdate::tick(SimObject& sim, std::int64_t time) {
    if (!sim.isPerson()) {
        return;
    }
    if (time >= sim.motivesUpdatedAt + kTicksPerUpdate) {
        sim.motivesUpdatedAt = time;
        if (sim.personData[kMotivesPaused] == 0) {
            decayMotives(sim);
            updateMood(sim);
        }
    }
    auto& counter = sim.personData[kTickCounter];
    counter = static_cast<std::int16_t>(counter + 1);
    if (sim.personData[kMotivesPaused] < 2) {
        applyMotiveChanges(sim);
    }
}

void MotiveUpdate::updateMood(SimObject& sim) {
    const ScoreCurves* weights = moodWeights(sim);
    if (weights == nullptr) {
        return;
    }
    float mood = kept(weightedMood(*weights, sim));
    if (temperatureLimits_) {
        loadTemperatureLimits();
        const std::int16_t temperature = sim.personData[kTemperature];
        for (std::size_t i = 0; i < temperatures_->size(); ++i) {
            if (temperature >= (*temperatures_)[i]) {
                if (static_cast<float>(moodLimits_[i]) < mood) {
                    mood = moodLimits_[i];
                }
                break;
            }
        }
    }
    sim.motives[motive::kMood] = mood;
    sim.motives[motive::kMood + 1] = kept(mood);
}

const ScoreCurves* MotiveUpdate::moodWeights(const SimObject& sim) {
    const std::uint16_t instance = weightsFor(sim);
    auto [at, added] = weights_.try_emplace(instance);
    if (added) {
        ScoreCurves curves;
        bool complete = true;
        for (std::size_t m = 0; m < curves.size() && complete; ++m) {
            const auto text = code_.string(kGlobalGroup, instance, m);
            complete = text.has_value();
            if (complete) {
                curves[m] = ScoreCurve::parse(*text);
            }
        }
        if (complete) {
            at->second = std::move(curves);
        }
    }
    return at->second ? &*at->second : nullptr;
}

// Entries the game can't find stay 0, as its tables start.
void MotiveUpdate::loadTemperatureLimits() {
    if (temperatures_) {
        return;
    }
    temperatures_.emplace();
    if (const auto* steps = code_.constants(kGlobalGroup, kTemperatureSteps)) {
        for (std::size_t i = 0; i < temperatures_->size() && i < steps->size(); ++i) {
            (*temperatures_)[i] = (*steps)[i];
        }
    }
    if (const ObjectType* object = code_.objectType(kTemperatureObject)) {
        if (const auto* limits = code_.constants(object->group, kMoodLimitSteps)) {
            for (std::size_t i = 0; i < moodLimits_.size() && i < limits->size(); ++i) {
                moodLimits_[i] = (*limits)[i];
            }
        }
    }
}

} // namespace ts2::sim
