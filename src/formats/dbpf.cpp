#include "formats/dbpf.hpp"

#include "formats/binary_reader.hpp"
#include "formats/qfs.hpp"
#include "formats/resource_types.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace ts2::dbpf {

namespace {

constexpr std::size_t kHeaderSize = 96;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("DBPF: " + message);
}

} // namespace

Package::Package(const std::filesystem::path& path) : path_(path) {
    stream_ = openFile();
    fileSize_ = std::filesystem::file_size(path);
    load();
    stream_.reset();
}

Package::Package(std::span<const std::uint8_t> bytes)
    : stream_(std::make_unique<std::istringstream>(
          std::string(bytes.begin(), bytes.end()), std::ios::binary)),
      fileSize_(bytes.size()) {
    load();
}

void Package::close() {
    if (!path_.empty()) {
        stream_.reset();
    }
}

std::unique_ptr<std::istream> Package::openFile() const {
    auto file = std::make_unique<std::ifstream>(path_, std::ios::binary);
    if (!*file) {
        fail("cannot open " + path_.string());
    }
    return file;
}

void Package::load() {
    readHeader();
    if (appleDouble_) {
        return;
    }
    readIndex();
    applyCompressionDirectory();
}

std::vector<std::uint8_t> Package::readBytes(std::uint64_t offset, std::size_t size) {
    if (offset > fileSize_ || size > fileSize_ - offset) {
        fail("read lies outside the file");
    }
    if (!stream_) {
        stream_ = openFile();
    }
    std::vector<std::uint8_t> bytes(size);
    stream_->clear();
    stream_->seekg(static_cast<std::streamoff>(offset));
    if (!stream_->read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) {
        fail("read failed at offset " + std::to_string(offset));
    }
    return bytes;
}

void Package::readHeader() {
    const std::vector<std::uint8_t> bytes =
        readBytes(0, static_cast<std::size_t>(std::min<std::uintmax_t>(fileSize_, kHeaderSize)));
    // A Mac's AppleDouble sidecar, a "._" file of metadata, reads as an empty package.
    if (bytes.size() >= 4 && bytes[0] == 0x00 && bytes[1] == 0x05 && bytes[2] == 0x16 && bytes[3] == 0x07) {
        appleDouble_ = true;
        return;
    }
    if (bytes.size() < kHeaderSize) {
        fail("file is smaller than the 96-byte header");
    }

    if (bytes[0] != 'D' || bytes[1] != 'B' || bytes[2] != 'P' || bytes[3] != 'F') {
        fail("missing 'DBPF' magic - not a package file");
    }

    BinaryReader r(bytes);
    r.skip(4);
    header_.majorVersion = r.u32();
    header_.minorVersion = r.u32();
    r.skip(20);
    header_.indexMajorVersion = r.u32();
    header_.indexEntryCount = r.u32();
    header_.indexOffset = r.u32();
    header_.indexSize = r.u32();
    header_.holeCount = r.u32();
    header_.holeOffset = r.u32();
    header_.holeSize = r.u32();
    header_.indexMinorVersion = r.u32();

    if (header_.majorVersion != 1) {
        fail("unsupported DBPF version " + std::to_string(header_.majorVersion) +
             " (this reader handles The Sims 2 packages, version 1)");
    }
    if (header_.indexMajorVersion != 7) {
        fail("unsupported index version " + std::to_string(header_.indexMajorVersion));
    }
}

void Package::readIndex() {
    if (header_.indexEntryCount == 0) {
        return;
    }

    const std::vector<std::uint8_t> bytes = readBytes(header_.indexOffset, header_.indexSize);

    const bool hasInstanceHigh = header_.indexMinorVersion == 2;
    const std::size_t entrySize = hasInstanceHigh ? 24 : 20;
    if (static_cast<std::size_t>(header_.indexEntryCount) * entrySize > bytes.size()) {
        fail("index is smaller than its entry count says");
    }

    BinaryReader r(bytes);
    entries_.reserve(header_.indexEntryCount);
    for (std::uint32_t i = 0; i < header_.indexEntryCount; ++i) {
        IndexEntry e;
        e.key.type = r.u32();
        e.key.group = r.u32();
        e.key.instance = r.u32();
        if (hasInstanceHigh) {
            e.key.instanceHigh = r.u32();
        }
        e.offset = r.u32();
        e.size = r.u32();
        e.uncompressedSize = e.size;
        entries_.push_back(e);
    }
}

void Package::applyCompressionDirectory() {
    const auto dir = std::find_if(entries_.begin(), entries_.end(),
                                  [](const IndexEntry& e) { return e.key.type == types::kDirectory; });
    if (dir == entries_.end()) {
        return;
    }

    const std::vector<std::uint8_t> bytes = readRaw(*dir);
    const bool hasInstanceHigh = header_.indexMinorVersion == 2;
    const std::size_t recordSize = hasInstanceHigh ? 20 : 16;

    std::unordered_map<ResourceKey, std::uint32_t, ResourceKeyHash> uncompressedSizes;
    BinaryReader r(bytes);
    while (r.remaining() >= recordSize) {
        ResourceKey key;
        key.type = r.u32();
        key.group = r.u32();
        key.instance = r.u32();
        if (hasInstanceHigh) {
            key.instanceHigh = r.u32();
        }
        uncompressedSizes[key] = r.u32();
    }

    // A key listed twice in the index has every copy marked.
    for (IndexEntry& e : entries_) {
        if (auto it = uncompressedSizes.find(e.key); it != uncompressedSizes.end()) {
            e.compressed = true;
            e.uncompressedSize = it->second;
        }
    }
}

std::vector<std::uint8_t> Package::readRaw(const IndexEntry& entry) {
    return readBytes(entry.offset, entry.size);
}

std::vector<std::uint8_t> Package::read(const IndexEntry& entry) {
    std::vector<std::uint8_t> bytes = readRaw(entry);
    if (entry.compressed && qfs::isCompressed(bytes)) {
        std::vector<std::uint8_t> decompressed = qfs::decompress(bytes);
        if (decompressed.size() != entry.uncompressedSize) {
            fail("decompressed size differs from the compression directory");
        }
        return decompressed;
    }
    return bytes;
}

} // namespace ts2::dbpf
