#include "turn.hpp"

#include <cmath>
#include <iterator>

namespace ts2::sim {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kQuarterPi = kPi / 4;
// Turns up to this far use no clip. Each further 45 degrees uses the next clip.
constexpr float kClipThreshold = 40.0f * kPi / 180.0f;
// Differences above this (just under half a turn) count as turning the other way.
constexpr float kLargestPositiveTurn = 2.9845130f;

// The name's start for a life stage (elders use the adults' clips), empty for babies.
const char* clipPrefix(std::int16_t lifeStage) {
    if (lifeStage <= 1) return "";
    if (lifeStage == 2) return "p-walk";
    if (lifeStage <= 15) return "c-";
    if (lifeStage <= 18) return "t-";
    return "a-";
}

// A turn from one heading to another in whole 45-degree steps of a clip (positive: towards
// positive headings), turns up to 40 degrees taking none.
int clipSteps(float start, float end) {
    float diff = std::remainder(end - start, 2 * kPi);
    if (diff > kLargestPositiveTurn) {
        diff -= 2 * kPi;
    }
    int steps = 0;
    while (diff < -kClipThreshold) {
        diff += kQuarterPi;
        --steps;
    }
    while (diff > kClipThreshold) {
        diff -= kQuarterPi;
        ++steps;
    }
    return steps;
}

// A walk's start ("Go") or stop clips. Only adults' (which teens and elders use), children's and
// toddlers' exist.
std::optional<WalkTurnClips> walkTurnClips(const char* kind, float from, float to, std::int16_t lifeStage) {
    if (lifeStage <= 1 || !std::isfinite(from) || !std::isfinite(to)) {
        return std::nullopt;
    }
    const int steps = clipSteps(from, to);
    const std::string angle = std::to_string(45 * std::abs(steps));
    const std::string age = lifeStage == 2 ? "p-walkTurnAnd" : lifeStage <= 15 ? "c-turnAnd" : "a-turnAnd";
    const std::string tail = lifeStage == 2 ? "-" + angle : "-alt-" + angle;
    // The clips turn towards negative headings.
    return WalkTurnClips{age + kind + tail, age + kind + "Short" + tail, steps > 0, static_cast<float>(steps) * kQuarterPi,
                         age + kind + "R" + tail, age + kind + "ShortR" + tail};
}

} // namespace

std::string walkClip(std::int16_t lifeStage, std::int16_t gender) {
    const std::string sex = gender == 1 ? "female-" : "male-";
    if (lifeStage <= 1) return "";
    if (lifeStage == 2) return "p-walk";
    if (lifeStage <= 15) return "c-walk-alt0-normal";
    if (lifeStage <= 18) return "t-" + sex + "walk-alt0-normal";
    return "a-" + sex + "walk-alt0-normal";
}

std::string walkShortClip(std::int16_t lifeStage, std::int16_t gender) {
    const std::string sex = gender == 1 ? "female-" : "male-";
    if (lifeStage <= 1) return "";
    if (lifeStage == 2) return "p-walkShort";
    if (lifeStage <= 15) return "c-walkShort-alt0-normal";
    if (lifeStage <= 18) return "t-" + sex + "walkShort-alt0-normal";
    return "a-" + sex + "walkShort-alt0-normal";
}

std::string walkStyleClip(std::int16_t lifeStage, std::int16_t gender, int style, bool shortOne) {
    static constexpr const char* kStyles[] = {"normal", "normal", "sexy", "pregnant", "happy", "depressed", "angry", "skip", "sneak", "", "hotsand"};
    if (style == 0 || lifeStage == 2) {
        return style != 0 ? "" : shortOne ? walkShortClip(lifeStage, gender) : walkClip(lifeStage, gender);
    }
    if (lifeStage <= 1 || style < 0 || style >= static_cast<int>(std::size(kStyles)) || *kStyles[style] == '\0') {
        return "";
    }
    const std::string sex = gender == 1 ? "female-" : "male-";
    const std::string prefix = lifeStage <= 15 ? "c-" : lifeStage <= 18 ? "t-" + sex : "a-" + sex;
    return prefix + (shortOne ? "walkShort" : "walk") + "-alt" + std::to_string(style) + "-" + kStyles[style];
}

std::string walkPoseClip(std::int16_t lifeStage) {
    if (lifeStage <= 1) return "";
    if (lifeStage == 2) return "p-walkPose";
    if (lifeStage <= 15) return "c-walkPose";
    return "a-walkPose";
}

float compassHeading(int direction) {
    return static_cast<float>(2 - direction) * kQuarterPi;
}

std::int16_t compassDirection(float heading) {
    const float h = heading - 2 * kPi * std::floor(heading / (2 * kPi));
    return static_cast<std::int16_t>(static_cast<int>(std::floor(2.5f - h / kQuarterPi)) & 7);
}

float wrapAngle(float angle) {
    angle = std::remainder(angle, 2 * kPi);
    return angle <= -kPi ? angle + 2 * kPi : angle;
}

float snapHeading(float heading) {
    return std::floor(heading / kQuarterPi + 0.5f) * kQuarterPi;
}

std::optional<TurnClip> turnClip(float start, float end, std::int16_t lifeStage) {
    const std::string prefix = clipPrefix(lifeStage);
    if (prefix.empty() || !std::isfinite(start) || !std::isfinite(end)) {
        return std::nullopt;
    }
    const int steps = clipSteps(start, end);
    if (steps == 0) {
        return std::nullopt;
    }
    // The clips turn towards negative headings.
    return TurnClip{prefix + "TurnInPlace-" + std::to_string(45 * std::abs(steps)), steps > 0, static_cast<float>(steps) * kQuarterPi};
}

std::optional<WalkTurnClips> walkStartClips(float from, float to, std::int16_t lifeStage) {
    return walkTurnClips("Go", from, to, lifeStage);
}

std::optional<WalkTurnClips> walkStopClips(float from, float to, std::int16_t lifeStage) {
    return walkTurnClips("Stop", from, to, lifeStage);
}

std::optional<WalkTurnClips> stepClips(float angle, std::int16_t lifeStage) {
    if (lifeStage <= 2 || !std::isfinite(angle)) {
        return std::nullopt;
    }
    const int steps = clipSteps(0, angle);
    const std::string prefix = lifeStage <= 15 ? "c-" : "a-";
    const std::string tail = "-" + std::to_string(45 * std::abs(steps));
    // The clips step towards negative headings.
    return WalkTurnClips{prefix + "step" + tail, prefix + "stepShort" + tail, steps > 0, static_cast<float>(steps) * kQuarterPi};
}

std::string stepInPlaceClip(std::int16_t lifeStage) {
    if (lifeStage <= 2) return "";
    return lifeStage <= 15 ? "c-stepInPlace" : "a-stepInPlace";
}

} // namespace ts2::sim
