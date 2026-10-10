#include "formats/reia.hpp"

#include <cstring>

namespace ts2::reia {

namespace {

constexpr std::uint32_t kBlock = 32;

std::uint32_t u32(std::span<const std::uint8_t> d, std::size_t at) {
    return static_cast<std::uint32_t>(d[at]) | static_cast<std::uint32_t>(d[at + 1]) << 8 |
           static_cast<std::uint32_t>(d[at + 2]) << 16 | static_cast<std::uint32_t>(d[at + 3]) << 24;
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
    if (frame >= video.frames.size() || video.width % kBlock || video.height % kBlock) {
        return false;
    }
    if (picture.width != video.width || picture.height != video.height) {
        picture.width = video.width;
        picture.height = video.height;
        picture.rgba.assign(static_cast<std::size_t>(video.width) * video.height * 4, 0);
        for (std::size_t i = 3; i < picture.rgba.size(); i += 4) {
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
            if (data[p++] == 0) {
                continue;
            }
            std::uint32_t n = 0;
            auto put = [&](std::size_t from) {
                const std::uint32_t x = bx * kBlock + n % kBlock, y = by * kBlock + n / kBlock;
                std::uint8_t* q = &picture.rgba[(static_cast<std::size_t>(y) * video.width + x) * 4];
                q[0] = static_cast<std::uint8_t>(q[0] + data[from + 2]);
                q[1] = static_cast<std::uint8_t>(q[1] + data[from + 1]);
                q[2] = static_cast<std::uint8_t>(q[2] + data[from]);
                ++n;
            };
            while (n < kBlock * kBlock) {
                if (p >= data.size()) {
                    return false;
                }
                const std::uint8_t c = data[p++];
                const std::uint32_t count = c < 0x80 ? c + 1u : 257u - c;
                if (n + count > kBlock * kBlock || p + (c < 0x80 ? 3u * count : 3u) > data.size()) {
                    return false;
                }
                for (std::uint32_t k = 0; k < count; ++k) {
                    put(c < 0x80 ? p + 3 * k : p);
                }
                p += c < 0x80 ? 3 * count : 3;
            }
        }
    }
    return p == data.size();
}

} // namespace ts2::reia
