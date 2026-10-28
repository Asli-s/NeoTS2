#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ts2::paths {

// NeoTS2's own save folders are named "The Sims 2 Neo - <name>", next to the original games' ones
// in "EA Games".
inline constexpr std::u8string_view kNeoSaveFolderPrefix = u8"The Sims 2 Neo - ";

struct NewSaveFolder {
    std::filesystem::path folder;  // the folder made, empty when none was
    std::string problem;           // why not, for the player
};

// Makes a new, empty NeoTS2 save folder in `parent` (the "EA Games" folder), named from what the
// player typed (UTF-8). An empty name takes the lowest number no folder there has yet. A name
// Windows can't use, or one a folder already has in any letter case, makes nothing.
NewSaveFolder makeNewSaveFolder(const std::filesystem::path& parent, std::string_view name);

// The NeoTS2 save folders already in `parent`, in name order.
std::vector<std::filesystem::path> neoSaveFolders(const std::filesystem::path& parent);

} // namespace ts2::paths
