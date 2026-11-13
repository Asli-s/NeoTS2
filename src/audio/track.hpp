#pragma once

// A track's settings (the 0x0B9EB87E resource beside its hit list) and the sound tuning property
// sets in Misc.package, as the mixer uses them.

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ts2::audio {

// The track settings keys, each 0xFF000000 | CRC24 of the setting's name.
namespace track_key {
constexpr std::uint32_t kLooping = 0xFFF7B8DF;       // nonzero: the track loops
constexpr std::uint32_t kPositional = 0xFFCC156F;    // nonzero: it plays from its object's place
constexpr std::uint32_t kMinDistance = 0xFFAAB623;   // full volume up to here, in tiles
constexpr std::uint32_t kMaxDistance = 0xFFA6A614;   // silent past here, in tiles
constexpr std::uint32_t kPriority = 0xFF050A94;
constexpr std::uint32_t kVolume = 0xFF53DBF3;        // in 1/1024
constexpr std::uint32_t kVolume2 = 0xFF90AAC8;       // in 1/1024, multiplies the other
constexpr std::uint32_t kFadeOut = 0xFF595FFD;       // fade-out time in ms
constexpr std::uint32_t kPlayChance = 0xFFE1CF85;    // in percent, 0 always plays
constexpr std::uint32_t kInstanceMode = 0xFF303B1B;  // 2: one at a time per object
constexpr std::uint32_t kRolloffScale = 0xFF0150F2;  // scales the distance drop, 0 counts as 1
constexpr std::uint32_t kKeepPlaying = 0xFFFBD55D;   // nonzero: never stolen or cut off
} // namespace track_key

struct TrackSettings {
    bool looping = false;
    bool positional = false;
    float minDistance = 2;
    float maxDistance = 64;
    float priority = 100;
    float volume = 1;  // the two volume settings multiplied
    std::uint32_t fadeOutMs = 0;
    std::uint32_t playChance = 0;
    std::uint32_t instanceMode = 0;
    float rolloffScale = 1;
    bool keepPlaying = false;
};

// The settings from a track's property set, keyed by the setting keys written as hex numbers.
// Missing or unreadable values keep their defaults.
TrackSettings trackSettings(const std::map<std::string, std::string>& properties);

// One sample a hit list names, in the index's fields.
struct SampleKey {
    std::uint32_t instance = 0;
    std::uint32_t instanceHigh = 0;
};

// The samples of a hit list (0x7B1ACFCD): a version, a count, then each sample's instance and
// instance high. Empty for a list cut short. At most kMaxHitListSamples are read.
constexpr std::size_t kMaxHitListSamples = 4096;
std::vector<SampleKey> hitListSamples(std::span<const std::uint8_t> data);

// The sound groups as the game numbers them.
enum class Group : int { Sfx = 0, Voice = 1, Ambience = 2, Stereo = 3, Music = 4, Ui = 5, Cinematic = 6, Television = 7, Stitcher = 8 };
constexpr int kGroupCount = 9;

using GroupLimits = std::array<std::array<int, 3>, kGroupCount>;
constexpr GroupLimits noGroupLimits() {
    GroupLimits out{};
    for (auto& levels : out) {
        levels = {-1, -1, -1};
    }
    return out;
}

// The sound manager's tuning (Misc.package property set 0xFF11CE39) and the listener's
// (0xFFD94C49). Missing values keep the defaults below: no voice limit, no speed change.
struct Tuning {
    // Voices that may play at each performance level (0 low, 1 medium, 2 high), -1 for no limit.
    std::array<int, 3> maxSounds{-1, -1, -1};
    // The same per group, three levels for each group in group order.
    GroupLimits maxSoundsByGroup = noGroupLimits();
    // Pitch and volume by the simulator's speed index (2 normal, 3 fast, 4 fastest).
    std::array<float, 5> speedPitch{1, 1, 1, 1, 1};
    std::array<float, 5> speedVolume{1, 1, 1, 1, 1};
    float secondaryListenerInfluence = 0;
    float rolloffAspectRatio = 1;  // the up axis's share of the distance drop
    float attenuationPositionsScalar = 0;
    // Listener tuning, with the game's own defaults.
    float rolloff = 1.8f;
    float listenerMinHeight = 2;
    float listenerToCameraOffset = 0;
};

// Fills `tuning` from the sound manager's property set.
void readManagerTuning(const std::map<std::string, std::string>& properties, Tuning& tuning);
// Fills `tuning` from the listener's property set.
void readListenerTuning(const std::map<std::string, std::string>& properties, Tuning& tuning);

} // namespace ts2::audio
