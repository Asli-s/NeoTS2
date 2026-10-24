#include "formats/dbpf_writer.hpp"

#include "formats/resource_types.hpp"

#include <array>
#include <limits>
#include <stdexcept>
#include <string>

namespace ts2::dbpf {

namespace {

constexpr std::size_t kHeaderSize = 96;

// The compression directory's own key in packages the game writes.
constexpr ResourceKey kDirectoryKey{types::kDirectory, types::kDirectory, 0x286B1F03, 0};

void put(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void putKey(std::vector<std::uint8_t>& out, const ResourceKey& key, bool instanceHigh) {
    put(out, key.type);
    put(out, key.group);
    put(out, key.instance);
    if (instanceHigh) {
        put(out, key.instanceHigh);
    }
}

} // namespace

PackageWriter::PackageWriter(std::ostream& out) : out_(out) {
    const std::array<std::uint8_t, kHeaderSize> placeholder{};
    write(placeholder);
}

void PackageWriter::write(std::span<const std::uint8_t> bytes) {
    // Offsets in a package are 32 bits.
    if (bytes.size() > std::numeric_limits<std::uint32_t>::max() - position_) {
        throw std::runtime_error("DBPF: the package would be larger than 4 GB");
    }
    if (!out_.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        throw std::runtime_error("DBPF: writing the package failed");
    }
    position_ += bytes.size();
}

void PackageWriter::add(const ResourceKey& key, std::span<const std::uint8_t> stored, bool compressed, std::uint32_t uncompressedSize) {
    IndexEntry entry;
    entry.key = key;
    entry.offset = static_cast<std::uint32_t>(position_);
    entry.size = static_cast<std::uint32_t>(stored.size());
    entry.compressed = compressed;
    entry.uncompressedSize = compressed ? uncompressedSize : entry.size;
    write(stored);
    entries_.push_back(entry);
}

void PackageWriter::finish(std::uint32_t headerWord40) {
    bool instanceHigh = false;
    for (const IndexEntry& entry : entries_) {
        instanceHigh = instanceHigh || entry.key.instanceHigh != 0;
    }
    std::vector<std::uint8_t> directory;
    for (const IndexEntry& entry : entries_) {
        if (entry.compressed) {
            putKey(directory, entry.key, instanceHigh);
            put(directory, entry.uncompressedSize);
        }
    }
    if (!directory.empty()) {
        add(kDirectoryKey, directory, false, 0);
    }

    std::vector<std::uint8_t> index;
    for (const IndexEntry& entry : entries_) {
        putKey(index, entry.key, instanceHigh);
        put(index, entry.offset);
        put(index, entry.size);
    }
    const auto indexOffset = static_cast<std::uint32_t>(position_);
    write(index);

    std::vector<std::uint8_t> header{'D', 'B', 'P', 'F'};
    put(header, 1);  // version 1.2
    put(header, 2);
    header.resize(0x20, 0);  // reserved and dates, left 0 as the game leaves them
    put(header, 7);  // index version 7
    put(header, static_cast<std::uint32_t>(entries_.size()));
    put(header, indexOffset);
    put(header, static_cast<std::uint32_t>(index.size()));
    put(header, 0);  // no holes
    put(header, 0);
    put(header, 0);
    put(header, instanceHigh ? 2 : 1);
    put(header, headerWord40);
    header.resize(kHeaderSize, 0);
    if (!out_.seekp(0) || !out_.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size())) ||
        !out_.flush()) {
        throw std::runtime_error("DBPF: writing the package failed");
    }
}

} // namespace ts2::dbpf
