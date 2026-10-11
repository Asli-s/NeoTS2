#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ts2::sim {

// The Animate primitives' operands, as the game reads them.
//
// Animate Sim (0x6A) plays an animation on the Sim running it and waits until it ends:
//   bytes 0-1  animation: an entry of the animation list, or (flag 0x04) the parameter holding it
//   byte 2     flags: 0x01 flipped, 0x02 speed from temp 2, 0x04 entry from a parameter,
//              0x08 interruptible, 0x10 start at the tag in temp 0, 0x20 back to idle after,
//              0x40 no blend out, 0x80 no blend in
//   byte 3     speed, in 32nds (32 = as authored)
//   bytes 4-5  the tree run for the animation's events (0: none); byte 7 says whose (0 private,
//              1 semi-global, 2 global)
//   byte 6     the animation list: the string table (0x81 adult animations, 0x86 objects...)
//   byte 8     more flags: 0x01 flip flag from temp 3, 0x02 sync to the last animation run,
//              0x10 the controlling object's list, 0x20 not hurryable
//   byte 9, bytes 10-11  the object used for IK (owner, data)
//   byte 12    priority (0 low, 1 medium, 2 high)
//   byte 13    blend, in hundredths
// Animate Object (0x69), Animate Overlay (0x6B) and Animate Stop (0x6C) start or stop an animation
// and carry on at once.
struct AnimateOperands {
    std::uint16_t entry = 0;
    bool entryFromParam = false;
    bool flipped = false;
    bool speedFromTemp = false;
    std::int8_t speed = 32;
    std::uint16_t eventTree = 0;
    std::uint8_t eventTreeScope = 0;
    std::uint8_t list = 0;
    std::uint8_t priority = 0;
    std::int8_t blend = 0;
};
AnimateOperands animateOperands(std::span<const std::uint8_t> operands);

// An animation playing on an object, started by an Animate primitive.
struct PlayingAnimation {
    std::string name;
    double startMs = 0;   // on the world's clock
    float speed = 1;      // 1: as authored
    float lengthMs = 0;   // at that speed; 0 when its length isn't known (it ends at once)
    bool flipped = false;
    // Where the primitive that started it is (so that it is that primitive that waits for it).
    std::size_t frame = 0;
    std::uint32_t group = 0;
    std::uint16_t tree = 0;
    std::uint16_t pc = 0;

    bool finished(double nowMs) const { return nowMs - startMs >= lengthMs; }
};

// How long an animation lasts as authored, in milliseconds, by name (without "_anim"); nullopt when
// the animation isn't known. Supplied by whoever has the game's animation files.
using AnimationLengths = std::function<std::optional<float>(const std::string& name)>;

} // namespace ts2::sim
