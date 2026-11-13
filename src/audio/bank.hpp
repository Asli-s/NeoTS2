#pragma once

#include "content/resource_index.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace ts2::audio {

// One hit-list entry: which sample resource a track picks from.
struct SampleLocation {
    std::uint32_t group = 0;
    std::uint32_t instance = 0;
    std::uint32_t instanceHigh = 0;
};

// The install's sound resources: track settings, hit lists and samples, read through one index.
// Adding an install twice does nothing.
class Bank {
public:
    void addInstall(const std::filesystem::path& installRoot);

    [[nodiscard]] bool empty() const { return tracks_.empty() && samples_.empty(); }
    // Whether a track of that name exists (the name is the event's, not yet suffixed).
    [[nodiscard]] bool hasTrack(std::string_view trackName) const;
    // The samples the track's hit list names, empty when the track or its list is missing.
    [[nodiscard]] std::vector<SampleLocation> samplesOf(std::string_view trackName);
    // Whether the track's settings say it loops.
    [[nodiscard]] bool loops(std::string_view trackName);
    // A sound property set by its name ("aliases", "events", ...), empty when it is missing.
    [[nodiscard]] std::map<std::string, std::string> propertySet(std::string_view trackName);
    // The resource bytes of a sample, decompressed by the index. Empty when it is not there.
    [[nodiscard]] std::vector<std::uint8_t> read(const SampleLocation& location);

private:
    using Key = std::uint64_t;
    static Key keyOf(std::uint32_t instance, std::uint32_t high) {
        return static_cast<Key>(instance) | static_cast<Key>(high) << 32;
    }
    // The resource of a name, trying the full key and then the instance alone: packages whose index
    // has no instance high file their resources with 0 there.
    [[nodiscard]] const content::ResourceIndex::Resource* find(const std::map<Key, std::vector<const content::ResourceIndex::Resource*>>& map,
                                               std::string_view trackName) const;
    [[nodiscard]] const content::ResourceIndex::Resource* findSample(const SampleLocation& location) const;

    void addPackages(const std::filesystem::path& folder);
    void indexAll();

    content::ResourceIndex index_;
    std::set<std::string> packages_;
    std::map<Key, std::vector<const content::ResourceIndex::Resource*>> tracks_;
    std::map<Key, std::vector<const content::ResourceIndex::Resource*>> hitLists_;
    std::map<Key, std::vector<const content::ResourceIndex::Resource*>> samples_;
    std::map<Key, std::vector<const content::ResourceIndex::Resource*>> propertySets_;
};

} // namespace ts2::audio
