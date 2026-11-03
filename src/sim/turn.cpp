#include "turn.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <string_view>

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
    if (lifeStage > 50) return "e-" + sex + "walk-alt0-normal";
    return "a-" + sex + "walk-alt0-normal";
}

std::string walkShortClip(std::int16_t lifeStage, std::int16_t gender) {
    const std::string sex = gender == 1 ? "female-" : "male-";
    if (lifeStage <= 1) return "";
    if (lifeStage == 2) return "p-walkShort";
    if (lifeStage <= 15) return "c-walkShort-alt0-normal";
    if (lifeStage <= 18) return "t-" + sex + "walkShort-alt0-normal";
    if (lifeStage > 50) return "e-" + sex + "walkShort-alt0-normal";
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
    const std::string prefix = lifeStage <= 15 ? "c-" : lifeStage <= 18 ? "t-" + sex : lifeStage > 50 ? "e-" + sex : "a-" + sex;
    return prefix + (shortOne ? "walkShort" : "walk") + "-alt" + std::to_string(style) + "-" + kStyles[style];
}

std::string walkPoseClip(std::int16_t lifeStage) {
    if (lifeStage <= 1) return "";
    if (lifeStage == 2) return "p-walkPose";
    if (lifeStage <= 15) return "c-walkPose";
    if (lifeStage > 50) return "e-walkPose";
    return "a-walkPose";
}

std::string adultClipFor(const std::string& name) {
    return name.starts_with("e-") ? "a-" + name.substr(2) : "";
}

float compassHeading(int direction) {
    return static_cast<float>(2 - direction) * kQuarterPi;
}

std::int16_t compassDirection(float heading) {
    const float h = heading - 2 * kPi * std::floor(heading / (2 * kPi));
    return static_cast<std::int16_t>(static_cast<int>(std::floor(2.5f - h / kQuarterPi)) & 7);
}

std::int16_t octant(float dx, float dy) {
    constexpr float kTan22_5 = 0.41421354f;
    const float ax = std::fabs(dx);
    const float ay = std::fabs(dy);
    int direction = 2;
    if (ax * kTan22_5 <= ay) {
        direction = kTan22_5 * ay <= ax ? 3 : 4;
    }
    if (dx < 0) {
        direction = direction == 2 ? 6 : direction == 3 ? 5 : direction;
    }
    if (dy < 0) {
        direction = direction == 3 ? 1 : direction == 4 ? 0 : direction == 5 ? 7 : direction;
    }
    return static_cast<std::int16_t>(direction);
}

