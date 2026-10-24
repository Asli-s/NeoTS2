#pragma once

#include "sim/happiness.hpp"
#include "sim/world.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>

namespace ts2::sim {

class CodeSource;

// The person data holding how far a motive moves in a Sim day, by motive, 0 for motives that have
// none. Set Motive Change can set it to 0.
std::size_t motiveRateField(std::size_t motive);

// Each motive moves by its rate (motiveRateField) times 1/720, the decaying ones (a rate of 0 or
// less) slowed by their modifier: rate x (1 - modifier / 100), the factor kept within 0..1. Then
// every motive is kept within -100..100.
void decayMotives(SimObject& sim);

// Each of the Sim's motive changes moves its motive by its rate per tick towards its limit, and no
// further than the limit. A motive already past its limit is left alone.
void applyMotiveChanges(SimObject& sim);

// The Sim's mood from its motives: the average of motives 0-15 (not 2, 10, 11, 12, nor those whose
// bit is set in person data 0x76), each weighted by its curve at its level, plus person data 0xA3.
// 0 plus 0xA3 when the weights add up to 0.
float weightedMood(const ScoreCurves& weights, const SimObject& sim);

// Keeps a Sim's motives moving while it lives. Called for each tick the Sim's script runs on
// (sleeping ticks too), it does three things in the game's order:
//  1. every 60 ticks, unless person data 0x1D is set: decay the motives, then work out the mood.
//  2. add one to the tick counter (person data 0x1B).
//  3. while person data 0x1D is below 2: move the motives by their motive changes.
class MotiveUpdate {
public:
    // `installedPacks` as nGameState.IsEPInstalled tests them. With bit 0x80 the temperature limits
    // the mood.
    MotiveUpdate(CodeSource& code, std::uint32_t installedPacks);

    // `time` is the simulation's tick count.
    void tick(SimObject& sim, std::int64_t time);

private:
    void updateMood(SimObject& sim);
    const ScoreCurves* moodWeights(const SimObject& sim);
    void loadTemperatureLimits();

    CodeSource& code_;
    bool temperatureLimits_ = false;
    std::map<std::uint16_t, std::optional<ScoreCurves>> weights_;
    // The temperature (person data 0x14) at and above which each mood limit applies, warmest first,
    // and the limits. Loaded the first time a mood is worked out.
    std::optional<std::array<std::int16_t, 5>> temperatures_;
    std::array<std::int16_t, 5> moodLimits_{};
};

} // namespace ts2::sim
