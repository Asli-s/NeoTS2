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
//   bytes 4-5  the tree run for the animation's events (0: none). Byte 7 says whose (0 private,
//              1 semi-global, 2 global)
//   byte 6     the animation list: the string table (0x81 adult animations, 0x86 objects...)
//   byte 8     more flags: 0x01 flip flag from temp 3, 0x02 sync to the last animation run,
//              0x10 the controlling object's list, 0x20 not hurryable
//   byte 9, bytes 10-11  the object used for IK (owner, data)
//   byte 12    priority (0 low, 1 medium, 2 high, 3 highest, above 3 taken as 3)
//   byte 13    blend, in hundredths
// Animate Object (0x69), Animate Overlay (0x6B) and Animate Stop (0x6C) start or stop an animation
// and carry on at once.
struct AnimateOperands {
    std::uint16_t entry = 0;
    bool entryFromParam = false;
    bool flipped = false;
    bool flipFromTemp = false;  // byte 8 flag 0x01: flipped when temp 3 isn't 0 (instead of byte 2's flag)
    bool speedFromTemp = false;
    std::int8_t speed = 32;
    std::uint16_t eventTree = 0;
    std::uint8_t eventTreeScope = 0;
    std::uint8_t list = 0;
    std::uint8_t priority = 0;
    std::int8_t blend = 0;
    // The clip's IK object: the value of byte 9 (owner) and bytes 10-11 (data), read when either isn't 0.
    std::uint8_t ikOwner = 0;
    std::uint16_t ikData = 0;
};
AnimateOperands animateOperands(std::span<const std::uint8_t> operands);

// A SimAntics event an animation marks ("event=N"): when it comes, in milliseconds of the clip as
// authored, and its number.
struct AnimationEvent {
    float timeMs = 0;
    int id = 0;
};

// An animation playing on an object, started by an Animate primitive.
struct PlayingAnimation {
    std::string name;
    double startMs = 0;   // on the world's clock
    float speed = 1;      // 1: as authored
    float lengthMs = 0;   // at that speed, 0 when its length isn't known (it ends at once)
    bool flipped = false;
    bool loops = false;   // plays over and over (a walk) instead of holding its last pose
    // Once played, an Animate Sim clip fades out over a quarter second, unless byte 2 flag 0x40 says
    // to drop it at once. Then it is gone, and the joints it drove keep their last pose.
    bool blendsOut = true;
    // A locomotion clip's base heading: its root and limb controls play turned to it (the heading
    // where a turn in place starts, the path's heading for a walk's clips). None for other clips.
    std::optional<float> baseHeading;
    // Where the clip's IK object stands, for its limb targets on that object (a reach clip's: the
    // object grabbed, or where the held one goes): route coordinates, level, height above that
    // level's floor and heading. None: such targets are in the Sim's own space.
    struct IkObject {
        float x = 0, y = 0;
        std::int16_t level = 1;
        float height = 0;
        float heading = 0;
    };
    std::optional<IkObject> ikObject;
    // The object Animate Sim names as the clip's IK object, whose bones its targets can name (a
    // hug's other Sim), 0 for none.
    std::int16_t ikTarget = 0;
    // The mixer layer it plays on: twice its priority (0 low .. 6 highest), between the overlays'
    // odd layers. Walks and turns play on 0.
    int layer = 0;
    std::uint8_t category = 1;  // see content::animationCategory
    // Where the primitive that started it is (so that it is that primitive that waits for it).
    std::size_t frame = 0;
    std::uint32_t group = 0;
    std::uint16_t tree = 0;
    std::uint16_t pc = 0;
    // Animate Sim's event tree (0: none), from the code owner of the tree that started it, with the
    // stack object it had then, and how many of the clip's events it has been told.
    std::uint16_t eventTree = 0;
    std::uint32_t eventCodeOwner = 0;
    std::int16_t eventStackObject = 0;
    std::size_t eventsTold = 0;

    bool finished(double nowMs) const { return nowMs - startMs >= lengthMs; }
};

// An animation laid over a Sim or an object by Animate Overlay (0x6B). It plays on its own while
// scripts go on. Its event tree hears when it starts blending out (kBlendingOutEvent), then when it
// is gone (kBlendedOutEvent and kEndEvent), with the percent of it played.
//   bytes 0-1  the entry of the animation list, or with byte 2 flag 0x04 the parameter holding it
//   byte 2     flags: 0x01 flipped, 0x04 entry from a parameter, 0x20 times to play from temp 1,
//              0x40 no blending out, 0x80 no blending in
//   byte 3     speed, in 32nds
//   bytes 4-5  the tree told of its events (0: none)
//   byte 6, 7-8  the object it plays on (owner, data), mostly "my object id". A bed's tree lays its
//              own clip on the bed this way while the Sim's clip plays on the Sim.
//   byte 9     the animation list (0x81 adult animations...)
//   byte 10    weight, in hundredths (outside 0-100: whole)
//   byte 11    priority 0-4 (low, medium, high, highest, under everything)
//   byte 15    flags: 0x01 flipped if temp 3 (in place of byte 2's flag), so a bed's clip plays on
//              the side the Sim gets in on, as the Sim's own clip does
struct PlayingOverlay {
    std::string name;
    double startMs = 0;   // on the world's clock
    float lengthMs = 0;   // at its speed, all its plays
    float speed = 1;
    bool flipped = false;
    std::int16_t plays = 1;          // -1: until stopped (it never ends by itself)
    float weight = 1;
    std::uint8_t priority = 0;
    bool blendsIn = true, blendsOut = true;
    std::uint16_t eventTree = 0;
    std::uint32_t codeOwner = 0;     // whose code the event tree is looked up in
    std::int16_t stackObject = 0;    // the event tree's stack object
    // Who ran Animate Overlay (0: the object playing it). The event tree runs as that object. Where
    // the game runs it when the overlay plays on another object is not known yet.
    std::int16_t caller = 0;
    bool blendingOut = false;        // kBlendingOutEvent told
    std::uint8_t category = 1;       // see content::animationCategory
    bool stopped = false;            // ended by Animate Stop: fades out without telling events
    // How long it fades in at its start and out before its end, when not kOverlayBlendMs.
    std::optional<float> blendInMs, blendOutMs;
};
constexpr std::int16_t kBlendingOutEvent = 9997, kBlendedOutEvent = 9998, kEndEvent = 9999;
// How long before its end an overlay starts blending out.
constexpr float kOverlayBlendMs = 250;

// How long an animation lasts as authored, in milliseconds, by name (without "_anim"), nullopt when
// the animation isn't known. Supplied by whoever has the game's animation files.
using AnimationLengths = std::function<std::optional<float>(const std::string& name)>;
// Whether an animation, by name, is a carry pose by its clip (a target of type 2): what an entry
// without a kind is (see content::animationCategory).
using AnimationCarries = std::function<bool(const std::string& name)>;
// How far an animation has turned its root about the up axis after some milliseconds as authored,
// in radians, nullopt when the animation isn't known.
using AnimationTurns = std::function<std::optional<float>(const std::string& name, float ms)>;

} // namespace ts2::sim
