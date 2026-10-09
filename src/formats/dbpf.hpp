#pragma once

#include <cstdint>
#include <filesystem>
#include <istream>
#include <memory>
#include <span>
#include <vector>

namespace ts2::dbpf {

struct Header {
    std::uint32_t majorVersion = 0;
    std::uint32_t minorVersion = 0;
    std::uint32_t indexMajorVersion = 0;
    std::uint32_t indexEntryCount = 0;
    std::uint32_t indexOffset = 0;
    std::uint32_t indexSize = 0;
    std::uint32_t holeCount = 0;
    std::uint32_t holeOffset = 0;
    std::uint32_t holeSize = 0;
    std::uint32_t indexMinorVersion = 0;
};

struct ResourceKey {
    std::uint32_t type = 0;
    std::uint32_t group = 0;
    std::uint32_t instance = 0;
    std::uint32_t instanceHigh = 0;

    bool operator==(const ResourceKey&) const = default;
};

struct ResourceKeyHash {
    std::size_t operator()(const ResourceKey& k) const noexcept {
        std::uint64_t h = 0xCBF29CE484222325ull;
        for (std::uint32_t part : {k.type, k.group, k.instance, k.instanceHigh}) {
            h = (h ^ part) * 0x100000001B3ull;
        }
        return static_cast<std::size_t>(h);
    }
};

struct IndexEntry {
    ResourceKey key;
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
    bool compressed = false;
    std::uint32_t uncompressedSize = 0;
};

class Package {
public:
    explicit Package(const std::filesystem::path& path);
    explicit Package(std::span<const std::uint8_t> bytes);

    const Header& header() const { return header_; }
    const std::vector<IndexEntry>& entries() const { return entries_; }

    std::vector<std::uint8_t> readRaw(const IndexEntry& entry);
    std::vector<std::uint8_t> read(const IndexEntry& entry);

    // A package read from a file holds it open from the first read until close(); the next read
    // reopens it.
    void close();

private:
    void load();
    void readHeader();
    void readIndex();
    void applyCompressionDirectory();
    std::vector<std::uint8_t> readBytes(std::uint64_t offset, std::size_t size);
    std::unique_ptr<std::istream> openFile() const;

    std::filesystem::path path_;
    std::unique_ptr<std::istream> stream_;
    std::uintmax_t fileSize_ = 0;
    Header header_;
    std::vector<IndexEntry> entries_;
};

} // namespace ts2::dbpf
