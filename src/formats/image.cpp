#include "formats/image.hpp"

#include "formats/jpeg.hpp"

#include <algorithm>
#include <limits>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 16384  // tga::kMaxSide
// Zeroed: stb leaves what a broken JPEG doesn't reach as it found it, which would show old memory.
#define STBI_MALLOC(size) calloc(1, size)
#define STBI_REALLOC(block, size) realloc(block, size)
#define STBI_FREE(block) free(block)
#include "stb_image.h"

namespace ts2::image {

namespace {

// The alpha the game keeps beside a JPEG's picture: an APP0 segment named "ALFA" holding one byte
// per pixel, rows from the top, run-length coded. A byte below 0x80 is followed by that many plus
// one bytes as they are. A byte from 0x80 is followed by one byte repeated 257 minus it times.
// Empty when there is none or it doesn't fill the picture.
std::vector<std::uint8_t> jpegAlpha(std::span<const std::uint8_t> data, std::size_t pixels) {
    std::size_t i = 2;
    while (i + 4 <= data.size() && data[i] == 0xFF) {
        const std::uint8_t marker = data[i + 1];
        const std::size_t length = static_cast<std::size_t>(data[i + 2]) << 8 | data[i + 3];
        if (marker == 0xDA || length < 2 || i + 2 + length > data.size()) {
            break;
        }
        const auto segment = data.subspan(i + 4, length - 2);
        if (marker == 0xE0 && segment.size() >= 4 && segment[0] == 'A' && segment[1] == 'L' && segment[2] == 'F' && segment[3] == 'A') {
            std::vector<std::uint8_t> alpha;
            alpha.reserve(pixels);
            for (std::size_t k = 4; k < segment.size() && alpha.size() < pixels;) {
                const std::uint8_t c = segment[k++];
                if (c < 0x80) {
                    const std::size_t n = std::min<std::size_t>(c + 1u, segment.size() - k);
                    alpha.insert(alpha.end(), segment.begin() + static_cast<std::ptrdiff_t>(k), segment.begin() + static_cast<std::ptrdiff_t>(k + n));
                    k += n;
                } else if (k < segment.size()) {
                    alpha.insert(alpha.end(), 257u - c, segment[k++]);
                }
            }
            if (alpha.size() == pixels) {
                return alpha;
            }
            return {};
        }
        i += 2 + length;
    }
    return {};
}

// The bytes a PNG's pixels unpack to (each row with its filter byte), from its header, or 0 when
// the header is broken.
std::size_t pngRawBytes(std::span<const std::uint8_t> data) {
    if (data.size() < 26) {
        return 0;
    }
    auto u32 = [&](std::size_t at) {
        return static_cast<std::size_t>(data[at]) << 24 | static_cast<std::size_t>(data[at + 1]) << 16 |
               static_cast<std::size_t>(data[at + 2]) << 8 | data[at + 3];
    };
    const std::size_t width = u32(16), height = u32(20), depth = data[24];
    const std::size_t channels = data[25] == 2 ? 3 : data[25] == 4 ? 2 : data[25] == 6 ? 4 : 1;
    return height * (1 + (width * channels * depth + 7) / 8);
}

} // namespace

std::optional<tga::Pixels> decode(std::span<const std::uint8_t> data, std::uint32_t maxSide) {
    const bool other = data.size() >= 4 && ((data[0] == 0x89 && data[1] == 'P') || (data[0] == 0xFF && data[1] == 0xD8) ||
                                            (data[0] == 'B' && data[1] == 'M'));
    if (!other) {
        return tga::decode(data, maxSide);
    }
    if (data.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    // JPEGs decode as the game's JPEG library decodes them when it can read them, and with stb otherwise.
    tga::Pixels out;
    if (auto exact = data[0] == 0xFF ? jpeg::decode(data, maxSide) : std::nullopt) {
        out = std::move(*exact);
    } else {
        int w = 0, h = 0, n = 0;
        if (!stbi_info_from_memory(data.data(), static_cast<int>(data.size()), &w, &h, &n) || static_cast<std::uint32_t>(w) > maxSide ||
            static_cast<std::uint32_t>(h) > maxSide) {
            return std::nullopt;
        }
        // stb allocates the whole picture from its header and fills what the data lacks with zeros:
        // a BMP (RLE packs at most 255 pixels in 2 bytes) or JPEG (at least 2 bits per 8 x 8 block)
        // too short for its size is refused first. A PNG can't unpack to more than 1032 times its
        // compressed size (deflate's limit), so one whose header asks for more is refused too.
        const std::size_t pixelCount = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
        const std::size_t pixelsPerByte = data[0] == 'B' ? 128 : data[0] == 0xFF ? 256 : 0;
        if (pixelsPerByte != 0 && pixelCount / pixelsPerByte > data.size()) {
            return std::nullopt;
        }
        constexpr std::size_t kDeflateMostRatio = 1032;
        if (data[0] == 0x89 && pngRawBytes(data) / kDeflateMostRatio > data.size()) {
            return std::nullopt;
        }
        stbi_uc* pixels = stbi_load_from_memory(data.data(), static_cast<int>(data.size()), &w, &h, &n, 4);
        if (pixels == nullptr) {
            return std::nullopt;
        }
        out.width = static_cast<std::uint32_t>(w);
        out.height = static_cast<std::uint32_t>(h);
        out.rgba.assign(pixels, pixels + static_cast<std::size_t>(w) * h * 4);
        stbi_image_free(pixels);
    }
    const int w = static_cast<int>(out.width), h = static_cast<int>(out.height);
    if (data[0] == 0xFF) {
        const auto alpha = jpegAlpha(data, static_cast<std::size_t>(w) * h);
        for (std::size_t k = 0; k < alpha.size(); ++k) {
            out.rgba[k * 4 + 3] = alpha[k];
        }
    }
    return out;
}

} // namespace ts2::image
