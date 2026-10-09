#include "formats/dbpf.hpp"

#include "formats/binary_reader.hpp"
#include "formats/qfs.hpp"
#include "formats/resource_types.hpp"

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
    if (fileSize_ < kHeaderSize) {
        fail("file is smaller than the 96-byte header");
    }
    const std::vector<std::uint8_t> bytes = readBytes(0, kHeaderSize);

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
    std::unordered_map<ResourceKey, std::size_t, ResourceKeyHash> byKey;
    byKey.reserve(entries_.size());
    const IndexEntry* dir = nullptr;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        byKey.try_emplace(entries_[i].key, i);
        if (dir == nullptr && entries_[i].key.type == types::kDirectory) {
            dir = &entries_[i];
        }
    }
    if (dir == nullptr) {
        return;
    }

    const std::vector<std::uint8_t> bytes = readRaw(*dir);
    const bool hasInstanceHigh = header_.indexMinorVersion == 2;
    const std::size_t recordSize = hasInstanceHigh ? 20 : 16;

    BinaryReader r(bytes);
    while (r.remaining() >= recordSize) {
        ResourceKey key;
        key.type = r.u32();
        key.group = r.u32();
        key.instance = r.u32();
        if (hasInstanceHigh) {
            key.instanceHigh = r.u32();
        }
        const std::uint32_t uncompressedSize = r.u32();

        if (auto it = byKey.find(key); it != byKey.end()) {
            IndexEntry& e = entries_[it->second];
            e.compressed = true;
            e.uncompressedSize = uncompressedSize;
        }
    }
}

std::vector<std::uint8_t> Package::readRaw(const IndexEntry& entry) {
    return readBytes(entry.offset, entry.size);
}

std::vector<std::uint8_t> Package::read(const IndexEntry& entry) {
    std::vector<std::uint8_t> bytes = readRaw(entry);
    if (entry.compressed && qfs::isCompressed(bytes)) {
        return qfs::decompress(bytes);
    }
    return bytes;
}

} // namespace ts2::dbpf
