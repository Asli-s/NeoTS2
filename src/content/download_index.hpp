#pragma once

#include "formats/dbpf.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace ts2::content {

// What a Downloads package's own index holds, kept so a package is read once, and not again on
// later starts while its size and time stay the same.
struct IndexedPackage {
    std::filesystem::path path;
    std::uintmax_t size = 0;
    std::int64_t modified = 0;   // the file's time, in the file system's own units
    bool usable = false;         // false for a package that couldn't be read or that the game refuses
    std::vector<dbpf::IndexEntry> entries;
};

[[nodiscard]] std::vector<std::uint8_t> writeDownloadIndex(std::span<const IndexedPackage> packages);

// The packages in a saved index. Empty when it is damaged or from another version, which only
// means the packages are read again.
[[nodiscard]] std::vector<IndexedPackage> readDownloadIndex(std::span<const std::uint8_t> bytes);

} // namespace ts2::content
