#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace ts2::sim {

// A Sim's facing ("heading"): radians about the up axis, 0 facing the world's +y, growing towards
// the world's -x. Compass direction d (object data 1) faces (2 - d) * pi / 4.
float compassHeading(int direction);
// The compass direction nearest a heading, as the game rounds it.
std::int16_t compassDirection(float heading);
// Into (-pi, pi].
float wrapAngle(float angle);
// To the nearest multiple of 45 degrees: where a turn ends.
float snapHeading(float heading);

// Turn Body Towards doesn't turn when the target is within this angle.
constexpr float kNoTurnAngle = 3.14159265358979f / 8;
// Turn-in-place clips play this many times faster than authored: the game's fixed turn rate times
// the clip set's speed tag, 1 for the plain walk set's turns (walk styles with other tags aren't
// ported yet).
constexpr float kTurnInPlaceSpeed = 1.6f;

// The turn-in-place clip for a turn from one heading to another (snapped) heading, by life stage
// (person data 0x3A). Clips turn one way only. Turns the other way play them mirrored.
struct TurnClip {
    std::string name;
    bool mirrored = false;
    float turn = 0;  // the turn the played clip makes (positive towards positive headings)
};
[[nodiscard]] std::optional<TurnClip> turnClip(float start, float end, std::int16_t lifeStage);

// The clips a walk starts with (turning from the Sim's facing onto the path as it steps off) and
// stops with (turning from the way it walked to the facing it ends in), by life stage: the turn in
// 45-degree steps as for turns (none, the "-0" clip, up to 40 degrees), each in a full and a short
// length. They turn one way only, played mirrored the other way. `turn` is the turn the played clip
// makes (positive towards positive headings).
struct WalkTurnClips {
    std::string full;
    std::string shortOne;
    bool mirrored = false;
    float turn = 0;
    // The same turn's clips that begin on the other foot (none for no turn).
    std::string fullOtherFoot, shortOtherFoot;
};
[[nodiscard]] std::optional<WalkTurnClips> walkStartClips(float from, float to, std::int16_t lifeStage);
[[nodiscard]] std::optional<WalkTurnClips> walkStopClips(float from, float to, std::int16_t lifeStage);

// The step clips for a move `angle` off the Sim's facing (positive towards positive headings), by
// life stage: the angle in 45-degree steps as for turns (the "-0" step up to 40 degrees), each in a
// full and a short length. They step to one side only, played mirrored to the other. `turn` is the
// direction the played clip steps in, off the facing. Teens and elders take the adults' steps.
// Babies and toddlers have none.
[[nodiscard]] std::optional<WalkTurnClips> stepClips(float angle, std::int16_t lifeStage);
// The step on the spot that a step shorter than its short clip is mixed with (empty without one).
std::string stepInPlaceClip(std::int16_t lifeStage);

// The walk a Sim plays while it routes, and the pose it ends in, by life stage (person data 0x3A)
// and gender (0x41, 1 female): the plain walk of each age (elders take the adults' as for turns).
// Empty for babies.
std::string walkClip(std::int16_t lifeStage, std::int16_t gender);
// The short walk loop mixed with it to shorten its stride (empty for babies).
std::string walkShortClip(std::int16_t lifeStage, std::int16_t gender);
// Walk style `style`'s loop (its short one with `shortOne`), as the walk set names them: 0 and 1
// the two plain walks, 2 sexy, 3 pregnant, 4 happy, 5 depressed, 6 angry, 7 skip, 8 sneak, 10 hot
// sand. Toddlers have the plain walk only. Empty when the set has no such style.
std::string walkStyleClip(std::int16_t lifeStage, std::int16_t gender, int style, bool shortOne);
std::string walkPoseClip(std::int16_t lifeStage);

// A Turn Body Towards in progress (which instruction runs it, and its first steps).
struct Turning {
    std::size_t frame = 0;
    std::uint32_t group = 0;
    std::uint16_t tree = 0;
    std::uint16_t pc = 0;
    float start = 0;
    float end = 0;
    bool failsFalse = false;         // facing a point or object: false when the turn fails
    std::optional<bool> preRoute;    // what the Sim's pre-route tree returned, unset while it runs
};

} // namespace ts2::sim
