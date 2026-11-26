#include "content/covering_colors.hpp"

#include "diag/log.hpp"
#include "formats/dbpf.hpp"
#include "formats/dbpf_append.hpp"
#include "formats/image.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <exception>

namespace ts2::content {

namespace {

constexpr std::uint32_t kFloorThumbnail = 0x8C311262;
constexpr std::uint32_t kWallThumbnail = 0x8C31125E;
constexpr std::uint32_t kFenceThumbnail = 0xCC30CDF8;
// A fireplace's picture, the whole fireplace from mantle to chimney top, filed under its id.
constexpr std::uint32_t kFireplaceThumbnail = 0xCC48C51F;

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

tga::Pixels coveringThumbnail(const tga::Pixels& texture, CoveringKind kind) {
    constexpr std::uint32_t kSide = 128, kFirst = 10, kShown = 109;
    tga::Pixels out{kSide, kSide, std::vector<std::uint8_t>(std::size_t{kSide} * kSide * 4, 0)};
    const std::uint32_t w = texture.width, h = texture.height;
    if (w == 0 || h == 0 || texture.rgba.size() < std::size_t{w} * h * 4) {
        return out;
    }
    const std::uint64_t across = kind == CoveringKind::Wall ? 3 : 1;
    // Each picture pixel is the average of the texels it covers.
    const auto span = [](std::uint64_t i, std::uint64_t size) {
        const std::uint64_t from = i * size / kShown, to = std::max(from + 1, ((i + 1) * size + kShown - 1) / kShown);
        return std::pair{from, to};
    };
    for (std::uint32_t py = 0; py < kShown; ++py) {
        const auto [y0, y1] = span(py, h);
        for (std::uint32_t px = 0; px < kShown; ++px) {
            const auto [x0, x1] = span(px, w * across);
            std::array<std::uint64_t, 3> sum{};
            std::uint64_t count = 0;
            for (std::uint64_t y = y0; y < y1; ++y) {
                for (std::uint64_t x = x0; x < x1; ++x) {
                    const std::size_t at = (static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x % w)) * 4;
                    sum[0] += texture.rgba[at];
                    sum[1] += texture.rgba[at + 1];
                    sum[2] += texture.rgba[at + 2];
                    ++count;
                }
            }
            const std::size_t to = (std::size_t{kFirst + py} * kSide + kFirst + px) * 4;
            for (int c = 0; c < 3; ++c) {
                out.rgba[to + c] = static_cast<std::uint8_t>((sum[c] + count / 2) / count);
            }
            out.rgba[to + 3] = 255;
        }
    }
    return out;
}

CoveringColors::CoveringColors(const std::filesystem::path& thumbnails) : file_(thumbnails) {
    std::error_code ec;
    if (thumbnails.empty() || !std::filesystem::is_regular_file(thumbnails, ec)) {
        return;
    }
    std::error_code timeError, sizeError;
    written_ = std::filesystem::last_write_time(thumbnails, timeError);
    size_ = std::filesystem::file_size(thumbnails, sizeError);
    try {
        const auto& entries = package_.emplace(thumbnails).entries();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto& k = entries[i].key;
            if (k.type == kFloorThumbnail || k.type == kWallThumbnail || k.type == kFenceThumbnail || k.type == kFireplaceThumbnail) {
                index_.emplace(std::pair{k.type, (static_cast<std::uint64_t>(k.instanceHigh) << 32) | k.instance}, i);
            }
        }
    } catch (const std::exception&) {
        index_.clear();
        package_.reset();
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
    if (missing_.contains({type, low})) {
        return std::nullopt;
    }
    const auto c = read(type, name, low);
    if (c) {
        cache_.emplace(low, *c);
    } else {
        missing_.insert({type, low});
    }
    return c;
}

std::optional<std::array<float, 3>> CoveringColors::read(std::uint32_t type, const std::string& name, std::uint32_t low) {
    const auto found = image(type, name, low, tga::kMaxSide);
    return found ? std::optional(averageColor(found->rgba)) : std::nullopt;
}

std::optional<tga::Pixels> CoveringColors::picture(CoveringKind kind, const std::string& name, std::uint32_t maxSide) {
    if (name.empty()) {
        return std::nullopt;
    }
    return image(kind == CoveringKind::Floor ? kFloorThumbnail : kWallThumbnail, name, nameHash32(name), maxSide);
}

std::optional<tga::Pixels> CoveringColors::fencePicture(std::uint32_t guid, std::uint32_t maxSide) {
    return cached(kFenceThumbnail, guid, maxSide);
}

std::optional<tga::Pixels> CoveringColors::fireplacePicture(std::int32_t id, std::uint32_t maxSide) {
    return cached(kFireplaceThumbnail, static_cast<std::uint32_t>(id), maxSide);
}

std::optional<tga::Pixels> CoveringColors::cached(std::uint32_t type, std::uint64_t instance, std::uint32_t maxSide) {
    const auto found = index_.find({type, instance});
    if (found == index_.end() || !package_) {
        return std::nullopt;
    }
    try {
        return ts2::image::decode(package_->read(package_->entries().at(found->second)), maxSide);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<tga::Pixels> CoveringColors::image(std::uint32_t type, const std::string& name, std::uint32_t low, std::uint32_t maxSide) {
    // Cached thumbnails carry the CRC-24 as their high instance, or 0 (both occur).
    const std::uint64_t named = (static_cast<std::uint64_t>(nameHash24(name)) << 32) | low;
    return cached(type, index_.contains({type, named}) ? named : low, maxSide);
}

bool CoveringColors::store(CoveringKind kind, const std::string& name, const tga::Pixels& picture) {
    constexpr std::uint32_t kAnyGroup = 0xFFFFFFFF;
    std::error_code ec;
    if (file_.empty() || name.empty() || !std::filesystem::is_regular_file(file_, ec)) {
        return false;
    }
    const auto jpeg = image::encodeJpegWithAlpha(picture);
    if (jpeg.empty()) {
        return false;
    }
    const std::uint32_t type = kind == CoveringKind::Floor ? kFloorThumbnail : kWallThumbnail;
    const dbpf::StoredResource stored{{type, kAnyGroup, nameHash32(name), nameHash24(name)}, jpeg, false, static_cast<std::uint32_t>(jpeg.size())};
    if (package_) {
        package_->close();
    }
    try {
        dbpf::appendToPackage(file_, std::span(&stored, 1));
    } catch (const std::exception& e) {
        diag::warnOnce(std::string("thumbnails: a drawn covering picture can't be kept: ") + e.what());
        *this = CoveringColors(file_);
        return false;
    }
    *this = CoveringColors(file_);
    return true;
}

bool CoveringColors::changedOnDisk() const {
    std::error_code ec;
    if (file_.empty() || !std::filesystem::is_regular_file(file_, ec)) {
        return false;
    }
    std::error_code timeError, sizeError;
    const auto written = std::filesystem::last_write_time(file_, timeError);
    const auto size = std::filesystem::file_size(file_, sizeError);
    return !timeError && !sizeError && (written != written_ || size != size_);
}

} // namespace ts2::content
