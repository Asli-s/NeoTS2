#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace ts2::content {

// Merges thumbnail packages into one new package, as the game merges the packs' shipped
// Thumbnails packages into a new save folder. Every resource is copied as stored and filed under
// group 0xFFFFFFFF. A later package's resource replaces an earlier one with the same type and
// instance when their high instances match or either is 0 (packages with the older index have
// none). Returns the size of the new package. Throws on a broken package, a failed write, or
// thumbnails larger than `maxBytes`, before anything is written.
std::uintmax_t mergeThumbnailPackages(const std::vector<std::filesystem::path>& sources, const std::filesystem::path& target,
                                      std::uintmax_t maxBytes);

} // namespace ts2::content
