#pragma once

#include "formats/dbpf.hpp"
#include "formats/tga.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>

namespace ts2::content {

// The colour the game gives a floor or wall covering in its room light: the average of the
// covering's catalog thumbnail (the build mode thumbnails it caches in the user folder), over
// the pixels that are not transparent. The game uses 0.5 grey when it has no thumbnail.

// The game's name hashes (on the lower-cased name): a CRC-32 with polynomial 0x04C11DB7, no
// reflection and no final xor, and a CRC-24 with polynomial 0x864CFB from 0xB704CE.
std::uint32_t nameHash32(const std::string& name);
std::uint32_t nameHash24(const std::string& name);

// The average colour of an image's opaque pixels (RGBA rows), summed as the game sums them.
// Black when none is opaque.
std::array<float, 3> averageColor(std::span<const std::uint8_t> rgba);

enum class CoveringKind { Floor, Wall };

// A catalog picture for a covering with no cached one: 128 x 128 with the covering's texture seen
// straight on over pixels 10 to 118 each way, unlit and unchanged in colour, and a clear frame
// around it so the catalog button shows there. A wall's texture repeats three times across.
[[nodiscard]] tga::Pixels coveringThumbnail(const tga::Pixels& texture, CoveringKind kind);

class CoveringColors {
public:
    // Reads the thumbnail cache (Thumbnails/BuildModeThumbnails.package in the user folder). An
    // empty or missing path gives grey for everything.
    explicit CoveringColors(const std::filesystem::path& thumbnails);

    // The colour for a covering name (a catalog GUID in hex). Nullopt when no thumbnail is cached,
    // for which the game uses 0.5 grey.
    [[nodiscard]] std::optional<std::array<float, 3>> color(CoveringKind kind, const std::string& name);
    // The covering's cached catalog picture, no larger than `maxSide`. Nullopt when none is cached
    // (see coveringThumbnail for the one the game draws then).
    [[nodiscard]] std::optional<tga::Pixels> picture(CoveringKind kind, const std::string& name, std::uint32_t maxSide);

    // A fence or half wall's cached catalog picture (filed under its GUID), no larger than
    // `maxSide`. Nullopt when none is cached.
    [[nodiscard]] std::optional<tga::Pixels> fencePicture(std::uint32_t guid, std::uint32_t maxSide);
    // A fireplace's cached catalog picture (filed under its id in fireplaces.txt), no larger than
    // `maxSide`. Nullopt when none is cached.
    [[nodiscard]] std::optional<tga::Pixels> fireplacePicture(std::int32_t id, std::uint32_t maxSide);

    // Adds a covering picture the game would draw (see coveringThumbnail) to the cache file, as the
    // game files the ones it draws. False when the file is missing or can't be written.
    bool store(CoveringKind kind, const std::string& name, const tga::Pixels& picture);
    // True when the cache file was written since this read it, so a fresh read sees pictures added
    // since. The game never remembers a missing picture and finds a new one at its next look.
    [[nodiscard]] bool changedOnDisk() const;

private:
    // The colour from the thumbnail of this type, name and name hash.
    [[nodiscard]] std::optional<std::array<float, 3>> read(std::uint32_t type, const std::string& name, std::uint32_t low);
    // The thumbnail of this type and instance pair (high half first), decoded.
    [[nodiscard]] std::optional<tga::Pixels> cached(std::uint32_t type, std::uint64_t instance, std::uint32_t maxSide);
    // The thumbnail of this type, name and name hash, decoded.
    [[nodiscard]] std::optional<tga::Pixels> image(std::uint32_t type, const std::string& name, std::uint32_t low, std::uint32_t maxSide);

    std::filesystem::path file_;
    std::filesystem::file_time_type written_{};
    std::uintmax_t size_ = 0;
    std::optional<dbpf::Package> package_;  // kept open while the colours are looked up
    std::map<std::pair<std::uint32_t, std::uint64_t>, std::size_t> index_;  // (type, instance pair) -> entry
    std::map<std::uint32_t, std::array<float, 3>> cache_;                   // by name hash, as the game keeps it
    std::set<std::pair<std::uint32_t, std::uint32_t>> missing_;             // (type, name hash) with no colour
};

} // namespace ts2::content
