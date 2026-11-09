// High lot imposter pictures kept between runs, a file per lot in NeoTS2's cache folder (never in the
// saves).
#pragma once

#include "scene/scene.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace ts2::scene {

// What a lot's pictures were made from: its lot package as it was. Pictures made from another state
// of the package, or by another version of the bake, are made again.
struct ImposterCacheKey {
    std::uint64_t packageSize = 0;
    std::int64_t packageTime = 0;  // the package's last write, in the file clock's ticks
    bool operator==(const ImposterCacheKey&) const = default;
};

// The key of a lot package as it is now. Nullopt when it can't be read.
std::optional<ImposterCacheKey> imposterCacheKey(const std::filesystem::path& lotPackage);

// The file a lot's pictures are kept in: <cache>/<lot package's name>.neoimposter.
std::filesystem::path imposterCacheFile(const std::filesystem::path& cacheFolder, const std::filesystem::path& lotPackage);

// Reads a lot's pictures (DXT5 blocks, by texture name), when the file holds them for `key`. Nullopt
// when it is missing, made for something else, or broken.
std::optional<std::map<std::string, Image>> readImposterCache(const std::filesystem::path& file, const ImposterCacheKey& key);

// Writes a lot's pictures, each with its DXT5 blocks, replacing the file whole. False when it can't.
bool writeImposterCache(const std::filesystem::path& file, const ImposterCacheKey& key, const std::map<std::string, Image>& pictures);

} // namespace ts2::scene
