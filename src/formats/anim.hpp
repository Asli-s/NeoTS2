#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ts2::anim {

constexpr std::uint32_t kType = 0xFB00791E;

// A key of one animated value: when (milliseconds), the value, and the slopes into and out of it,
// each already over the length of its segment (a cubic Hermite curve runs between keys).
struct Key {
    float time = 0;
    float value = 0;
    float in = 0, out = 0;
};

struct Curve {
    std::vector<Key> keys;
    // The value at `ms`. Before the first key and after the last, theirs.
    float sample(float ms) const;
};

// What a channel animates.
enum class ChannelKind : std::uint8_t { Scalar = 0, Rotation = 3, Translation = 4 };

// One animated property of a joint (by name): Euler rotations in degrees (x, then y, then z),
// translations, or a single value (a blend weight). A joint can have more than one.
struct Channel {
    std::string name;
    std::uint32_t flags = 0;
    std::vector<Curve> components;

    ChannelKind kind() const { return static_cast<ChannelKind>((flags >> 22) & 7); }
    // Without flag bit 15 the channel is added on top of what the other clips make of the joint
    // instead of being averaged with them (weight-shift overlays and the like).
    bool additive() const { return (flags & 0x8000) == 0; }
    // The IK chain whose joint the channel turns, as an index into Animation::chains, -1 for none.
    // Such a channel shares its joint with the chain's solve (see scene::channelShare).
    int chain() const {
        const int c = static_cast<int>((flags >> 25) & 15);
        return c == 15 ? -1 : c;
    }
};

// Where an IK chain's end reaches, as channels of the animation (they need not be skeleton joints):
// the position from `position`'s translation, the orientation from `node`'s rotation followed by
// `rotationNode`'s, expressed in its own skeleton's space (kind 0) or a bone's.
// Names are hashes (hash::crc32 of the lowercase name), 0 for none.
struct IkTarget {
    std::uint8_t kind = 0;  // 0 own space, 1 a bone of its own, 2 a bone of another object
    std::uint32_t frame = 0, frameMirror = 0;
    std::uint32_t node = 0, rotationNode = 0, position = 0;
    std::uint32_t weight = 0;  // a scalar channel blending the target in, 0: always fully on
};

// An IK chain: the joints from `begin` down to `end` are turned so that `end` reaches its target,
// bending toward the `pole` node. Each name comes with the one it has on the mirrored side.
struct IkChain {
    std::uint32_t name = 0, mirror = 0;
    std::uint32_t strategy = 0;  // kLimbStrategy, kLookAtStrategy
    std::uint32_t begin = 0, beginMirror = 0, end = 0, endMirror = 0, pole = 0;
    std::uint32_t poleFrame = 0, poleFrameMirror = 0;  // the bone the pole is expressed in, 0: own space
    std::uint32_t extra = 0;
    std::vector<IkTarget> targets;
};

// The game's IK strategies: hip/knee/ankle (or shoulder/elbow/wrist) limbs, and heads looking at
// something.
constexpr std::uint32_t kLimbStrategy = 0x48EE7A41;
constexpr std::uint32_t kLookAtStrategy = 0x09F9E87E;

// A moment the clip marks: a sound (type 1014), an effect (8) or a SimAntics event (1005, text
// "event=N..."), with its text.
struct Event {
    std::uint16_t timeMs = 0;
    std::uint16_t type = 0;
    std::string text;
};
constexpr std::uint16_t kSimAnticsEvent = 1005;

// The N of a SimAntics event's "event=N" line, or nullopt for other events.
std::optional<int> simAnticsEventId(const Event& event);

// An animation (cAnimResourceConst, the game's memory image of it): the skeleton it is made for,
// its channels, IK chains and events. Only the first target (the skeleton) is kept.
struct Animation {
    std::string name;
    std::string skeleton;
    float duration = 0;  // milliseconds, as the clip states it
    // The turn the clip plays at, off its Sim's frame (radians, the other way when mirrored), as its
    // header states: an entry into a seat from its side is authored a quarter turn round.
    float frameTurn = 0;
    // The type of each target (skeleton) the clip animates, as its header states.
    std::vector<std::uint16_t> targetTypes;
    // How far the clip carries its Sim over its length (tiles), as its header states: a walk is
    // authored in place and the game moves the Sim this far per loop.
    float travel = 0;
    // For a walk's start clip, the walk loop's phase (0-1) at the moment the clip ends: 0.5 when it
    // ends on the other foot.
    float loopPhase = 0;
    // A value the game's walk planner matches start, stop and loop clips by (its meaning isn't known yet).
    float match = 0;
    // For a walk loop, the speed it is made for, in tiles per millisecond.
    float speed = 0;
    // How far the clip turns its Sim, in radians (negative: towards negative headings).
    float rootTurn = 0;
    // The skeleton stretch the clip was made for, as its header states: 1 for adults, the teens'
    // 0.94 for teen clips.
    float bodyStretch = 1;
    std::vector<Channel> channels;
    std::vector<IkChain> chains;
    std::vector<Event> events;
};

Animation parse(std::span<const std::uint8_t> data);

} // namespace ts2::anim
