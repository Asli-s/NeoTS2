#pragma once

#include "sim/motives.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace ts2::sim {

class CodeSource;
struct SimObject;

// How much a motive's level adds to a Sim's happiness, for autonomy: points "(x;y)" in a text, x a
// motive level (-100 to 100). Between points the value is linear. Outside them it stays at the end
// point's value. Points come sorted by x whatever the text's order, and a repeated x is ignored.
class ScoreCurve {
public:
    static ScoreCurve parse(std::string_view text);
    float operator()(float x) const;
    const std::vector<std::pair<float, float>>& points() const { return points_; }

private:
    std::vector<std::pair<float, float>> points_;
};

// A Sim's curves, one per motive (string N of the STR# is motive N).
using ScoreCurves = std::array<ScoreCurve, kMotiveCount>;

// The curves a Sim scores interactions with: STR# 0x298 when bit 0x200 of its person data 0x54 is
// set, else the STR# its object definition names in field 0x33, from its own group, else the global
// curves for its age. Nothing for pets (their curves are not done yet) or a missing table.
[[nodiscard]] std::optional<ScoreCurves> scoreCurves(CodeSource& code, const SimObject& sim, std::uint32_t simGroup);

// The motive levels autonomy scores with: the Sim's motives, with motive 13 (environment) replaced
// by person data 0x79 when that is not 0.
std::array<float, kMotiveCount> scoredMotives(const SimObject& sim);

// A human Sim's happiness for these motive levels: the mean of its curves over motives 0, 3, 5-9 and
// 13-15. Find Best Interaction compares it before and after an interaction's advertisements.
float happiness(const ScoreCurves& curves, const std::array<float, kMotiveCount>& motives);

} // namespace ts2::sim
