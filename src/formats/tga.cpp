#include "formats/tga.hpp"

namespace ts2::tga {

std::optional<Pixels> decode(std::span<const std::uint8_t> data) {
    if (data.size() < 18) {
        return std::nullopt;
    }
    const std::uint8_t idLength = data[0], mapType = data[1], kind = data[2];
    const std::uint32_t width = static_cast<std::uint32_t>(data[12] | data[13] << 8);
    const std::uint32_t height = static_cast<std::uint32_t>(data[14] | data[15] << 8);
    const std::uint8_t bits = data[16], descriptor = data[17];
    const bool rle = kind == 10 || kind == 11;
    const bool gray = kind == 3 || kind == 11;
    if (mapType != 0 || !(kind == 2 || kind == 3 || rle) || width == 0 || height == 0 || width > kMaxSide ||
        height > kMaxSide) {
        return std::nullopt;
    }
    if (gray ? bits != 8 : (bits != 24 && bits != 32)) {
        return std::nullopt;
    }
    const std::size_t bytes = bits / 8u;
    const std::size_t count = static_cast<std::size_t>(width) * height;
    std::size_t at = 18u + idLength;
    if (at > data.size()) {
        return std::nullopt;
    }
    // Memory is set aside only for as many pixels as the data can hold: run-length packets turn at
    // least 1 + `bytes` bytes into at most 128 pixels.
    const std::size_t left = data.size() - at;
    const std::size_t room = rle ? (left / (1 + bytes) + 1) * 128 : left / bytes;
    if (count > room) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> raw;  // pixels in file order, `bytes` each
    raw.reserve(count * bytes);
    if (!rle) {
        raw.assign(data.begin() + static_cast<std::ptrdiff_t>(at), data.begin() + static_cast<std::ptrdiff_t>(at + count * bytes));
    } else {
        while (raw.size() < count * bytes) {
            if (at >= data.size()) {
                return std::nullopt;
            }
            const std::uint8_t packet = data[at++];
            const std::size_t n = (packet & 0x7Fu) + 1u;
            if (packet & 0x80) {  // one pixel repeated
                if (at + bytes > data.size()) {
                    return std::nullopt;
                }
                for (std::size_t k = 0; k < n; ++k) {
                    raw.insert(raw.end(), data.begin() + static_cast<std::ptrdiff_t>(at), data.begin() + static_cast<std::ptrdiff_t>(at + bytes));
                }
                at += bytes;
            } else {
                if (at + n * bytes > data.size()) {
                    return std::nullopt;
                }
                raw.insert(raw.end(), data.begin() + static_cast<std::ptrdiff_t>(at), data.begin() + static_cast<std::ptrdiff_t>(at + n * bytes));
                at += n * bytes;
            }
        }
        raw.resize(count * bytes);
    }
    Pixels out;
    out.width = width;
    out.height = height;
    out.rgba.resize(count * 4);
    const bool topFirst = (descriptor & 0x20) != 0;
    const bool rightFirst = (descriptor & 0x10) != 0;
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint32_t row = topFirst ? y : height - 1 - y;
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint32_t col = rightFirst ? width - 1 - x : x;
            const std::uint8_t* p = &raw[(static_cast<std::size_t>(y) * width + x) * bytes];
            std::uint8_t* q = &out.rgba[(static_cast<std::size_t>(row) * width + col) * 4];
            if (gray) {
                q[0] = q[1] = q[2] = p[0];
                q[3] = 255;
            } else {
                q[0] = p[2];
                q[1] = p[1];
                q[2] = p[0];
                q[3] = bytes == 4 ? p[3] : 255;
            }
        }
    }
    return out;
}

} // namespace ts2::tga
