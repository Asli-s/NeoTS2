#include "formats/reia.hpp"

#include <cstring>

namespace ts2::reia {

namespace {

constexpr std::uint32_t kBlock = 32;
constexpr std::uint32_t kBlockPixels = kBlock * kBlock;
constexpr std::size_t kPixelBytes = 3;  // blue, green, red
// Larger than any picture the game shows, so a broken file can't ask for huge amounts of memory.
constexpr std::uint32_t kMaxSide = 4096;

std::uint32_t u32(std::span<const std::uint8_t> d, std::size_t at) {
    return static_cast<std::uint32_t>(d[at]) | static_cast<std::uint32_t>(d[at + 1]) << 8 |
           static_cast<std::uint32_t>(d[at + 2]) << 16 | static_cast<std::uint32_t>(d[at + 3]) << 24;
}

// Where pixel n (0 to 1023, row by row) of block (bx, by) starts in the picture's RGBA bytes.
std::size_t pixelOffset(const tga::Pixels& picture, std::uint32_t bx, std::uint32_t by, std::uint32_t n) {
    const std::size_t x = bx * kBlock + n % kBlock;
    const std::size_t y = by * kBlock + n / kBlock;
    return (y * picture.width + x) * 4;
}

// Adds a coded pixel (blue, green, red) to the picture's pixel at `at`. Each pixel is coded as a
// change from the last frame, so the sums wrap around at 256 on purpose.
void addPixel(std::vector<std::uint8_t>& rgba, std::size_t at, std::span<const std::uint8_t> bgr) {
    rgba[at + 0] = static_cast<std::uint8_t>(rgba[at + 0] + bgr[2]);  // red
    rgba[at + 1] = static_cast<std::uint8_t>(rgba[at + 1] + bgr[1]);  // green
    rgba[at + 2] = static_cast<std::uint8_t>(rgba[at + 2] + bgr[0]);  // blue
}

// Decodes one changed block from data[p], moving p past it: runs of pixels until all 1024 are
// added. False when the data is broken.
bool decodeBlock(std::span<const std::uint8_t> data, std::size_t& p, tga::Pixels& picture, std::uint32_t bx,
                 std::uint32_t by) {
    std::uint32_t n = 0;
    while (n < kBlockPixels) {
        if (p >= data.size()) {
            return false;
        }
        const std::uint8_t c = data[p++];
        // Below 0x80: c + 1 pixels written out one after another. Otherwise one pixel, 257 - c times.
        const bool written = c < 0x80;
        const std::uint32_t count = written ? c + 1u : 257u - c;
        const std::size_t bytes = kPixelBytes * (written ? count : 1);
        if (n + count > kBlockPixels || bytes > data.size() - p) {
            return false;
        }
        for (std::uint32_t k = 0; k < count; ++k, ++n) {
            const auto pixel = data.subspan(p + (written ? kPixelBytes * k : 0), kPixelBytes);
            addPixel(picture.rgba, pixelOffset(picture, bx, by, n), pixel);
        }
        p += bytes;
    }
    return true;
}

} // namespace

Video parse(std::span<const std::uint8_t> file) {
    Video v;
    if (file.size() < 12 || std::memcmp(file.data(), "RIFF", 4) != 0 || std::memcmp(file.data() + 8, "Reia", 4) != 0) {
        return v;
    }
    for (std::size_t at = 12; at + 8 <= file.size();) {
        const std::uint32_t size = u32(file, at + 4);
        if (at + 8 + size > file.size()) {
            break;
        }
        const auto body = file.subspan(at + 8, size);
        if (std::memcmp(file.data() + at, "head", 4) == 0 && size >= 24) {
            v.width = u32(body, 4);
            v.height = u32(body, 8);
            v.rateNumerator = u32(body, 12);
            v.rateDenominator = u32(body, 16) ? u32(body, 16) : 1;
        } else if (std::memcmp(file.data() + at, "frme", 4) == 0) {
            v.frames.push_back(body);
        }
        at += 8 + size + (size & 1);
    }
    return v;
}

bool decodeFrame(const Video& video, std::size_t frame, tga::Pixels& picture) {
    if (frame >= video.frames.size() || video.width == 0 || video.height == 0 || video.width % kBlock ||
        video.height % kBlock || video.width > kMaxSide || video.height > kMaxSide) {
        return false;
    }
    // A picture of another size (or none yet) starts over as black.
    const std::size_t bytes = static_cast<std::size_t>(video.width) * video.height * 4;
    if (picture.width != video.width || picture.height != video.height || picture.rgba.size() != bytes) {
        picture.width = video.width;
        picture.height = video.height;
        picture.rgba.assign(bytes, 0);
        for (std::size_t i = 3; i < bytes; i += 4) {
            picture.rgba[i] = 255;
        }
    }
    const auto data = video.frames[frame];
    std::size_t p = 0;
    for (std::uint32_t by = 0; by < video.height / kBlock; ++by) {
        for (std::uint32_t bx = 0; bx < video.width / kBlock; ++bx) {
            if (p >= data.size()) {
                return false;
            }
            const bool changed = data[p++] != 0;
            if (changed && !decodeBlock(data, p, picture, bx, by)) {
                return false;
            }
        }
    }
    return p == data.size();
}

} // namespace ts2::reia
