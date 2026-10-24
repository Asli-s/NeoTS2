#pragma once

#include "formats/dbpf.hpp"

#include <cstdint>
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
    // header's 4 bytes at 0x40.
    void finish(std::uint32_t headerWord40);

private:
    void write(std::span<const std::uint8_t> bytes);

    std::ostream& out_;
    std::uint64_t position_ = 0;
    std::vector<IndexEntry> entries_;
};

} // namespace ts2::dbpf
