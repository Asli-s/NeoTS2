#include "content/lot_import.hpp"

#include "formats/dbpf.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace ts2::content {

namespace {

// Shipped lots are 2 MB at most. A lot is read into memory whole, so the limit keeps a broken or
// tampered one from taking it all.
constexpr std::uintmax_t kMaxLotBytes = std::uintmax_t{256} << 20;

std::uint32_t u32(const std::vector<std::uint8_t>& bytes, std::uint64_t at) {
    return static_cast<std::uint32_t>(bytes[at]) | static_cast<std::uint32_t>(bytes[at + 1]) << 8 |
           static_cast<std::uint32_t>(bytes[at + 2]) << 16 | static_cast<std::uint32_t>(bytes[at + 3]) << 24;
}

void put(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        bytes[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

// Where the package's last used part ends. Every part must lie inside the file.
std::uint64_t usedEnd(const std::vector<std::uint8_t>& bytes) {
    const dbpf::Package package{std::span<const std::uint8_t>(bytes)};
    const dbpf::Header& header = package.header();
    std::uint64_t end = 96;
    const auto take = [&](std::uint64_t offset, std::uint64_t size) {
        if (offset + size > bytes.size()) {
            throw std::runtime_error("a part of the package lies outside the file");
        }
        end = std::max(end, offset + size);
    };
    for (const dbpf::IndexEntry& entry : package.entries()) {
        take(entry.offset, entry.size);
    }
    take(header.indexOffset, header.indexSize);
    if (header.holeCount > 0) {
        take(header.holeOffset, header.holeSize);
        if (std::uint64_t{header.holeCount} * 8 > header.holeSize) {
            throw std::runtime_error("the hole table is smaller than its count says");
        }
        for (std::uint32_t i = 0; i < header.holeCount; ++i) {
            const std::uint64_t at = std::uint64_t{header.holeOffset} + std::uint64_t{i} * 8;
            take(u32(bytes, at), u32(bytes, at + 4));
        }
    }
    return end;
}

} // namespace

std::uintmax_t importCatalogLot(const std::filesystem::path& from, const std::filesystem::path& to, int gameNumber,
                                std::uintmax_t maxBytes) {
    const std::uintmax_t size = std::filesystem::file_size(from);
    if (size > kMaxLotBytes) {
        throw std::runtime_error("the lot is larger than any shipped lot");
    }
    if (size > maxBytes) {
        throw std::runtime_error("the install's shipped content is larger than any real install");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream in(from, std::ios::binary);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        throw std::runtime_error("can't read the lot");
    }
    bytes.resize(static_cast<std::size_t>(usedEnd(bytes)));
    put(bytes, 8, 2);
    put(bytes, 0x40, 0x00010000u + static_cast<std::uint32_t>(gameNumber) + 1);

    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())) || !out.flush()) {
        throw std::runtime_error("can't write the lot");
    }
    return bytes.size();
}

} // namespace ts2::content
