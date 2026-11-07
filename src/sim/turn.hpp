#pragma once

#include "sim/walk_plan.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ts2::sim {

// A Sim's facing ("heading"): radians about the up axis, 0 facing the world's +y, growing towards
// the world's -x. Compass direction d (object data 1) faces (2 - d) * pi / 4.
float compassHeading(int direction);
// The compass direction nearest a heading, as the game rounds it.
std::int16_t compassDirection(float heading);
// One of 8 directions for a vector, split at 22.5 degrees from each axis: 0 is -y, 2 +x, 4 +y,
// 6 -x, with the diagonals between.
std::int16_t octant(float dx, float dy);
// The heading a standing or walking Sim is drawn at: the compass direction nearest its heading, an
// exact half going to the lower one. The game turns a Sim's drawn place only in eighth turns, and
// its walk and turn clips play turned by the rest, so the body still faces its own heading.
float drawnHeading(float heading);
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
// and gender (0x41, 1 female): the plain walk of each age. Elders have their own walks but take the adults' turns.
// Empty for babies.
std::string walkClip(std::int16_t lifeStage, std::int16_t gender);
// The short walk loop mixed with it to shorten its stride (empty for babies).
std::string walkShortClip(std::int16_t lifeStage, std::int16_t gender);
// Walk style `style`'s loop (its short one with `shortOne`), as the walk set names them: 0 and 1
// the two plain walks, 2 sexy, 3 pregnant, 4 happy, 5 depressed, 6 angry, 7 skip, 8 sneak, 10 hot
// sand. Toddlers have the plain walk only. Empty when the set has no such style.
std::string walkStyleClip(std::int16_t lifeStage, std::int16_t gender, int style, bool shortOne);
// The pose a walk settles into. Elders have their own, teens use the adults'.
std::string walkPoseClip(std::int16_t lifeStage);
// The adults' clip an elder's clip ("e-...") stands in for when the game has no elder one, as the
// walk set falls back. Empty for any other name.
std::string adultClipFor(const std::string& name);

// A walk set as Assign Locomotion Animations pushes it from a string table (STR# 0x84): three lines
// of comma-separated clip names. A name may carry "@" and a speed (not used yet), and a name in
// brackets after it stands in when it finds no clip.
using WalkSetLines = std::array<std::string, 3>;
using LocoClipLookup = std::function<std::optional<LocoClip>(const std::string& name)>;

// The clips a walk set names for a Sim of a life stage and gender, in the set's order, as the game
// finds them. Turns and steps are looked for at 0, 45, 90, 135 and 180 degrees ("swim-TurnInPlace-90"),
// poses and the step in place by name alone, and the rest with the gender ("a-male-swimSlow"). Each
// name takes the age's prefix, and elders and teens fall back to the adults' clips. A clip of
// category 0, or of category 100 without a speed, doesn't count.
[[nodiscard]] std::vector<LocoClip> walkSetClips(const WalkSetLines& lines, std::int16_t lifeStage, std::int16_t gender,
                                                 const LocoClipLookup& clip);
// The set's first clip of a category (see anim::Animation::category).
[[nodiscard]] std::optional<LocoClip> walkSetClip(const std::vector<LocoClip>& set, std::uint16_t category);
// The set's second clip of a category, the long one a walk loop's first is mixed with.
[[nodiscard]] std::optional<LocoClip> walkSetLongClip(const std::vector<LocoClip>& set, std::uint16_t category);
// The pose a walk settles into: the newest pushed walk set's (`pushed`, its walk lines), else
// walkPoseClip's. Empty when there is none.
[[nodiscard]] std::string settlePose(const WalkSetLines* pushed, std::int16_t lifeStage, std::int16_t gender, const LocoClipLookup& clip);
// As turnClip, from the set's turns in place.
[[nodiscard]] std::optional<TurnClip> walkSetTurn(const std::vector<LocoClip>& set, float start, float end);
// As walkStartClips, walkStopClips and stepClips, from the set's clips of a category (96 starts, 112
// stops, 144 steps) that turn as far: the first is the short one, then the full one, then the two
// that begin on the other foot. None when the set has no clip for that turn.
[[nodiscard]] std::optional<WalkTurnClips> walkSetTurnClips(const std::vector<LocoClip>& set, std::uint16_t category, float from, float to);

constexpr std::uint16_t kPoseCategory = 16, kStartCategory = 96, kStopCategory = 112, kStepCategory = 144,
                        kStepInPlaceCategory = 160, kLoopCategory = 176;
// Each walk style's loops have their own category, 16 on from the style before (style 1's are 192).
constexpr std::uint16_t kLoopStyleStep = 16;

// A Turn Body Towards in progress (which instruction runs it, and its first steps).
struct Turning {
    std::size_t frame = 0;
    std::uint32_t group = 0;
    std::uint16_t tree = 0;
    std::uint16_t pc = 0;
    std::uint16_t mode = 0;
    std::int16_t direction = 0;      // modes 25 and 26: the compass direction picked when the turn started
    float start = 0;
    float end = 0;
    bool failsFalse = false;         // facing a point or object: false when the turn fails
    std::optional<bool> preRoute;    // what the Sim's pre-route tree returned, unset while it runs
    std::optional<bool> turned;      // set once the turn has ended (false when it failed) and the post-route tree runs
    bool postRouteReturned = false;
};

} // namespace ts2::sim
