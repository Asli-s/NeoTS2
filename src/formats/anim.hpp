#pragma once

#include <cstdint>
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
    // The value at `ms`; before the first key and after the last, theirs.
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
};

// Where an IK chain's end reaches: the skeleton node it goes to (and the node giving its
// orientation, when another), expressed in its own skeleton's space (kind 0) or a bone's.
// Names are hashes (hash::crc32 of the lowercase name); 0 for none.
struct IkTarget {
    std::uint8_t kind = 0;  // 0 own space, 1 a bone of its own, 2 a bone of another object
    std::uint32_t frame = 0, frameMirror = 0;
    std::uint32_t node = 0, rotationNode = 0;
    std::uint32_t weight = 0;  // a scalar channel blending the target in; 0: always fully on
};

// An IK chain: the joints from `begin` down to `end` are turned so that `end` reaches its target,
// bending toward the `pole` node. Each name comes with the one it has on the mirrored side.
struct IkChain {
    std::uint32_t name = 0, mirror = 0;
    std::uint32_t strategy = 0;  // kLimbStrategy, kLookAtStrategy
    std::uint32_t begin = 0, beginMirror = 0, end = 0, endMirror = 0, pole = 0;
    std::uint32_t poleFrame = 0, poleFrameMirror = 0;  // the bone the pole is expressed in; 0: own space
    std::uint32_t extra = 0;
    std::vector<IkTarget> targets;
};

// The game's IK strategies: hip/knee/ankle (or shoulder/elbow/wrist) limbs, and heads looking at
// something.
constexpr std::uint32_t kLimbStrategy = 0x48EE7A41;
constexpr std::uint32_t kLookAtStrategy = 0x09F9E87E;

// An animation (cAnimResourceConst, the game's memory image of it): the skeleton it is made for,
// its channels and IK chains. Only the first target (the skeleton) is kept.
struct Animation {
    std::string name;
    std::string skeleton;
    float duration = 0;  // milliseconds: the longest channel
    std::vector<Channel> channels;
    std::vector<IkChain> chains;
};

Animation parse(std::span<const std::uint8_t> data);

} // namespace ts2::anim
