#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ts2::tga {

// Decoded pixels, rows from the top, 4 bytes (RGBA) each.
struct Pixels {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
};

// A Targa image: true color (24 or 32 bits) or grayscale (8 bits), plain or run-length encoded.
// Empty for other kinds or broken data.
std::optional<Pixels> decode(std::span<const std::uint8_t> data);

} // namespace ts2::tga
