#pragma once

#include "formats/dbpf.hpp"

#include <array>
#include <cstdint>
#include <ostream>
#include <span>
#include <vector>

namespace ts2::dbpf {

// A resource as a package stores it: compressed (QFS) or plain.
struct StoredResource {
    ResourceKey key;
    std::vector<std::uint8_t> bytes;
    bool compressed = false;
    std::uint32_t uncompressedSize = 0;
};

// Stores a resource as the game does when it writes one into a package. It is compressed when it is
// 50 to 16,000,000 bytes long and the compressed form is under 0.8 of its size, else kept plain.
// The game compresses every resource of a saved neighborhood, lot or character file this way.
StoredResource storeResource(const ResourceKey& key, std::span<const std::uint8_t> body);

// A package ready to be written back the way the game leaves one after packing it: every resource
// one after the other from offset 0x60, the compression directory rebuilt, the index straight after
// the data and no free space. The game reads resources by key, so the order they lie in only has to
// be stable. A package read from a file keeps its index order and the order its data lay in.
struct PackageImage {
    // The source header. Fields the game keeps (dates, the word at 0x40) are written back as they
    // were, and the counts, offsets and versions are set anew.
    std::array<std::uint8_t, 96> header{};
    // In index order. The compression directory is not one of them.
    std::vector<StoredResource> resources;
    // Positions in `resources`, in the order their data is written. Must name each one once.
    std::vector<std::size_t> dataOrder;
    // Where the compression directory goes among the index entries and among the data, and the
    // order its entries were in. A package without compressed resources gets no directory.
    std::size_t directoryIndexPosition = 0;
    std::size_t directoryDataPosition = 0;
    std::vector<ResourceKey> directoryOrder;
};

// Reads a whole package file into an image that writes back to the same resources. Throws when the
// package is broken: an unreadable index, a resource outside the file, two with one key, or a
// compressed resource that does not unpack to its listed size.
PackageImage readImage(std::span<const std::uint8_t> file);

// Writes the image. Throws when it can't make a valid package (two resources with one key, the
// compression directory's own key among the resources, or more than 4 GB).
std::vector<std::uint8_t> writeImage(const PackageImage& image);

// Reads `file` back with the package reader and checks that it holds exactly the image's resources:
// the same keys, stored bytes and compression, with every compressed resource unpacking to its
// listed size. Throws on the first difference. Used on every package before it replaces a save.
void verifyImage(std::span<const std::uint8_t> file, const PackageImage& image);

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
