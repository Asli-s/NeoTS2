#include "sim/happiness.hpp"

#include "diag/log.hpp"
#include "formats/objd.hpp"
#include "sim/code_source.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

namespace ts2::sim {

namespace {

// A curve text longer than this many points is broken. The rest is left out.
constexpr std::size_t kMaxCurvePoints = 256;
// The longest point text read, "(x;y)" with its brackets.
constexpr std::size_t kMaxPointText = 64;

constexpr std::uint16_t kCustomCurvesField = 0x33;
constexpr std::size_t kCurveFlagsPersonData = 0x54;
constexpr std::int16_t kSpecialCurvesFlag = 0x200;
constexpr std::uint16_t kSpecialCurves = 0x298;
constexpr std::size_t kAgePersonData = 0x3A;
constexpr std::size_t kYoungAdultPersonData = 0xAF;
constexpr std::size_t kSpeciesPersonData = 0xBA;

// The global curves by age (the same age groups as the advertisements use).
constexpr std::uint16_t kElderCurves = 0x28A, kAdultCurves = 0x28B, kTeenCurves = 0x28C, kChildCurves = 0x28D,
                        kToddlerCurves = 0x28E, kBabyCurves = 0x28F;

// The motives a human Sim's happiness is the mean of.
constexpr std::array<std::size_t, 10> kHappinessMotives{0, 3, 5, 6, 7, 8, 9, 13, 14, 15};

std::uint16_t ageCurves(const SimObject& sim) {
    const std::int16_t age = sim.personData[kAgePersonData];
    if (age > 50) return kElderCurves;
    if (age > 18 || sim.personData[kYoungAdultPersonData] != 0) return kAdultCurves;
    if (age > 15) return kTeenCurves;
    if (age > 2) return kChildCurves;
    return age == 2 ? kToddlerCurves : kBabyCurves;
}

// Reads "(x;y)": a number right after the bracket (spaces allowed), ';' right after it, a number.
bool readPoint(const std::string& text, float& x, float& y) {
    const char* start = text.c_str() + 1;
    char* end = nullptr;
    x = std::strtof(start, &end);
    if (end == start || *end != ';') {
        return false;
    }
    start = end + 1;
    y = std::strtof(start, &end);
    return end != start;
}

} // namespace

ScoreCurve ScoreCurve::parse(std::string_view text) {
    ScoreCurve curve;
    std::size_t at = text.find('(');
    while (at != std::string_view::npos && curve.points_.size() < kMaxCurvePoints) {
        const std::size_t close = text.find(')', at);
        if (close == std::string_view::npos) {
            break;
        }
        const std::string point(text.substr(at, std::min(close - at + 1, kMaxPointText)));
        float x = 0, y = 0;
        if (readPoint(point, x, y) && std::isfinite(x) && std::isfinite(y)) {
            const auto place = std::lower_bound(curve.points_.begin(), curve.points_.end(), x,
                                                [](const auto& p, float v) { return p.first < v; });
            if (place == curve.points_.end() || place->first != x) {
                curve.points_.insert(place, {x, y});
            }
        }
        at = text.find('(', close);
    }
    if (curve.points_.size() >= kMaxCurvePoints && at != std::string_view::npos) {
        diag::warnOnce("happiness: a score curve past 256 points ignores the rest");
    }
    return curve;
}

float ScoreCurve::operator()(float x) const {
    if (points_.empty()) {
        return 0;
    }
    // Written so a NaN level takes the low end instead of falling between points.
    if (!(x > points_.front().first)) {
        return points_.front().second;
    }
    if (x >= points_.back().first) {
        return points_.back().second;
    }
    const auto next = std::upper_bound(points_.begin(), points_.end(), x,
                                       [](float v, const auto& p) { return v < p.first; });
    const auto& [x1, y1] = *next;
    const auto& [x0, y0] = *(next - 1);
    // In double, so points near the float limits can't overflow to NaN.
    const double t = (static_cast<double>(x) - x0) / (static_cast<double>(x1) - x0);
    return static_cast<float>(y0 + t * (static_cast<double>(y1) - y0));
}

std::optional<ScoreCurves> scoreCurves(CodeSource& code, const SimObject& sim, std::uint32_t simGroup) {
    if (sim.personData.size() <= kSpeciesPersonData || sim.personData[kSpeciesPersonData] != 0) {
        return std::nullopt;
    }
    std::uint32_t group = kGlobalGroup;
    std::uint16_t instance = ageCurves(sim);
    if ((sim.personData[kCurveFlagsPersonData] & kSpecialCurvesFlag) != 0) {
        instance = kSpecialCurves;
    } else if (const std::uint16_t custom = sim.definition.field(kCustomCurvesField); custom != 0) {
        group = simGroup;
        instance = custom;
    }
    ScoreCurves curves;
    for (std::size_t m = 0; m < curves.size(); ++m) {
        const auto text = code.string(group, instance, m);
        if (!text) {
            return std::nullopt;
        }
        curves[m] = ScoreCurve::parse(*text);
    }
    return curves;
}

std::array<float, kMotiveCount> scoredMotives(const SimObject& sim) {
    std::array<float, kMotiveCount> levels{};
    for (std::size_t m = 0; m < levels.size(); ++m) {
        levels[m] = sim.personData.size() > kEnvironmentScore ? motiveValue(sim, m) : sim.motives[m];
    }
    return levels;
}

float happiness(const ScoreCurves& curves, const std::array<float, kMotiveCount>& motives) {
    float sum = 0;
    for (const std::size_t m : kHappinessMotives) {
        sum += curves[m](motives[m]);
    }
    return sum * (1.0f / static_cast<float>(kHappinessMotives.size()));
}

} // namespace ts2::sim
