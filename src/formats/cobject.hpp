#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// A lot object's saved scene state (resource type 0xFA1C39F7, instance = object id): its class
// chain (cPerson -> cLocomotable -> cAnimatable -> cObject, or a shorter one) with the meshes,
// joints and blends of cObject, then the animations cAnimatable was playing when the lot was saved.
namespace ts2::cobject {

// The class tag a Sim's record starts with. The game reads a record only through the reader of the
// object's own outermost class, and each class's reader stops at a record that starts with another.
constexpr std::uint32_t kPersonTag = 0xFA34F3EB;

// One animation an object was playing.
struct SavedAnimation {
    enum class Kind : std::uint32_t { Clip = 1, Attached = 2 };
    Kind kind = Kind::Clip;
    std::string name;
    std::string type;                   // "ovl", "ovm", "ovh", "ovu", "osl" (overlays), "sim", "lat"...
    std::optional<std::int32_t> category;  // its kind, as its list entry gives it (see content::animationCategory)
    std::int16_t eventObject = 0;       // the event tree's stack object
    std::uint16_t eventTree = 0;
    std::uint32_t eventObjectGuid = 0;  // the type whose code holds the event tree
    float rate = 1;                     // as Animate Sim's speed / 32 gives it (negative: backwards)
    std::int32_t state = 0;             // 1 playing
    std::int32_t layer = 0;
    std::int32_t plays = 1;             // -1: until stopped
    float weight = 1;
    std::optional<float> lengthMs;      // all of its plays, blends included, none without a blend curve
    float fadeInMs = 0, fadeOutMs = 0;  // its blend curve's rise at the start and fall at the end
    bool flipped = false;               // played mirrored
    std::int32_t startedMsAgo = 0;      // when the lot was saved (game ticks are milliseconds)
};

// A joint of the object's model as it was posed: its place relative to its parent, which replaces
// the model's own (a dining chair pushed under a table keeps its slid seat this way).
struct SavedJoint {
    std::string name;
    std::array<float, 3> translation{};
    std::array<float, 4> rotation{0, 0, 0, 1};  // x, y, z, w
};

struct SavedObject {
    std::vector<std::string> classes;   // outermost first
    std::uint32_t outerTag = 0;         // the outermost class's tag
    std::vector<SavedJoint> joints;     // left out of records older than version 0xF, whose layout isn't known
    std::vector<SavedAnimation> animations;
    // The object's place in the scene (position, then rotation as x, y, z, w), version 0xF and later,
    // and where in the record its floats start.
    std::optional<std::array<float, 7>> transform;
    std::size_t transformOffset = 0;
    // cAnimatable's own copy of a transform and where its floats start, and where its animation
    // count is (0 when the record has no cAnimatable).
    std::optional<std::array<float, 7>> animatableTransform;
    std::size_t animatableTransformOffset = 0, animationsOffset = 0;
};

// Throws std::runtime_error on data it cannot read.
SavedObject parse(std::span<const std::uint8_t> data);

} // namespace ts2::cobject
