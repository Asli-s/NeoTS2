#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
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

// The average colour of an image's opaque pixels (RGBA rows), summed as the game sums them;
// black when none is opaque.
std::array<float, 3> averageColor(std::span<const std::uint8_t> rgba);

enum class CoveringKind { Floor, Wall };

class CoveringColors {
public:
    // Reads the thumbnail cache (Thumbnails/BuildModeThumbnails.package in the user folder); an
    // empty or missing path gives grey for everything.
    explicit CoveringColors(const std::filesystem::path& thumbnails);

    // The colour for a covering name (a catalog GUID in hex); nullopt when no thumbnail is cached,
    // for which the game uses 0.5 grey.
    std::optional<std::array<float, 3>> color(CoveringKind kind, const std::string& name);

private:
    std::filesystem::path path_;
    std::map<std::pair<std::uint32_t, std::uint64_t>, std::size_t> index_;  // (type, instance pair) -> entry
    std::map<std::uint32_t, std::array<float, 3>> cache_;                   // by name hash, as the game keeps it
};

} // namespace ts2::content
