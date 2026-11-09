#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace ts2::content {

// Merges the packs' Thumbnails packages into one new package as the game does for a new save folder,
// each resource copied as stored into group 0xFFFFFFFF. A later resource replaces an earlier one of the
// same type and instance when their high instances match or either is 0 (older index packages have none).
// Returns the new package's size. Throws on a broken package, a failed write or thumbnails over
// `maxBytes`, before anything is written.
std::uintmax_t mergeThumbnailPackages(const std::vector<std::filesystem::path>& sources, const std::filesystem::path& target,
                                      std::uintmax_t maxBytes);

} // namespace ts2::content
