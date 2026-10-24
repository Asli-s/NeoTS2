#pragma once

#include <cstdint>
#include <filesystem>

namespace ts2::content {

// Copies a pack's shipped lot (a LotCatalog cx_ package) into a save folder's lot catalog, as the
// game imports it. The package is kept as it is up to the end of the last part it uses (resources,
// index, hole table and holes), so bytes past that are dropped. Its version becomes 1.2 and the
// header's 4 bytes at 0x40 become 0x00010000 plus the pack's game number plus 1. Returns the size
// written. Throws on a broken package, a file larger than `maxBytes` or a failed write.
std::uintmax_t importCatalogLot(const std::filesystem::path& from, const std::filesystem::path& to, int gameNumber,
                                std::uintmax_t maxBytes);

} // namespace ts2::content
