#pragma once

#include "audio/decode.hpp"
#include "formats/dbpf.hpp"

#include <cstdint>
#include <filesystem>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ts2::audio {

// Which of the game's volume options a sound follows.
enum class Category : std::uint8_t { Effects, Voice, Music, Ambience };

// A track's settings (resource type 0x0B9EB87E, a property set whose keys are the crc24 of the
// names EA's HitLab tool gives them), with the game's defaults where a track leaves one out.
struct TrackSettings {
    int pitchHz = 0;               // "Pitch": the rate to play the clip at, 0 for its own
    float volume = 1;              // "VolumeInternal" / 1024
    bool is3d = false;             // "Is3d"
    float minDistance = 4;         // "MinimumDistance", in tiles
    float maxDistance = 32;        // "MaximumDistance"
    float rolloff = 1;             // "RolloffFactor"
    bool looped = false;           // "IsSampleLooped"
    int fadeInMs = 0, fadeOutMs = 100;
    int priority = 32;             // "Priority", 0-127
    float pan = 0;                 // "PanPosition", -1 left to 1 right (512 of 1024 is the middle)
    int pitchRandomMin = 0, pitchRandomMax = 0;  // "PitchRandomizeMin/Max", semitones
    int selfInterrupting = 0;      // "IsSelf-Interrupting" (1-3; what each means isn't known yet)
};

// A clip of a sound package, by its instance and high instance.
struct ClipKey {
    std::uint32_t instance = 0, instanceHigh = 0;
    bool operator==(const ClipKey&) const = default;
};

// A sound ready to play: its settings and one clip of its hit list.
struct PickedSound {
    std::string name;  // as asked
    TrackSettings track;
    Category category = Category::Effects;
    std::shared_ptr<const Pcm> pcm;
};

// Who a Sim's voice belongs to, for its "vox" sounds.
struct SimVoice {
    enum class Age { Baby, Toddler, Child, Teen, Adult, Elder };
    Age age = Age::Adult;
    bool female = false;
    char voice = 'a';  // adults have voices a and b, the others a only
};

// A script's vox sound is filed per kind of Sim: its name, "_", then the age (a adult, t teen, e
// elder, c child), the sex (f or m, not for children) and the voice letter: vox_soc_agree_meda for
// an adult woman with voice a is vox_soc_agree_meda_afa, for a child vox_soc_agree_meda_ca. The
// names to try in order, the closest first (babies' and toddlers' letters aren't known: they try p,
// b, then the child's).
std::vector<std::string> voiceNames(std::string_view name, const SimVoice& voice);
// Whether a sound name is a Sim's voice line (it begins with "vox").
bool isVoiceName(std::string_view name);

// The game's sounds, from every product's TSData/Res/Sound/*.package (later packs replace earlier
// ones' resources with the same key). A sound's name, lowercased after the game's alias table
// (property set 0xEBFEE33F 0x4C1940C5 0xFF1EBA43:0x3CF64D4B) has renamed it, gives the key of its
// track and hit list: instance 0xFF000000 | crc24(name), high instance crc32(name). Groups differ
// by package and aren't used.
//
// Thread-safe: the index is built once, and picks and clip reads lock.
class SoundBank {
public:
    explicit SoundBank(const std::filesystem::path& install);
    SoundBank(const SoundBank&) = delete;
    SoundBank& operator=(const SoundBank&) = delete;

    // A clip of the named sound, chosen as the game chooses from a hit list: at random among those
    // not played lately (see HitList). Nothing for a sound the packages don't have, as the game
    // stays silent for those, or a clip that doesn't decode.
    std::optional<PickedSound> pick(std::string_view name);
    // Whether the packages have a sound by this name.
    [[nodiscard]] bool has(std::string_view name) const;

    // A music station's songs: the clips filed under group 0xFF000000 | crc24(lowercase station name),
    // such as "splash" (the main menu), "nhood", "build", "buy", "cas", "pop" or "hiphop".
    [[nodiscard]] std::vector<ClipKey> station(std::string_view name) const;
    // A clip decoded (kept for a while, so playing it again doesn't decode it again).
    std::shared_ptr<const Pcm> clip(const ClipKey& key);

    [[nodiscard]] std::size_t packageCount() const { return packages_.size(); }
    [[nodiscard]] std::size_t clipCount() const { return clips_.size(); }
    [[nodiscard]] std::size_t trackCount() const { return tracks_.size(); }

    // The track settings a property set holds (exposed for tests).
    static TrackSettings readTrack(std::span<const std::uint8_t> propertySet);
    // A hit list's clips (resource type 0x7B1ACFCD): version 0x38 lists instance and high instance
    // pairs, version 1 instances only (exposed for tests).
    static std::vector<ClipKey> readHitList(std::span<const std::uint8_t> bytes);

    // The game's choice from a hit list: a random one of the clips not played lately. A played clip
    // waits in a queue of recent ones as long as `window` (a third of the list, plus one) before it
    // can come again.
    class HitList {
    public:
        explicit HitList(std::vector<ClipKey> clips);
        ClipKey choose(std::mt19937& random);

    private:
        std::vector<ClipKey> available_;
        std::vector<ClipKey> recent_;
        std::size_t window_ = 1;
    };

private:
    struct Where {
        std::uint16_t package = 0;
        dbpf::IndexEntry entry;
    };
    struct Package {
        std::filesystem::path path;
        Category category = Category::Effects;
        std::unique_ptr<dbpf::Package> file;
    };
    static std::uint64_t key(std::uint32_t instance, std::uint32_t high) { return std::uint64_t{instance} << 32 | high; }
    std::vector<std::uint8_t> read(const Where& where);

    std::vector<Package> packages_;
    std::unordered_map<std::uint64_t, Where> tracks_, hitLists_, clips_;
    // Version 1 hit lists name clips by instance only: the full key of the clip with each instance.
    std::unordered_map<std::uint32_t, std::uint64_t> clipByInstance_;
    std::unordered_map<std::uint32_t, std::vector<ClipKey>> stations_;  // music, by group
    std::unordered_map<std::string, std::string> aliases_;

    std::mutex mutex_;
    std::unordered_map<std::uint64_t, HitList> chosen_;
    std::mt19937 random_{std::random_device{}()};
    // Decoded clips, the latest used at the front, up to kCacheBytes of samples.
    std::list<std::pair<std::uint64_t, std::shared_ptr<const Pcm>>> cache_;
    std::size_t cacheBytes_ = 0;
    static constexpr std::size_t kCacheBytes = std::size_t{192} << 20;
};

} // namespace ts2::audio
