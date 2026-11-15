#pragma once

// The install's sound resources: track settings, hit lists, samples and the sound property sets,
// indexed once in the game's package order.

#include "audio/names.hpp"
#include "audio/track.hpp"
#include "formats/dbpf.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ts2::audio {

// Where a resource is: its package's place in the load order and its index entry.
struct Location {
    std::uint32_t package = 0;
    dbpf::IndexEntry entry;
};
// Resources of one type by instance and instance high. The group is not part of the key.
using KeyMap = std::unordered_map<std::uint64_t, Location>;
constexpr std::uint64_t mapKey(std::uint32_t instance, std::uint32_t instanceHigh) {
    return static_cast<std::uint64_t>(instanceHigh) << 32 | instance;
}

// Adds one package's resources of `type` as the game fills its sound maps: the package's index
// from its last entry to its first, each key kept only when no earlier package or later entry
// holds it. So the first package wins, and inside a package the last entry.
void addSegment(KeyMap& map, std::uint32_t package, std::span<const dbpf::IndexEntry> entries, std::uint32_t type);

// The sound packages in the order the game fills its maps: first the packages each pack's
// TSAudioPacked*.ini names, newest pack first and in each in the file's order, then every other
// package in a TSData/Res/Sound folder, newest pack first and by name.
std::vector<std::filesystem::path> soundPackageOrder(const std::filesystem::path& installRoot);

class Bank {
public:
    struct Track {
        TrackSettings settings;
        std::uint32_t firstSample = 0;
        std::uint32_t sampleCount = 0;
    };

    // Indexes the install's sound packages and reads every track's settings and hit list. Slow:
    // run it off the main thread. Packages that fail to open are logged and left out.
    void load(const std::filesystem::path& installRoot);

    // The track filed under a key: the exact key, or with instance high 0 the first track with
    // that instance. None when there is neither a track nor a hit list of that key.
    [[nodiscard]] const Track* track(SoundKey key) const;
    [[nodiscard]] std::span<const SampleKey> samples(const Track& track) const;
    // A sample's bytes, decompressed. Empty when it is not there or cannot be read. Not thread
    // safe: one thread reads samples once loading is done.
    [[nodiscard]] std::vector<std::uint8_t> readSample(SampleKey key);

    // An event's modifiers as the "events" property set lists them, 0 when it lists none.
    [[nodiscard]] std::uint32_t listedMask(std::string_view event) const;
    [[nodiscard]] const AliasMap& aliases() const { return aliases_; }
    [[nodiscard]] const Tuning& tuning() const { return tuning_; }
    [[nodiscard]] std::size_t trackCount() const { return tracks_.size(); }

private:
    [[nodiscard]] std::vector<std::uint8_t> read(const Location& location);
    [[nodiscard]] std::map<std::string, std::string> propertySet(SoundKey key);
    void readTracks(const KeyMap& definitions, const KeyMap& hitLists);
    void readPropertySets();

    std::vector<std::unique_ptr<dbpf::Package>> packages_;
    KeyMap samples_;
    KeyMap propertySets_;
    std::unordered_map<std::uint64_t, Track> tracks_;
    // The first track of each instance, for keys with instance high 0.
    std::unordered_map<std::uint32_t, std::uint64_t> firstOfInstance_;
    std::vector<SampleKey> sampleLists_;
    std::unordered_map<std::uint64_t, std::uint32_t> eventMasks_;
    AliasMap aliases_;
    Tuning tuning_;
};

} // namespace ts2::audio
