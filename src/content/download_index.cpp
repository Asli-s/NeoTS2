#include "content/download_index.hpp"

#include <array>
#include <cstring>
#include <string>

namespace ts2::content {

namespace {

constexpr std::array<std::uint8_t, 4> kMagic{'N', 'D', 'L', 'I'};
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kEntryBytes = 8 * 4;
// Longer than any path Windows allows, and a guard against a damaged length.
constexpr std::uint32_t kMostPathBytes = 65536;

void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
}

void put64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    put32(out, static_cast<std::uint32_t>(value));
    put32(out, static_cast<std::uint32_t>(value >> 32));
}

// Reads little-endian numbers, and turns false for good once the bytes run out.
class Cursor {
public:
    explicit Cursor(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    bool ok() const { return ok_; }
    std::size_t left() const { return bytes_.size() - at_; }

    std::span<const std::uint8_t> take(std::size_t count) {
        if (!ok_ || count > left()) {
            ok_ = false;
            return {};
        }
        const auto part = bytes_.subspan(at_, count);
        at_ += count;
        return part;
    }

    std::uint32_t u32() {
        const auto part = take(4);
        return part.empty() ? 0 : static_cast<std::uint32_t>(part[0]) | static_cast<std::uint32_t>(part[1]) << 8 | static_cast<std::uint32_t>(part[2]) << 16 | static_cast<std::uint32_t>(part[3]) << 24;
    }

    std::uint64_t u64() {
        const std::uint64_t low = u32();
        return low | static_cast<std::uint64_t>(u32()) << 32;
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t at_ = 0;
    bool ok_ = true;
};

} // namespace

std::vector<std::uint8_t> writeDownloadIndex(std::span<const IndexedPackage> packages) {
    std::vector<std::uint8_t> out(kMagic.begin(), kMagic.end());
    put32(out, kVersion);
    put32(out, static_cast<std::uint32_t>(packages.size()));
    for (const IndexedPackage& p : packages) {
        const std::u8string path = p.path.u8string();
        put32(out, static_cast<std::uint32_t>(path.size()));
        out.insert(out.end(), path.begin(), path.end());
        put64(out, p.size);
        put64(out, static_cast<std::uint64_t>(p.modified));
        put32(out, p.usable ? 1 : 0);
        put32(out, static_cast<std::uint32_t>(p.entries.size()));
        for (const dbpf::IndexEntry& e : p.entries) {
            for (const std::uint32_t value : {e.key.type, e.key.group, e.key.instance, e.key.instanceHigh, e.offset, e.size, e.compressed ? 1u : 0u, e.uncompressedSize}) {
                put32(out, value);
            }
        }
    }
    return out;
}

std::vector<IndexedPackage> readDownloadIndex(std::span<const std::uint8_t> bytes) {
    Cursor in(bytes);
    const auto magic = in.take(kMagic.size());
    if (!in.ok() || std::memcmp(magic.data(), kMagic.data(), kMagic.size()) != 0 || in.u32() != kVersion) {
        return {};
    }
    const std::uint32_t count = in.u32();
    // Each package takes at least its fixed fields, so a damaged count can't ask for too much.
    constexpr std::size_t kLeastPackageBytes = 4 + 8 + 8 + 4 + 4;
    if (!in.ok() || count > in.left() / kLeastPackageBytes) {
        return {};
    }
    std::vector<IndexedPackage> packages(count);
    for (IndexedPackage& p : packages) {
        const std::uint32_t pathBytes = in.u32();
        if (pathBytes > kMostPathBytes) {
            return {};
        }
        const auto path = in.take(pathBytes);
        if (!in.ok()) {
            return {};
        }
        p.path = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size()));
        p.size = in.u64();
        p.modified = static_cast<std::int64_t>(in.u64());
        p.usable = in.u32() != 0;
        const std::uint32_t entries = in.u32();
        if (!in.ok() || entries > in.left() / kEntryBytes) {
            return {};
        }
        p.entries.resize(entries);
        for (dbpf::IndexEntry& e : p.entries) {
            e.key.type = in.u32();
            e.key.group = in.u32();
            e.key.instance = in.u32();
            e.key.instanceHigh = in.u32();
            e.offset = in.u32();
            e.size = in.u32();
            e.compressed = in.u32() != 0;
            e.uncompressedSize = in.u32();
        }
    }
    return in.ok() ? packages : std::vector<IndexedPackage>{};
}

} // namespace ts2::content
