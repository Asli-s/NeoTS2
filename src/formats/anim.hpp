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

// An animation (cAnimResourceConst, the game's memory image of it): the skeleton it is made for
// and its channels. Only the first target (the skeleton) is kept.
struct Animation {
    std::string name;
    std::string skeleton;
    float duration = 0;  // milliseconds: the longest channel
    std::vector<Channel> channels;
};

Animation parse(std::span<const std::uint8_t> data);

} // namespace ts2::anim
