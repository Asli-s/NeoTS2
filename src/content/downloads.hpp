#pragma once

#include "content/resource_index.hpp"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace ts2::content {

// The .package files in a save folder in the game's load order. The game walks the whole folder, not
// just Downloads, so Body Shop parts in SavedSims count too. Files and folders sort together by name,
// ignoring case. Later packages override earlier ones and all override the install. Characters, Lots,
// LotCatalog, Thumbnails, .files and .sgfiles folders are left out at any depth.
[[nodiscard]] std::vector<std::filesystem::path> downloadPackages(const std::filesystem::path& saveFolder);

// The group of a package's own resources, those saved in group 0xFFFFFFFF: from its file name up
// to the first dot, ignoring case. So a mod named UIText.package files under the game's UIText group.
[[nodiscard]] std::uint32_t packageNameGroup(const std::filesystem::path& package);

// Chooses a save folder's Downloads for addDownloads, reading only the packages that changed since
// `indexFile` was saved, and saving it again if any did. Each package's index is read here once,
// however many content indexes take it. No file is kept when `indexFile` is empty. Call before
// anything loads on another thread.
void loadDownloads(const std::filesystem::path& saveFolder, const std::filesystem::path& indexFile);

// Chooses these packages for addDownloads, reading each.
void useDownloads(const std::vector<std::filesystem::path>& packages);

// Adds the chosen packages after what `index` holds, so they override it. A package that can't be
// read is left out, and logged when first read. Returns how many were added.
std::size_t addDownloads(ResourceIndex& index);

} // namespace ts2::content
