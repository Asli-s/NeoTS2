#pragma once

#include "formats/dbpf.hpp"

#include <cstdint>
#include <optional>
#include <ostream>
#include <span>
#include <vector>

namespace ts2::dbpf {

// Writes a package (DBPF 1.2, index 7) to a stream, one resource at a time, as the game writes
// the packages it makes in the user folder. Resources go in the order they are added, with the
// compression directory and the index after them. The index has instance-high fields (index 7.2)
// only when a resource needs one.
class PackageWriter {
public:
    // `out` must be empty, as the header is written back at its start at the end.
    explicit PackageWriter(std::ostream& out);

    // Adds a resource stored as given. A compressed resource is listed in the compression directory
    // with its size once unpacked.
    void add(const ResourceKey& key, std::span<const std::uint8_t> stored, bool compressed, std::uint32_t uncompressedSize);

    // Writes the compression directory, the index and the header. `headerWord40` goes in the
    // header's 4 bytes at 0x40. The index has instance-high fields (7.2) when a resource needs one
    // or `instanceHigh` asks for them, as when a package that had them is written again.
    void finish(std::uint32_t headerWord40, bool instanceHigh = false);

    // Writes the header with these versions instead (DBPF 1.`minorVersion`, index
    // 7.`indexMinorVersion`), as a package written again keeps the ones it had. Index 7.2 has
    // instance-high fields, 7.0 and 7.1 don't, so a resource that needs one with 7.0 or 7.1 throws.
    void keepVersions(std::uint32_t minorVersion, std::uint32_t indexMinorVersion);

private:
    void write(std::span<const std::uint8_t> bytes);

    std::ostream& out_;
    std::uint32_t minorVersion_ = 2;
    std::optional<std::uint32_t> indexMinorVersion_;
    std::uint64_t position_ = 0;
    std::vector<IndexEntry> entries_;
};

} // namespace ts2::dbpf