float drawnHeading(float heading) {
    return compassHeading(compassDirection(heading));
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

namespace {

bool contains(std::string_view text, std::string_view part) {
    const auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    return std::search(text.begin(), text.end(), part.begin(), part.end(), [&](char a, char b) { return lower(a) == lower(b); }) != text.end();
}

std::string_view trimmed(std::string_view text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

// The name without its "@" speed.
std::string_view withoutSpeed(std::string_view name) {
    return trimmed(name.substr(0, name.find('@')));
}

// Adds the clips one set name finds for a Sim (see walkSetClips), and says whether it found any.
bool addSetClips(std::string_view name, std::string_view prefix, std::string_view gender, std::string_view fallback,
                 const LocoClipLookup& clip, std::vector<LocoClip>& out) {
    constexpr int kAngles = 5, kAngleStep = 45;
    const bool turns = contains(name, "turn") || (contains(name, "step") && !contains(name, "stepinplace"));
    const bool alone = contains(name, "pose") || contains(name, "stepinplace");
    const auto nameWith = [&](std::string_view age, int angle) {
        std::string full(age);
        if (!turns && !alone) {
            full += gender;
        }
        full += name;
        if (turns) {
            full += "-" + std::to_string(angle);
        }
        return full;
    };
    bool found = false;
    for (int i = 0; i < (turns ? kAngles : 1); ++i) {
        auto c = clip(nameWith(prefix, i * kAngleStep));
        if (!c && !fallback.empty()) {
            c = clip(nameWith(fallback, i * kAngleStep));
        }
        if (c && c->category != 0 && (c->category != 100 || c->speed > 0)) {
            out.push_back(std::move(*c));
            found = true;
        }
    }
    return found;
}

// The first steps * 45 degrees turn of a category in the set, as authored (towards negative
// headings) or mirrored, from `from` on.
std::vector<LocoClip>::const_iterator turnOf(const std::vector<LocoClip>& set, std::vector<LocoClip>::const_iterator from,
                                             std::uint16_t category, int steps, bool& mirrored) {
    constexpr float kSame = 1e-3f;
    const float turn = static_cast<float>(steps) * kQuarterPi;
    return std::find_if(from, set.end(), [&](const LocoClip& c) {
        if (c.category != category) {
            return false;
        }
        if (std::abs(c.rootTurn + turn) < kSame) {
            mirrored = steps > 0;
            return true;
        }
        if (std::abs(c.rootTurn - turn) < kSame) {
            mirrored = steps < 0;
            return true;
        }
        return false;
    });
}

} // namespace

std::vector<LocoClip> walkSetClips(const WalkSetLines& lines, std::int16_t lifeStage, std::int16_t gender, const LocoClipLookup& clip) {
    std::vector<LocoClip> out;
    if (lifeStage <= 1 || !clip) {
        return out;
    }
    const std::string_view sex = gender == 1 ? "female-" : "male-";
    std::string_view prefix = "a-", fallback, withSex = sex;
    if (lifeStage == 2) {
        prefix = "p-";
        withSex = {};
    } else if (lifeStage <= 15) {
        prefix = "c-";
        withSex = {};
    } else if (lifeStage <= 18) {
        prefix = "t-";
        fallback = "a-";
    } else if (lifeStage > 50) {
        prefix = "e-";
        fallback = "a-";
    }
    for (const auto& line : lines) {
        std::string_view rest = line;
        while (!rest.empty()) {
            const auto comma = rest.find(',');
            const std::string_view item = rest.substr(0, comma);
            rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
            const auto open = item.find('(');
            const std::string_view first = withoutSpeed(item.substr(0, open));
            if (!first.empty() && addSetClips(first, prefix, withSex, fallback, clip, out)) {
                continue;
            }
            if (open != std::string_view::npos) {
                const std::string_view inside = item.substr(open + 1, item.find(')', open) - open - 1);
                if (const auto second = withoutSpeed(inside); !second.empty()) {
                    addSetClips(second, prefix, withSex, fallback, clip, out);
                }
            }
        }
    }
    return out;
}

std::optional<LocoClip> walkSetClip(const std::vector<LocoClip>& set, std::uint16_t category) {
    const auto it = std::find_if(set.begin(), set.end(), [&](const LocoClip& c) { return c.category == category; });
    return it == set.end() ? std::nullopt : std::optional<LocoClip>(*it);
}

std::optional<LocoClip> walkSetLongClip(const std::vector<LocoClip>& set, std::uint16_t category) {
    const auto of = [&](const LocoClip& c) { return c.category == category; };
    const auto first = std::find_if(set.begin(), set.end(), of);
    const auto second = first == set.end() ? set.end() : std::find_if(std::next(first), set.end(), of);
    return second == set.end() ? std::nullopt : std::optional<LocoClip>(*second);
}

std::string settlePose(const WalkSetLines* pushed, std::int16_t lifeStage, std::int16_t gender, const LocoClipLookup& clip) {
    if (pushed == nullptr) {
        return walkPoseClip(lifeStage);
    }
    const auto pose = walkSetClip(walkSetClips(*pushed, lifeStage, gender, clip), kPoseCategory);
    return pose ? pose->name : std::string();
}

std::optional<TurnClip> walkSetTurn(const std::vector<LocoClip>& set, float start, float end) {
    constexpr std::uint16_t kTurnInPlaceCategory = 128;
    if (!std::isfinite(start) || !std::isfinite(end)) {
        return std::nullopt;
    }
    const int steps = clipSteps(start, end);
    bool mirrored = false;
    const auto it = steps == 0 ? set.end() : turnOf(set, set.begin(), kTurnInPlaceCategory, steps, mirrored);
    if (it == set.end()) {
        return std::nullopt;
    }
    return TurnClip{it->name, mirrored, static_cast<float>(steps) * kQuarterPi};
}

std::optional<WalkTurnClips> walkSetTurnClips(const std::vector<LocoClip>& set, std::uint16_t category, float from, float to) {
    if (!std::isfinite(from) || !std::isfinite(to)) {
        return std::nullopt;
    }
    const int steps = clipSteps(from, to);
    std::array<std::string, 4> names;
    bool mirrored = false;
    auto at = set.begin();
    for (auto& name : names) {
        bool thisMirrored = false;
        at = turnOf(set, at, category, steps, thisMirrored);
        if (at == set.end()) {
            break;
        }
        if (&name == names.data()) {
            mirrored = thisMirrored;
        }
        name = at->name;
        ++at;
    }
    if (names[0].empty()) {
        return std::nullopt;
    }
    return WalkTurnClips{names[1], names[0], mirrored, static_cast<float>(steps) * kQuarterPi, names[3], names[2]};
}

} // namespace ts2::sim
