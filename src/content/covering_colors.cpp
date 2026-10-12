#include "content/covering_colors.hpp"

#include "formats/dbpf.hpp"
#include "formats/image.hpp"

#include <bit>
#include <cctype>
#include <exception>

namespace ts2::content {

namespace {

constexpr std::uint32_t kFloorThumbnail = 0x8C311262;
constexpr std::uint32_t kWallThumbnail = 0x8C31125E;

std::array<std::uint32_t, 256> table(std::uint32_t polynomial, int width) {
    std::array<std::uint32_t, 256> t{};
    const std::uint32_t top = 1u << (width - 1);
    const std::uint32_t mask = width == 32 ? 0xFFFFFFFFu : (1u << width) - 1;
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i << (width - 8);
        for (int bit = 0; bit < 8; ++bit) {
            c = (c & top) ? ((c << 1) ^ polynomial) & mask : (c << 1) & mask;
        }
        t[i] = c;
    }
    return t;
}

} // namespace

std::uint32_t nameHash32(const std::string& name) {
    static const auto t = table(0x04C11DB7, 32);
    std::uint32_t crc = 0xFFFFFFFF;
    for (const unsigned char ch : name) {
        const auto c = static_cast<std::uint8_t>(std::tolower(ch));
        crc = (crc << 8) ^ t[(crc >> 24) ^ c];
    }
    return crc;
}

std::uint32_t nameHash24(const std::string& name) {
    static const auto t = table(0x864CFB, 24);
    std::uint32_t crc = 0xB704CE;
    for (const unsigned char ch : name) {
        const auto c = static_cast<std::uint8_t>(std::tolower(ch));
        crc = ((crc << 8) & 0xFFFFFF) ^ t[((crc >> 16) ^ c) & 0xFF];
    }
    return crc & 0xFFFFFF;
}

std::array<float, 3> averageColor(std::span<const std::uint8_t> rgba) {
    const float scale = std::bit_cast<float>(0x3B808081u);  // 1/255
    std::array<float, 3> sum{0, 0, 0};
    std::uint32_t count = 0;
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        if (rgba[i + 3] == 0) {
            continue;
        }
        for (std::size_t c = 0; c < 3; ++c) {
            sum[c] = static_cast<float>(sum[c] + static_cast<double>(rgba[i + c]) * scale);
        }
        ++count;
    }
    if (count == 0) {
        return {0, 0, 0};
    }
    const double each = 1.0 / static_cast<double>(count);
    return {static_cast<float>(sum[0] * each), static_cast<float>(sum[1] * each), static_cast<float>(sum[2] * each)};
}

CoveringColors::CoveringColors(const std::filesystem::path& thumbnails) : path_(thumbnails) {
    std::error_code ec;
    if (thumbnails.empty() || !std::filesystem::is_regular_file(thumbnails, ec)) {
        return;
    }
    try {
        dbpf::Package package{thumbnails};
        const auto& entries = package.entries();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto& k = entries[i].key;
            if (k.type == kFloorThumbnail || k.type == kWallThumbnail) {
                index_.emplace(std::pair{k.type, (static_cast<std::uint64_t>(k.instanceHigh) << 32) | k.instance}, i);
            }
        }
    } catch (const std::exception&) {
        index_.clear();
    }
}

std::optional<std::array<float, 3>> CoveringColors::color(CoveringKind kind, const std::string& name) {
    if (name.empty()) {
        return std::nullopt;
    }
    const std::uint32_t low = nameHash32(name);
    if (const auto hit = cache_.find(low); hit != cache_.end()) {
        return hit->second;
    }
    const std::uint32_t type = kind == CoveringKind::Floor ? kFloorThumbnail : kWallThumbnail;
    // Cached thumbnails carry the CRC-24 as their high instance, or 0 (both are seen).
    auto found = index_.find({type, (static_cast<std::uint64_t>(nameHash24(name)) << 32) | low});
    if (found == index_.end()) {
        found = index_.find({type, low});
    }
    if (found == index_.end()) {
        return std::nullopt;
    }
    try {
        dbpf::Package package{path_};
        const auto image = image::decode(package.read(package.entries().at(found->second)));
        if (!image) {
            return std::nullopt;
        }
        const auto c = averageColor(image->rgba);
        cache_.emplace(low, c);
        return c;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace ts2::content
