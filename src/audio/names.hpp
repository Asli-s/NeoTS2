#pragma once

// How a sound event's name becomes the name of the track that plays: the event's modifiers append
// suffixes in a fixed order, then an alias may replace the whole name.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace ts2::audio {

// The key a sound resource is filed under for a name, in the index's fields: instance
// 0xFF000000 | CRC24 and instance high CRC32, both of the lowercase name.
struct SoundKey {
    std::uint32_t instance = 0;
    std::uint32_t instanceHigh = 0;
    auto operator<=>(const SoundKey&) const = default;
};
SoundKey soundKey(std::string_view name);

// The modifier ids of the "events" property set (Misc.package), each the bit 1 << id of an event's mask.
namespace modifier {
constexpr int kDestSurface = 1;          // destsurfacetype: STR# 0x94 entry 0
constexpr int kSourceSurface = 2;        // sourcesurfacetype: STR# 0x94 entry 1
constexpr int kFirstSkill = 3;           // cleaningskill to logicskill, ids 3 to 12
constexpr int kLastSkill = 12;
constexpr int kObjectQuality = 14;       // objectquality: STR# 0x94 entry 2
constexpr int kObjectQualityEffect = 15;
constexpr int kDestFootstep = 16;
constexpr int kSourceFootstep = 17;
constexpr int kVox = 18;
constexpr int kStitcher = 19;
constexpr int kAlias = 20;
} // namespace modifier

// An event's mask from its comma list in the "events" set. Unknown words (such as lowlifescore)
// add nothing, as in the game.
std::uint32_t modifierMask(std::string_view list);

// The mask the game uses for an event: its own from the "events" set, the alias always, and the
// vox modifier for every name that starts with "vox".
std::uint32_t eventMask(std::string_view name, std::uint32_t listed);

// The person data the vox suffix reads, by number.
struct Speaker {
    std::int16_t age = 0;          // person data 0x3A, age in years
    std::int16_t gender = 0;       // person data 0x41, 0 male, 1 female
    std::int16_t ghostFlags = 0;   // person data 0x44, bit 0 a ghost
    std::int16_t voice = 0;        // person data 0x58, voice
    std::int16_t personData0xAF = 0;  // person data 0xAF, nonzero keeps an adult's voice at teen age
    std::int16_t species = 0;      // person data 0xBA, 0 human, 1 dog, 2 large dog, 3 cat
};

// The vox suffix for a speaker, such as "tfa" (teen, female, voice a), "ca" or "p".
std::string voxSuffix(const Speaker& speaker);

// What the modifiers read from the event's object. A modifier whose value is missing leaves the
// name as it is, as the game does when it cannot get its string.
struct NameInputs {
    std::optional<std::string> destSurface;    // STR# 0x94 entry 0
    std::optional<std::string> sourceSurface;  // STR# 0x94 entry 1
    std::optional<std::string> quality;        // STR# 0x94 entry 2
    std::optional<Speaker> speaker;            // set when the object is a person
};

// Aliases by the CRC32 of the lowercase full name, from the "aliases" property set.
using AliasMap = std::map<std::uint32_t, std::string>;
AliasMap aliasMap(const std::map<std::string, std::string>& aliases);

// The one track name an event resolves to: each modifier in its mask, lowest bit first, appends
// "_" and its value, then the alias replaces the full name when one is listed. There is no
// fallback to shorter names. The skill, quality effect, footstep and stitcher modifiers leave the
// name as it is (their sources are not known yet).
std::string trackName(std::string_view event, std::uint32_t mask, const NameInputs& inputs, const AliasMap& aliases);

} // namespace ts2::audio
